/*
 * Internet radio for the Waveshare ESP32-S3-Touch-AMOLED-2.16.
 * GUI: Arduino_GFX + Arduino_Canvas, CST9220 touch, AXP2101 PMU,
 * ES8311 audio codec + PA_CTRL amplifier, streams via ESP32-audioI2S.
 *
 * IMPORTANT:
 *  - Use the libraries supplied with the official Waveshare example package for this board.
 *  - Copy es8311.c, es8311.h and es8311_reg.h into this sketch folder from the official
 *    07_ES8311 example. If the files are in a different folder in your package, use the
 *    matching ES8311 driver sources from that same package.
 *  - Audio.h comes from the ESP32-audioI2S library.
 *  - Enter your Wi-Fi credentials below.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include "pin_config.h"
#include <U8g2lib.h>             // U8g2 fonts with extended UTF-8 character support
#include <Arduino_GFX_Library.h>
#include "TouchDrvCST.hpp"

#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>

#include <Audio.h>                 // ESP32-audioI2S
#include "es8311.h"                // from the official Waveshare 07_ES8311 example
#include <driver/i2c.h>
#include <esp_err.h>

// -------------------- Wi-Fi configuration --------------------
static const char *WIFI_SSID = "YOUR_WIFI_NAME";
static const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// -------------------- Radio presets --------------------
struct RadioStation {
  const char *name;
  const char *url;
};

static const RadioStation STATIONS[] = {
  {"Radio Paradise", "http://stream-uk1.radioparadise.com/mp3-128"},
  {"Groove Salad",   "https://ice5.somafm.com/groovesalad-128-mp3"},
  {"Drone Zone",     "https://ice5.somafm.com/dronezone-128-mp3"},
  {"Power 181",      "http://listen.181fm.com/181-power_128k.mp3"}
};
static constexpr int STATION_COUNT = sizeof(STATIONS) / sizeof(STATIONS[0]);

// The ES8311 codec is configured for 44.1 kHz. If you use streams with a different
// sample rate and the codec does not behave correctly, adjust this value according
// to the Waveshare example and the selected stream.
static constexpr uint32_t AUDIO_SAMPLE_RATE = 44100;

// -------------------- RGB565 colors --------------------
static constexpr uint16_t C_BG      = 0x0861;
static constexpr uint16_t C_PANEL   = 0x10A3;
static constexpr uint16_t C_PANEL2  = 0x18E5;
static constexpr uint16_t C_TEXT    = 0xFFFF;
static constexpr uint16_t C_MUTED   = 0x9CF3;
static constexpr uint16_t C_CYAN    = 0x07FF;
static constexpr uint16_t C_GREEN   = 0x07E0;
static constexpr uint16_t C_ORANGE  = 0xFD20;
static constexpr uint16_t C_RED     = 0xF800;
static constexpr uint16_t C_TRACK   = 0x4208;

// -------------------- Hardware objects --------------------
Arduino_DataBus *displayBus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

Arduino_CO5300 *panel = new Arduino_CO5300(
  displayBus, LCD_RESET, 0, LCD_WIDTH, LCD_HEIGHT);

// The CO5300 display is rendered through a canvas, which is more reliable than issuing
// individual drawing operations directly to the QSPI display.
Arduino_GFX *gfx = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, panel);

TouchDrvCST92xx touch;
XPowersPMU power;
Audio audio;
Preferences prefs;

// -------------------- Shared data between the audio task and GUI --------------------
static portMUX_TYPE textMux = portMUX_INITIALIZER_UNLOCKED;
static char streamTitle[100] = "";             // actual track title from ICY metadata
static char reportedStation[72] = "";          // station name from the HTTP/ICY header
static char audioInfoText[72] = "Ready";
static volatile uint32_t streamRequestStartedAt = 0;
static char marqueeTitleSnapshot[sizeof(streamTitle)] = "";
static uint32_t marqueeTitleStartedAt = 0;

static QueueHandle_t audioQueue = nullptr;
enum AudioCommandType : uint8_t { CMD_PLAY_STATION, CMD_TOGGLE_PAUSE, CMD_STOP, CMD_SET_VOLUME };
struct AudioCommand {
  AudioCommandType type;
  int value;
};

static bool touchAvailable = false;
static bool pmuAvailable = false;
static bool isPlaying = false;
static bool hasStream = false;
static int selectedStation = 0;
static int volumePercent = 45;      // 0..100; the audio library uses 0..21
static int brightnessPercent = 75;  // 5..100
static int batteryPercent = -1;
static bool isCharging = false;
static bool isBatteryConnected = false;
static uint32_t lastBatteryRead = 0;
static uint32_t lastTouchRead = 0;
static uint32_t lastPowerKeyPoll = 0;
static uint32_t lastDraw = 0;
static uint32_t lastMarqueeDraw = 0;
static bool displayIsOn = true;
static bool audioMuted = false;
static volatile bool uiDirty = true;
static bool wasTouched = false;
static bool settingsChangedDuringTouch = false;
static bool userKeyRawState = HIGH;
static bool userKeyStableState = HIGH;
static uint32_t userKeyLastChange = 0;
static constexpr uint32_t USER_KEY_DEBOUNCE_MS = 35;

static int16_t touchX[1] = {0};
static int16_t touchY[1] = {0};

// -------------------- Helper functions --------------------
static void copySharedText(char *dst, size_t dstSize, const char *src) {
  if (!dst || dstSize == 0) return;
  portENTER_CRITICAL(&textMux);
  strlcpy(dst, src ? src : "", dstSize);
  portEXIT_CRITICAL(&textMux);
}

static void readSharedText(char *dst, size_t dstSize, const char *src) {
  if (!dst || dstSize == 0) return;
  portENTER_CRITICAL(&textMux);
  strlcpy(dst, src, dstSize);
  portEXIT_CRITICAL(&textMux);
}

static void queueAudioCommand(AudioCommandType type, int value = 0) {
  if (!audioQueue) return;
  AudioCommand cmd{type, value};
  // The GUI must not wait for the audio task. If the queue is full, the command is dropped;
  // a later touch will usually submit the current value again.
  xQueueSend(audioQueue, &cmd, 0);
}

static int audioVolumeFromPercent(int pct) {
  pct = constrain(pct, 0, 100);
  return map(pct, 0, 100, 0, 21);
}

static uint8_t displayBrightnessFromPercent(int pct) {
  pct = constrain(pct, 5, 100);
  return (uint8_t)map(pct, 5, 100, 12, 255);
}

static void setDisplayBrightness() {
  if (panel) panel->setBrightness(displayBrightnessFromPercent(brightnessPercent));
}

// PWR is connected to the AXP2101, not to a GPIO. A short press turns off only the
// CO5300 panel; the audio task, Wi-Fi and playback continue running.
static void toggleDisplayPower() {
  if (!panel) return;

  if (displayIsOn) {
    panel->displayOff();
    displayIsOn = false;
    Serial.println("PWR: display off");
  } else {
    panel->displayOn();
    setDisplayBrightness();
    displayIsOn = true;
    uiDirty = true;
    lastDraw = 0;  // redraw the entire canvas after waking the display
    Serial.println("PWR: display on");
  }
}

static void toggleAudioMute() {
  audioMuted = !audioMuted;
  const int targetVolume = audioMuted ? 0 : audioVolumeFromPercent(volumePercent);
  queueAudioCommand(CMD_SET_VOLUME, targetVolume);
  Serial.printf("+/KEY: audio %s (configured volume %d%%)\n",
                audioMuted ? "muted" : "restored", volumePercent);
  uiDirty = true;
}

// GPIO18 (Key3, the +/KEY button) is active-low.
// Debouncing handles only a new press, not the button release.
static void pollUserKey() {
  const bool rawState = digitalRead(USER_KEY_PIN);
  const uint32_t now = millis();

  if (rawState != userKeyRawState) {
    userKeyRawState = rawState;
    userKeyLastChange = now;
  }

  if ((now - userKeyLastChange) >= USER_KEY_DEBOUNCE_MS &&
      userKeyStableState != userKeyRawState) {
    userKeyStableState = userKeyRawState;
    if (userKeyStableState == LOW) {
      toggleAudioMute();
    }
  }
}

static void pollPowerButton() {
  // PWR is an AXP2101 input. XPowersLib must first read the IRQ status registers;
  // isPekeyShortPressIrq() checks the status that has already been read.
  if (!pmuAvailable || millis() - lastPowerKeyPoll < 50) return;
  lastPowerKeyPoll = millis();

  const uint32_t irqStatus = power.getIrqStatus();
  if (power.isPekeyShortPressIrq()) {
    // Clear the flags before redrawing so the same press is not handled again.
    power.clearIrqStatus();
    Serial.printf("PWR: short press detected (AXP2101 IRQ=0x%08lX)\n",
                  (unsigned long)irqStatus);
    toggleDisplayPower();
  } else if (irqStatus != 0) {
    // Other PMU flags are not needed by this radio; clear them so
    // subsequent polling does not process the same events repeatedly.
    power.clearIrqStatus();
  }
}

static void setTextAt(const char *text, int x, int y, uint16_t color, uint8_t size = 1) {
  gfx->setTextWrap(false);
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  gfx->setCursor(x, y);
  gfx->print(text ? text : "");
}

static void setCenteredText(const char *text, int centerX, int y, uint16_t color, uint8_t size = 1) {
  const int approxWidth = (int)strlen(text ? text : "") * 6 * size;
  setTextAt(text, centerX - approxWidth / 2, y, color, size);
}

// Center text and shorten long metadata so it does not overflow the display
// when using a larger font. The text occupies at most maxWidth pixels.
static void setCenteredTextFitted(const char *text, int centerX, int y, int maxWidth,
                                  uint16_t color, uint8_t size) {
  const char *source = text ? text : "";
  const int maxChars = maxWidth / (6 * size);
  if ((int)strlen(source) <= maxChars) {
    setCenteredText(source, centerX, y, color, size);
    return;
  }
  char fitted[100];
  const int copyChars = maxChars >= 3 ? maxChars - 3 : maxChars;
  const size_t n = (size_t)constrain(copyChars, 0, (int)sizeof(fitted) - 1);
  memcpy(fitted, source, n);
  fitted[n] = '\0';
  if (maxChars >= 3) strcat(fitted, "...");
  setCenteredText(fitted, centerX, y, color, size);
}

// Stream metadata uses UTF-8. The built-in Arduino_GFX font supports only basic ASCII,
// so U8g2 Unifont is used for track titles to provide an extended Unicode character set
// (32-701), including accented Latin characters.
// Latin characters in this font use a fixed 8 px cell, so the title width can be calculated
// by Unicode code point without splitting a UTF-8 character.
static void trimIncompleteUtf8Tail(char *text) {
  size_t len = strlen(text);
  if (!len) return;

  size_t start = len - 1;
  while (start > 0 && (((uint8_t)text[start] & 0xC0) == 0x80)) --start;

  const uint8_t lead = (uint8_t)text[start];
  size_t expected = 1;
  if ((lead & 0xE0) == 0xC0) expected = 2;
  else if ((lead & 0xF0) == 0xE0) expected = 3;
  else if ((lead & 0xF8) == 0xF0) expected = 4;

  if (len - start < expected) text[start] = '\0';
}

static void removeLastUtf8Codepoint(char *text) {
  size_t len = strlen(text);
  if (!len) return;
  size_t start = len - 1;
  while (start > 0 && (((uint8_t)text[start] & 0xC0) == 0x80)) --start;
  text[start] = '\0';
}

static size_t utf8CodepointCount(const char *text) {
  size_t count = 0;
  for (const uint8_t *p = (const uint8_t *)text; *p; ++p) {
    // UTF-8 continuation bytes start with the bit pattern 10; other bytes start a code point.
    if ((*p & 0xC0) != 0x80) ++count;
  }
  return count;
}

static uint16_t utf8TextWidth(const char *text, uint8_t textScale) {
  // Unifont uses an 8 px cell for common Latin characters; Arduino_GFX scales it
  // with setTextSize(), just like the built-in font.
  return (uint16_t)(utf8CodepointCount(text) * 8 * textScale);
}

// Draw long UTF-8 text as a horizontal marquee. Each character is drawn
// separately so its byte range can be identified safely; this prevents truncation
// in the middle of a UTF-8 character. Unifont uses a fixed cell width for this character set:
// 8 px (16 px at scale 2).
static void drawUtf8MarqueeCopy(const char *text, int startX, int topY,
                                int clipX, int clipWidth, uint16_t color,
                                uint8_t textScale) {
  const int cellWidth = 8 * textScale;
  const int clipRight = clipX + clipWidth;
  int charIndex = 0;
  const uint8_t *p = (const uint8_t *)(text ? text : "");

  gfx->setFont(u8g2_font_unifont_te);
  gfx->setUTF8Print(true);
  gfx->setTextSize(textScale);
  gfx->setTextWrap(false);
  gfx->setTextColor(color);

  while (*p) {
    size_t bytes = 1;
    if ((*p & 0xE0) == 0xC0) bytes = 2;
    else if ((*p & 0xF0) == 0xE0) bytes = 3;
    else if ((*p & 0xF8) == 0xF0) bytes = 4;

    // Do not read past the terminating NUL if malformed UTF-8 is encountered.
    size_t available = 0;
    while (available < bytes && p[available] != 0) ++available;
    if (available < bytes) break;

    const int x = startX + charIndex * cellWidth;
    // Arduino_GFX does not expose a common clipRect API for all backends; draw only
    // characters that overlap the area reserved for the metadata line.
    if (x < clipRight && x + cellWidth > clipX) {
      char glyph[5] = {0, 0, 0, 0, 0};
      memcpy(glyph, p, bytes);
      gfx->setCursor(x, topY + 16 * textScale);
      gfx->print(glyph);
    }
    p += bytes;
    ++charIndex;
  }

  gfx->setUTF8Print(false);
  gfx->setFont();
  gfx->setTextSize(1);
}

static void drawScrollingUtf8Title(const char *text, int leftX, int topY,
                                   int maxWidth, uint16_t color, uint8_t textScale,
                                   uint32_t elapsedMs) {
  const uint16_t titleWidth = utf8TextWidth(text, textScale);
  const int gapWidth = 3 * 8 * textScale;
  const int cycleDistance = (int)titleWidth + gapWidth;
  constexpr uint32_t START_HOLD_MS = 900;
  constexpr uint32_t SCROLL_SPEED_PX_PER_SEC = 28;
  const uint32_t travelMs = (uint32_t)cycleDistance * 1000UL / SCROLL_SPEED_PX_PER_SEC;
  const uint32_t cycleMs = START_HOLD_MS + (travelMs ? travelMs : 1);
  const uint32_t phase = elapsedMs % cycleMs;
  const int offset = phase < START_HOLD_MS ? 0 :
      (int)((phase - START_HOLD_MS) * SCROLL_SPEED_PX_PER_SEC / 1000UL);

  // Clear only the metadata strip; drawUi also redraws the full canvas. Two copies
  // provide a seamless transition back to the beginning after the full title scrolls by.
  gfx->fillRect(leftX, topY, maxWidth, 16 * textScale + 4, C_BG);
  drawUtf8MarqueeCopy(text, leftX - offset, topY, leftX, maxWidth, color, textScale);
  drawUtf8MarqueeCopy(text, leftX - offset + cycleDistance, topY,
                      leftX, maxWidth, color, textScale);
}

static bool streamTitleNeedsMarquee() {
  char title[sizeof(streamTitle)];
  readSharedText(title, sizeof(title), streamTitle);
  trimIncompleteUtf8Tail(title);
  return title[0] != '\0' && utf8TextWidth(title, 2) > 448;
}

static void setCenteredUtf8TextFitted(const char *text, int centerX, int topY,
                                      int maxWidth, uint16_t color, uint8_t textScale = 1) {
  if (textScale < 1) textScale = 1;
  const char *source = text ? text : "";
  char fitted[sizeof(streamTitle) + 1];
  strlcpy(fitted, source, sizeof(fitted));
  trimIncompleteUtf8Tail(fitted); // the copy must not end in the middle of a UTF-8 character
  bool shortened = strlen(fitted) < strlen(source);

  // Important: enable the Arduino_GFX UTF-8 decoder only when using a U8g2 font.
  gfx->setFont(u8g2_font_unifont_te);
  gfx->setUTF8Print(true);
  gfx->setTextSize(textScale);
  gfx->setTextWrap(false);
  gfx->setTextColor(color);

  while (fitted[0] && utf8TextWidth(fitted, textScale) > maxWidth) {
    removeLastUtf8Codepoint(fitted);
    shortened = true;
  }

  if (shortened) {
    while (fitted[0]) {
      char withEllipsis[sizeof(fitted)];
      snprintf(withEllipsis, sizeof(withEllipsis), "%s...", fitted);
      if (utf8TextWidth(withEllipsis, textScale) <= maxWidth) {
        strlcpy(fitted, withEllipsis, sizeof(fitted));
        break;
      }
      removeLastUtf8Codepoint(fitted);
    }
  }

  const uint16_t width = utf8TextWidth(fitted, textScale);
  // Unifont is 16 px high; textScale increases both the height and width of each character cell.
  gfx->setCursor(centerX - (int)width / 2, topY + 16 * textScale);
  gfx->print(fitted);

  gfx->setUTF8Print(false);
  gfx->setFont(); // restore the built-in font for the rest of the GUI
  gfx->setTextSize(1);
}

// Larger controls for the 480x480 display: a wider slider track, thicker bar
// and larger knob. Drawing and touch handling share the geometry below.
static constexpr int SLIDER_X = 128;
static constexpr int SLIDER_WIDTH = 328;
static constexpr int SLIDER_HIT_X_MIN = 112;
static constexpr int SLIDER_HIT_X_MAX = 472;
static constexpr int SLIDER_HIT_HALF_HEIGHT = 22;
static constexpr int VOLUME_SLIDER_Y = 350;
static constexpr int BRIGHTNESS_SLIDER_Y = 419;

// A 2x2 layout for four stations allows labels to use a larger font.
static constexpr int STATION_BTN_X0 = 14;
static constexpr int STATION_BTN_X_STEP = 228;
static constexpr int STATION_BTN_WIDTH = 220;
static constexpr int STATION_BTN_Y0 = 157;
static constexpr int STATION_BTN_Y_STEP = 46;
static constexpr int STATION_BTN_HEIGHT = 40;

static void drawSlider(int x, int y, int width, int percent) {
  percent = constrain(percent, 0, 100);
  gfx->fillRoundRect(x, y - 7, width, 14, 7, C_TRACK);
  const int filled = (width * percent) / 100;
  if (filled > 0) gfx->fillRoundRect(x, y - 7, filled, 14, 7, C_ORANGE);
  const int knobX = x + filled;
  gfx->fillCircle(knobX, y, 13, C_TEXT);
  gfx->drawCircle(knobX, y, 13, C_ORANGE);
}

static void drawUi() {
  char titleLocal[sizeof(streamTitle)];
  char stationLocal[sizeof(reportedStation)];
  char infoLocal[sizeof(audioInfoText)];
  readSharedText(titleLocal, sizeof(titleLocal), streamTitle);
  readSharedText(stationLocal, sizeof(stationLocal), reportedStation);
  readSharedText(infoLocal, sizeof(infoLocal), audioInfoText);
  trimIncompleteUtf8Tail(titleLocal);

  // Reset marquee clock whenever metadata changes, including when it clears.
  if (strcmp(titleLocal, marqueeTitleSnapshot) != 0) {
    strlcpy(marqueeTitleSnapshot, titleLocal, sizeof(marqueeTitleSnapshot));
    marqueeTitleStartedAt = millis();
  }

  gfx->fillScreen(C_BG);

  // Top status row: larger text with controls arranged to avoid overlap.
  setTextAt("AMOLED RADIO", 27, 14, C_CYAN, 2);
  const bool wifiOk = WiFi.status() == WL_CONNECTED;
  setTextAt(wifiOk ? "WiFi OK" : "WiFi OFF", 214, 14,
            wifiOk ? C_GREEN : C_ORANGE, 2);

  if (batteryPercent >= 0 && isBatteryConnected) {
    char bat[12];
    snprintf(bat, sizeof(bat), "%d%%", batteryPercent);
    setTextAt(bat, 322, 14, batteryPercent <= 15 ? C_ORANGE : C_TEXT, 2);
    gfx->drawRect(395, 13, 38, 20, C_TEXT);
    gfx->fillRect(433, 18, 4, 10, C_TEXT);
    int barWidth = map(constrain(batteryPercent, 0, 100), 0, 100, 0, 32);
    if (barWidth > 0) gfx->fillRect(398, 16, barWidth, 14,
      batteryPercent <= 15 ? C_ORANGE : (isCharging ? C_GREEN : C_CYAN));
    if (isCharging) setTextAt("+", 376, 14, C_GREEN, 2);
  } else {
    setTextAt("USB", 340, 14, C_TEXT, 2);
    gfx->drawRect(395, 13, 38, 20, C_TEXT);
    gfx->fillRect(433, 18, 4, 10, C_TEXT);
  }
  gfx->drawFastHLine(16, 36, 448, C_PANEL2);

  // The upper status area is compact; track titles use a UTF-8 font.
  // Short metadata stays centered; longer titles scroll horizontally.
  setCenteredUtf8TextFitted(STATIONS[selectedStation].name, 240, 30, 448, C_TEXT, 2);
  setCenteredUtf8TextFitted("NOW PLAYING", 240, 64, 448, C_MUTED, 2);
  if (strlen(titleLocal) > 0) {
    if (utf8TextWidth(titleLocal, 2) > 448) {
      drawScrollingUtf8Title(titleLocal, 16, 96, 448, C_CYAN, 2,
                             millis() - marqueeTitleStartedAt);
    } else {
      setCenteredUtf8TextFitted(titleLocal, 240, 96, 448, C_CYAN, 2);
    }
  } else if (!hasStream) {
    setCenteredUtf8TextFitted("Choose a station", 240, 96, 448, C_CYAN, 2);
  } else if (streamRequestStartedAt != 0 && millis() - streamRequestStartedAt < 20000) {
    setCenteredUtf8TextFitted("Waiting for track...", 240, 96, 448, C_CYAN, 2);
  } else {
    setCenteredUtf8TextFitted("Station sends no title", 240, 96, 448, C_MUTED, 2);
  }

  char statusLine[80];
  if (strlen(infoLocal) > 0) strlcpy(statusLine, infoLocal, sizeof(statusLine));
  else strlcpy(statusLine, isPlaying ? "Playing" : "Paused", sizeof(statusLine));
  setCenteredUtf8TextFitted(statusLine, 240, 131, 448, C_MUTED, 1);

  // Station presets: two columns by two rows, shifted down to make room for larger metadata.
  for (int i = 0; i < STATION_COUNT; ++i) {
    const int col = i % 2;
    const int row = i / 2;
    const int x = STATION_BTN_X0 + col * STATION_BTN_X_STEP;
    const int y = STATION_BTN_Y0 + row * STATION_BTN_Y_STEP;
    const bool active = (i == selectedStation);
    gfx->fillRoundRect(x, y, STATION_BTN_WIDTH, STATION_BTN_HEIGHT, 10,
                       active ? C_ORANGE : C_PANEL);
    gfx->drawRoundRect(x, y, STATION_BTN_WIDTH, STATION_BTN_HEIGHT, 10,
                       active ? C_TEXT : C_PANEL2);
    setCenteredUtf8TextFitted(STATIONS[i].name, x + STATION_BTN_WIDTH / 2,
                          y, STATION_BTN_WIDTH - 16,
                          active ? C_BG : C_TEXT, 2);
  }

  // Main transport buttons use larger, easier-to-read labels.
  gfx->fillRoundRect(110, 251, 124, 52, 12, isPlaying ? C_GREEN : C_PANEL);
  gfx->drawRoundRect(110, 251, 124, 52, 12, C_TEXT);
  setCenteredText(isPlaying ? "PAUSE" : "PLAY", 172, 265, isPlaying ? C_BG : C_TEXT, 3);

  gfx->fillRoundRect(246, 251, 124, 52, 12, C_PANEL);
  gfx->drawRoundRect(246, 251, 124, 52, 12, C_PANEL2);
  setCenteredText("STOP", 308, 265, C_TEXT, 3);

  // Volume and brightness labels use text size 2 and have separate touch zones.
  char volLabel[40];
  if (audioMuted) snprintf(volLabel, sizeof(volLabel), "VOLUME %d%% - MUTED", volumePercent);
  else snprintf(volLabel, sizeof(volLabel), "VOLUME %d%%", volumePercent);
  setTextAt(volLabel, 25, 312, C_TEXT, 2);
  drawSlider(SLIDER_X, VOLUME_SLIDER_Y, SLIDER_WIDTH, volumePercent);

  char brightLabel[24];
  snprintf(brightLabel, sizeof(brightLabel), "BRIGHTNESS %d%%", brightnessPercent);
  setTextAt(brightLabel, 25, 381, C_TEXT, 2);
  drawSlider(SLIDER_X, BRIGHTNESS_SLIDER_Y, SLIDER_WIDTH, brightnessPercent);

  setCenteredText("+/KEY: MUTE / UNMUTE", 240, 452, C_MUTED, 2);
  gfx->flush();
  lastDraw = millis();
  uiDirty = false;
}

static bool initCodec() {
  es8311_handle_t handle = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
  if (!handle) {
    Serial.println("ES8311: failed to create handle");
    return false;
  }

  const es8311_clock_config_t clockConfig = {
    .mclk_inverted = false,
    .sclk_inverted = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency = AUDIO_SAMPLE_RATE * 256,
    .sample_frequency = AUDIO_SAMPLE_RATE
  };

  if (es8311_init(handle, &clockConfig, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16) != ESP_OK) {
    Serial.println("ES8311: es8311_init failed");
    return false;
  }
  if (es8311_sample_frequency_config(handle, clockConfig.mclk_frequency,
                                      clockConfig.sample_frequency) != ESP_OK) {
    Serial.println("ES8311: sample-rate configuration failed");
    return false;
  }
  // The radio does not use the microphone input; set the DAC volume to a fixed level.
  es8311_microphone_config(handle, false);
  if (es8311_voice_volume_set(handle, 82, nullptr) != ESP_OK) {
    Serial.println("ES8311: codec volume configuration failed");
    return false;
  }
  return true;
}

static void audioWorker(void *param) {
  (void)param;
  audio.setPinout(BCLKPIN, WSPIN, DOPIN, MCLKPIN);
  audio.setOutputSampleRate(Audio::SR_44100);  // matches the ES8311 configuration below
  audio.setVolumeSteps(21);
  audio.setVolume(audioVolumeFromPercent(volumePercent));
  copySharedText(audioInfoText, sizeof(audioInfoText), "Audio ready");

  uint32_t lastReconnect = 0;
  for (;;) {
    AudioCommand cmd;
    while (audioQueue && xQueueReceive(audioQueue, &cmd, 0) == pdTRUE) {
      switch (cmd.type) {
        case CMD_PLAY_STATION:
          if (cmd.value >= 0 && cmd.value < STATION_COUNT) {
            copySharedText(audioInfoText, sizeof(audioInfoText), "Connecting to stream...");
            copySharedText(streamTitle, sizeof(streamTitle), "");
            copySharedText(reportedStation, sizeof(reportedStation), "");
            if (streamRequestStartedAt == 0) streamRequestStartedAt = millis();
            audio.stopSong();
            audio.connecttohost(STATIONS[cmd.value].url);
          }
          break;
        case CMD_TOGGLE_PAUSE:
          audio.pauseResume();
          break;
        case CMD_STOP:
          audio.stopSong();
          copySharedText(streamTitle, sizeof(streamTitle), "");
          copySharedText(reportedStation, sizeof(reportedStation), "");
          streamRequestStartedAt = 0;
          copySharedText(audioInfoText, sizeof(audioInfoText), "Stopped");
          break;
        case CMD_SET_VOLUME:
          audio.setVolume(constrain(cmd.value, 0, 21));
          break;
      }
    }

    // If Wi-Fi drops, the ESP periodically attempts to reconnect.
    if (WiFi.status() != WL_CONNECTED && millis() - lastReconnect > 10000) {
      lastReconnect = millis();
      WiFi.reconnect();
    }
    audio.loop();
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

// -------------------- ESP32-audioI2S library callbacks --------------------
// ESP32-audioI2S 3.x uses Audio::audio_info_callback and typed events.
// In particular, evt_streamtitle contains the current track title from ICY metadata.
static void audioEventCallback(Audio::msg_t msg) {
  switch (msg.e) {
    case Audio::evt_streamtitle:
      copySharedText(streamTitle, sizeof(streamTitle), msg.msg);
      uiDirty = true;
      if (msg.msg && msg.msg[0]) {
        Serial.printf("NOW PLAYING: %s\n", msg.msg);
      }
      break;
    case Audio::evt_name:
      copySharedText(reportedStation, sizeof(reportedStation), msg.msg);
      break;
    default:
      break;
  }
}

// Backward compatibility with older ESP32-audioI2S releases that used
// global weak callbacks. Version 3.x uses the handler above.
void audio_info(const char *info) {
  copySharedText(audioInfoText, sizeof(audioInfoText), info);
}

void audio_showstation(const char *info) {
  copySharedText(reportedStation, sizeof(reportedStation), info);
}

void audio_showstreamtitle(const char *info) {
  copySharedText(streamTitle, sizeof(streamTitle), info);
  uiDirty = true;
  if (info && info[0]) Serial.printf("NOW PLAYING: %s\n", info);
}

void audio_bitrate(const char *info) {
  // Bitrate metadata is intentionally ignored so the status text does not change too often.
  (void)info;
}

// -------------------- Power and battery --------------------
static void readBattery() {
  if (!pmuAvailable) return;
  isBatteryConnected = power.isBatteryConnect();
  isCharging = power.isCharging();
  if (isBatteryConnected) {
    batteryPercent = constrain((int)power.getBatteryPercent(), 0, 100);
  } else {
    batteryPercent = -1;
  }
}

static void saveSettings() {
  prefs.begin("radio216", false);
  prefs.putUChar("volume", (uint8_t)volumePercent);
  prefs.putUChar("brightness", (uint8_t)brightnessPercent);
  prefs.putUChar("station", (uint8_t)selectedStation);
  prefs.end();
}

static void updateVolumeFromTouch(int x) {
  const int pct = constrain(map(x, SLIDER_X, SLIDER_X + SLIDER_WIDTH, 0, 100), 0, 100);
  const bool volumeChanged = pct != volumePercent;
  if (volumeChanged) {
    volumePercent = pct;
    settingsChangedDuringTouch = true;
  }

  // Intentionally using the volume slider cancels mute even if the knob did not move.
  if (audioMuted || volumeChanged) {
    audioMuted = false;
    queueAudioCommand(CMD_SET_VOLUME, audioVolumeFromPercent(volumePercent));
    uiDirty = true;
  }
}

static void updateBrightnessFromTouch(int x) {
  const int pct = constrain(map(x, SLIDER_X, SLIDER_X + SLIDER_WIDTH, 5, 100), 5, 100);
  if (pct != brightnessPercent) {
    brightnessPercent = pct;
    setDisplayBrightness();
    settingsChangedDuringTouch = true;
    uiDirty = true;
  }
}

static void handleTouch() {
  if (!touchAvailable || millis() - lastTouchRead < 22) return;
  lastTouchRead = millis();

  const uint8_t n = touch.getPoint(touchX, touchY, 1);
  const bool touched = n > 0;

  // Ignore touch while the display is off so hidden controls cannot be
  // activated accidentally. Releasing a touch still saves any pending changes.
  if (!displayIsOn) {
    if (!touched && wasTouched && settingsChangedDuringTouch) saveSettings();
    if (!touched) settingsChangedDuringTouch = false;
    wasTouched = touched;
    return;
  }

  if (touched) {
    int x = constrain((int)touchX[0], 0, LCD_WIDTH - 1);
    int y = constrain((int)touchY[0], 0, LCD_HEIGHT - 1);

    // Sliders can be changed by dragging, not just by tapping once.
    if (y >= VOLUME_SLIDER_Y - SLIDER_HIT_HALF_HEIGHT &&
        y <= VOLUME_SLIDER_Y + SLIDER_HIT_HALF_HEIGHT &&
        x >= SLIDER_HIT_X_MIN && x <= SLIDER_HIT_X_MAX) {
      updateVolumeFromTouch(x);
    } else if (y >= BRIGHTNESS_SLIDER_Y - SLIDER_HIT_HALF_HEIGHT &&
               y <= BRIGHTNESS_SLIDER_Y + SLIDER_HIT_HALF_HEIGHT &&
               x >= SLIDER_HIT_X_MIN && x <= SLIDER_HIT_X_MAX) {
      updateBrightnessFromTouch(x);
    } else if (!wasTouched) {
      // Station presets in the 2x2 grid.
      bool stationHit = false;
      for (int i = 0; i < STATION_COUNT; ++i) {
        const int col = i % 2;
        const int row = i / 2;
        const int left = STATION_BTN_X0 + col * STATION_BTN_X_STEP;
        const int top = STATION_BTN_Y0 + row * STATION_BTN_Y_STEP;
        if (x >= left && x <= left + STATION_BTN_WIDTH &&
            y >= top && y <= top + STATION_BTN_HEIGHT) {
          selectedStation = i;
          hasStream = true;
          isPlaying = true;
          streamRequestStartedAt = millis();
          copySharedText(streamTitle, sizeof(streamTitle), "");
          queueAudioCommand(CMD_PLAY_STATION, selectedStation);
          settingsChangedDuringTouch = true;
          uiDirty = true;
          stationHit = true;
          break;
        }
      }
      if (!stationHit && y >= 247 && y <= 307 && x >= 104 && x <= 236) {
        // Play/Pause
        if (!hasStream) {
          hasStream = true;
          isPlaying = true;
          streamRequestStartedAt = millis();
          copySharedText(streamTitle, sizeof(streamTitle), "");
          queueAudioCommand(CMD_PLAY_STATION, selectedStation);
        } else {
          isPlaying = !isPlaying;
          queueAudioCommand(CMD_TOGGLE_PAUSE);
        }
        uiDirty = true;
      } else if (!stationHit && y >= 247 && y <= 307 && x >= 240 && x <= 376) {
        // Stop
        isPlaying = false;
        hasStream = false;
        streamRequestStartedAt = 0;
        copySharedText(streamTitle, sizeof(streamTitle), "");
        queueAudioCommand(CMD_STOP);
        uiDirty = true;
      }
    }
  } else if (wasTouched) {
    if (settingsChangedDuringTouch) saveSettings();
    settingsChangedDuringTouch = false;
  }

  wasTouched = touched;
}

void setup() {
  Serial.begin(115200);
  // Required for metadata with ESP32-audioI2S 3.x. Without registering this handler,
  // evt_streamtitle is not delivered to the application and the waiting text remains on screen.
  Audio::audio_info_callback = audioEventCallback;
  delay(250);
  Serial.println();
  Serial.println("ESP32-S3-Touch-AMOLED-2.16 Internet Radio");

  pinMode(USER_KEY_PIN, INPUT_PULLUP);
  userKeyRawState = userKeyStableState = digitalRead(USER_KEY_PIN);
  userKeyLastChange = millis();
  Serial.println("GPIO18 +/KEY ready (short press toggles audio mute)");

  Wire.begin(IIC_SDA, IIC_SCL);
  Wire.setClock(400000);

  // Display and framebuffer
  if (!gfx->begin()) {
    Serial.println("ERROR: gfx->begin() failed");
  }
  gfx->fillScreen(C_BG);
  gfx->flush();

  // Load saved settings
  prefs.begin("radio216", true);
  volumePercent = constrain((int)prefs.getUChar("volume", 45), 0, 100);
  brightnessPercent = constrain((int)prefs.getUChar("brightness", 75), 5, 100);
  selectedStation = constrain((int)prefs.getUChar("station", 0), 0, STATION_COUNT - 1);
  prefs.end();
  setDisplayBrightness();

  // CST9220 touch controller
  touch.setPins(TP_RESET, TP_INT);
  touchAvailable = touch.begin(Wire, CST92XX_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (!touchAvailable) Serial.println("WARNING: CST9220 initialization failed; check the libraries and pin configuration.");
  else Serial.println("CST9220 touch ready");

  // AXP2101 PMU: battery and VBUS monitoring
  pmuAvailable = power.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (pmuAvailable) {
    power.enableBattDetection();
    power.enableBattVoltageMeasure();
    power.enableVbusVoltageMeasure();
    power.enableSystemVoltageMeasure();

    // PWR button: enable the short-press interrupt.
    // getIrqStatus() is polled regularly by pollPowerButton(); a separate
    // PMU IRQ wire connected to an ESP32 GPIO is not required for this polling approach.
    power.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    power.clearIrqStatus();
    power.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);
    power.clearIrqStatus();

    Serial.println("AXP2101 ready (PWR short-press polling enabled)");
    readBattery();
  } else {
    Serial.println("WARNING: AXP2101 is unavailable; battery information will not be displayed.");
  }

  // ES8311 codec + speaker amplifier
  pinMode(PA, OUTPUT);
  digitalWrite(PA, LOW);
  const bool codecOk = initCodec();
  digitalWrite(PA, HIGH);
  if (!codecOk) {
    Serial.println("WARNING: ES8311 configuration failed. Check the ES8311 driver sources.");
    copySharedText(audioInfoText, sizeof(audioInfoText), "Audio codec error");
  }

  // Wi-Fi: the app continues if the network is unavailable and retries the connection.
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  if (strlen(WIFI_SSID) > 0 && strcmp(WIFI_SSID, "YOUR_WIFI_NAME") != 0) {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    const uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
      if (millis() - lastDraw > 300) drawUi();
      delay(50);
    }
  } else {
    copySharedText(audioInfoText, sizeof(audioInfoText), "Set Wi-Fi credentials in the code");
  }

  audioQueue = xQueueCreate(12, sizeof(AudioCommand));
  if (audioQueue) {
    xTaskCreatePinnedToCore(audioWorker, "radio-audio", 8192, nullptr, 3, nullptr, 1);
  } else {
    Serial.println("ERROR: failed to create the audio command queue");
  }

  copySharedText(streamTitle, sizeof(streamTitle), "");
  uiDirty = true;
  drawUi();
  Serial.println("GUI ready. Tap a station or press PLAY.");
}

void loop() {
  pollPowerButton();
  pollUserKey();
  handleTouch();

  if (pmuAvailable && millis() - lastBatteryRead > 5000) {
    lastBatteryRead = millis();
    readBattery();
    uiDirty = true;
  }

  // Long track titles require animation frames. The rest of the GUI is redrawn
  // only on changes or every 2.5 seconds; the marquee advances every 180 ms.
  const uint32_t now = millis();
  const bool marqueeActive = streamTitleNeedsMarquee();
  const bool marqueeFrameDue = marqueeActive && (now - lastMarqueeDraw >= 180);
  if (displayIsOn && (uiDirty || now - lastDraw > 2500 || marqueeFrameDue)) {
    drawUi();
    if (marqueeActive) lastMarqueeDraw = millis();
  }
  delay(2);
}
