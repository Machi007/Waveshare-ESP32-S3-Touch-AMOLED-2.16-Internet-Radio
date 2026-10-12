# Internet Radio — Waveshare ESP32-S3-Touch-AMOLED-2.16

An Arduino IDE project for the Waveshare ESP32-S3-Touch-AMOLED-2.16 board. It provides a touch GUI, four internet-radio presets, Play/Pause/Stop, volume and AMOLED brightness controls, battery/charging status, current-track metadata, and hardware-button shortcuts.

## Features

- 480 × 480 AMOLED display using the CO5300 controller and QSPI
- CST9220 capacitive touch controller
- Wi-Fi streaming with `ESP32-audioI2S` 3.x
- Four international radio presets (direct MP3 streams)
- Current track title from ICY `StreamTitle` metadata, with horizontal scrolling for long titles
- UTF-8 rendering for accented and extended Latin characters using U8g2 Unifont
- ES8311 audio codec and the board's speaker-amplifier control on GPIO46
- On-screen volume control mapped to the audio library's 0–21 volume scale
- Display brightness control from 5% to 100%
- Battery percentage and charging status via the AXP2101 PMU
- Short press of the physical PWR button turns the display off/on while audio keeps playing
- Short press of the physical +/KEY button (GPIO18) toggles mute and restores the previously selected volume
- Touch input is ignored while the display is off
- Large controls and labels, including the track title and stream status
- Saved volume, brightness, and last-selected station using Preferences

> **Hardware testing note:** This project was prepared from the Waveshare pinout and library examples, but it has not been compiled or tested on a physical board in this environment. The ES8311 driver files must match the version in your installed Waveshare example package.

## 1. Configure Wi-Fi

Open `ESP32S3_InternetRadio.ino` and set your Wi-Fi credentials:

```cpp
static const char *WIFI_SSID = "YOUR_WIFI_NAME";
static const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
```

Replace both placeholders with your actual network name and password. The device uses a 2.4 GHz Wi-Fi network supported by the ESP32-S3.

## 2. Install the required tools and libraries

Recommended setup:

- Arduino IDE 2.x
- Espressif ESP32 board package compatible with the official Waveshare examples
- Board: **ESP32S3 Dev Module**
- PSRAM: **OPI PSRAM**
- A partition scheme with sufficient space for the application

Use the versions supplied with the official example package for the ESP32-S3-Touch-AMOLED-2.16 where possible:

- **GFX Library for Arduino** — display driver and `Arduino_Canvas`
- **U8g2** — `u8g2_font_unifont_te` font for UTF-8 track metadata
- **SensorLib / CSTXXX touch driver** — provides `TouchDrvCSTXXX.hpp`
- **XPowersLib** — AXP2101 PMU support
- **ESP32-audioI2S** by schreibfaul1, version 3.x — `Audio.h`, stream decoding, and volume steps
- **ES8311 driver sources** from the official Waveshare `07_ES8311` example: `es8311.c`, `es8311.h`, and `es8311_reg.h`

The board's pin configuration is already included as `pin_config.h` in this project.

### Important: add the ES8311 source files

Copy `es8311.c`, `es8311.h`, and `es8311_reg.h` from the same Waveshare example package into the same folder as `ESP32S3_InternetRadio.ino`. Do not mix driver files from different versions of the example package.

Official references:

- Waveshare Arduino documentation: <https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16/Arduino>
- Waveshare example sources: <https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.16>
- ESP32-audioI2S library: <https://github.com/schreibfaul1/ESP32-audioI2S>
- U8g2 library: <https://github.com/olikraus/u8g2>

## 3. Radio presets

The project includes four international stations using direct MP3 stream URLs:

| Preset | Style | Stream URL |
|---|---|---|
| Radio Paradise | Eclectic music | `http://stream-uk1.radioparadise.com/mp3-128` |
| Groove Salad (SomaFM) | Ambient / downtempo | `https://ice5.somafm.com/groovesalad-128-mp3` |
| Drone Zone (SomaFM) | Atmospheric / ambient | `https://ice5.somafm.com/dronezone-128-mp3` |
| Power 181 | Top 40 / pop | `http://listen.181fm.com/181-power_128k.mp3` |

These URLs were checked against the broadcasters' published stream lists on October 12, 2026. Direct stream endpoints can change or become temporarily unavailable. If a station stops working, update its `url` field in the `STATIONS` array. SomaFM recommends using its permanent PLS/M3U links when a player supports playlist files; this firmware currently expects a direct stream URL.

Official stream lists:

- Radio Paradise: <https://legacy.radioparadise.com/rp3.php?file=links&name=Listen>
- SomaFM Groove Salad: <https://somafm.com/groovesalad/directstreamlinks.html>
- SomaFM Drone Zone: <https://somafm.com/dronezone/directstreamlinks.html>
- 181.FM: <https://www.181.fm/legacy.html>

To change the presets, edit the `STATIONS` array in `ESP32S3_InternetRadio.ino`. Keep the number of entries at four unless you also adjust the screen layout.

## 4. Upload the firmware

1. Install the libraries and copy the ES8311 driver files as described above.
2. Open `ESP32S3_InternetRadio.ino` in Arduino IDE.
3. Select **ESP32S3 Dev Module**, enable **OPI PSRAM**, and choose a partition scheme with enough application space.
4. Enter your Wi-Fi credentials.
5. Connect the board using a USB-C data cable, select the correct port, and upload the sketch.
6. Open Serial Monitor at **115200 baud**. Check for messages such as `CST9220 touch ready`, `AXP2101 ready`, and successful ES8311 initialization.
7. Tap a station preset to start connecting to the stream. Use PLAY/PAUSE to pause or resume playback.

## 5. Controls and display behavior

- **PWR button:** short-press to turn the display off; short-press again to turn it back on. Wi-Fi and radio playback continue. The selected brightness is restored on wake. PWR is handled by the AXP2101 PMU rather than a normal ESP32 GPIO. The sketch polls `power.getIrqStatus()` and then checks `isPekeyShortPressIrq()`.
- **+ / KEY button (GPIO18):** short-press to mute; press again to restore the configured volume. The GUI shows `MUTED`. Moving the on-screen volume slider cancels mute.
- **Station presets:** tap one of the four buttons arranged in a 2 × 2 grid to select and play that station.
- **PLAY / PAUSE:** starts the selected station if no stream is active, otherwise toggles pause/resume.
- **STOP:** stops the stream and clears the current track title.
- **Track title:** the title is read from ICY `StreamTitle` metadata. Short titles are centered; titles wider than the available 448 px scroll horizontally, including extended UTF-8 characters. If a station does not provide a title, the display waits briefly and then shows a notice.
- **Status text:** messages such as `Connecting to stream...`, `Audio ready`, and codec or connection errors appear below the metadata.
- **Volume and brightness sliders:** wide tracks, large knobs, and enlarged touch regions make them easier to control. Drag the slider to change its value.
- **Settings:** volume, brightness, and the last-selected station are saved to Preferences when the touch interaction ends.
- **Battery:** percentage and charging state are shown when a battery is connected; otherwise the status area shows USB power.

## 6. Pinout

| Function | GPIO |
|---|---:|
| AMOLED QSPI SDIO0–3 | 4, 5, 6, 7 |
| AMOLED QSPI CLK / CS / RESET | 38 / 12 / 39 |
| I²C SDA / SCL | 15 / 14 |
| CST9220 INT / RESET | 11 / 40 |
| I²S MCLK / BCLK / LRCK / DOUT | 42 / 9 / 45 / 8 |
| ES8311 amplifier enable | 46 |
| +/KEY user button | 18 (active-low) |

The pinout is based on the official Waveshare documentation: <https://docs.waveshare.com/ESP32-S3-Touch-AMOLED-2.16>

## 7. Troubleshooting

- **Black display:** first run the official `01_HelloWorld` example for the 2.16-inch board and verify PSRAM settings and the GFX library version.
- **Touch does not work:** verify `TouchDrvCSTXXX.hpp` from the CST9220 example package and the `TP_INT=11`, `TP_RESET=40` pins.
- **+ / KEY does not mute:** make sure GPIO18 is not configured elsewhere in a modified sketch. The button is connected between GPIO18 and GND and uses the internal pull-up.
- **PWR does not toggle the display:** open Serial Monitor at 115200 baud and check for `AXP2101 ready (PWR short-press polling enabled)`. After a press, `PWR: short press detected` should appear. If it does not, check that the installed XPowersLib supports the AXP2101. If the message appears but the panel does not switch off, check the Arduino_GFX version and CO5300 driver.
- **Battery information is missing:** verify that XPowersLib supports the AXP2101 and that a battery is connected. Without a battery, the display shows USB.
- **No audio:** check ES8311 initialization, the matching ES8311 driver files, GPIO46 amplifier enable, speaker connections, stream URL, and volume setting.
- **Track title or accented characters are missing:** verify that U8g2 is installed and that the current Arduino_GFX version supports U8g2 fonts. The station must provide ICY `StreamTitle` metadata; some streams provide only a show name or no metadata at all. Search Serial Monitor (115200 baud) for `NOW PLAYING:`.
- **A stream will not connect:** check Wi-Fi, try the URL in VLC first, and then update the relevant station URL if necessary.
- **ES8311 compilation errors:** driver APIs can differ between Waveshare example-package versions. Use all three `es8311.*` source files from the same example package rather than mixing versions.

## Technical notes

- The audio output is configured for **44.1 kHz**, matching the ES8311 codec configuration in the sketch. If you change the output rate, update both `AUDIO_SAMPLE_RATE`, `audio.setOutputSampleRate(...)`, and the codec configuration.
- The firmware uses `Audio::audio_info_callback` and `Audio::evt_streamtitle` from ESP32-audioI2S 3.x to receive track titles.
- The radio must receive an actual stream that sends compatible metadata in order to show the song title. It cannot infer the title from the audio signal alone.
