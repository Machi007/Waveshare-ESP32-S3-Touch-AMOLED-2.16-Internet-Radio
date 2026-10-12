#pragma once

// Waveshare ESP32-S3-Touch-AMOLED-2.16 (CO5300 + CST9220 + ES8311 + AXP2101)
#define XPOWERS_CHIP_AXP2101

// AMOLED CO5300, QSPI
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK  38
#define LCD_CS    12
#define LCD_RESET 39
#define LCD_WIDTH  480
#define LCD_HEIGHT 480

// Shared I2C bus: CST9220, AXP2101, ES8311 and other peripherals
#define IIC_SDA 15
#define IIC_SCL 14

// CST9220 touch controller
#define TP_INT   11
#define TP_RESET 40

// Physical +/KEY user button (Key3), active-low
#define USER_KEY_PIN 18

// ES8311 audio codec / I2S output
#define MCLKPIN 42
#define BCLKPIN 9
#define WSPIN   45
#define DOPIN   8
#define DIPIN   10
#define PA      46
