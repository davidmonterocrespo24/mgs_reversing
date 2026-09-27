// Board selection.
//
// Two physical machines run this port and they share nothing but the chip: the
// Waveshare ESP32-S3-Touch-LCD-2 (ST7789, 16 MB flash, buttons on a header)
// and the XIAO ESP32S3 Sense handheld (ILI9341, 8 MB, resistor ladder and an
// analog stick). Rather than let one grow #ifdefs through the other, each gets
// its own complete pin map and this file picks one. Define MGS_BOARD_XIAO from
// the build to get the handheld.
#ifdef MGS_BOARD_XIAO
#include "board_pins_xiao.h"
#else

// Waveshare ESP32-S3-Touch-LCD-2 pin map — VERIFIED against the vendor
// schematic (ESP32-S3-Touch-LCD-2-SchDoc.pdf) and the ESP-IDF demos in
// ESP32-S3-Touch-LCD-2-Demo.zip (esp_lcd_new_panel_st7789 path).
#pragma once

#include "driver/spi_master.h"
#include "hal/lcd_types.h"

// ---- LCD: ST7789T3, 240x320 native portrait, SPI2 (shared with microSD) ----
#define LCD_SPI_HOST    SPI2_HOST
// 80 MHz is what ALL vendor ESP-IDF demos ship (write-only link; reads would
// cap at 40). Rebuild with 40 MHz only to compare benchmark numbers.
#define LCD_SPI_HZ      (80 * 1000 * 1000)

#define PIN_LCD_SCLK    39
#define PIN_LCD_MOSI    38
#define PIN_LCD_CS      45
#define PIN_LCD_DC      42
#define PIN_LCD_RST     (-1)  // no GPIO: RC power-on reset circuit; SWRESET only
#define PIN_LCD_BL      1     // SS8050 NPN driver
#define LCD_BL_ON_LEVEL 1

// vendor init: RGB element order, inversion ON (IPS), no gaps.
// Landscape 320x240 = vendor "rotation 1": MADCTL MX|MV -> swap_xy + mirror X.
#define LCD_RGB_ORDER    LCD_RGB_ELEMENT_ORDER_RGB
#define LCD_INVERT_COLOR true
#define LCD_MIRROR_X     true
#define LCD_MIRROR_Y     false
#define LCD_GAP_X        0
#define LCD_GAP_Y        0

// ---- microSD: SDSPI on the SAME SPI2 bus ----
// MISO=40 belongs to the bus config (SD only — never to the LCD panel IO).
// SD_CS must be held high from boot so the card stays off the bus.
#define PIN_SD_MISO     40
#define PIN_SD_CS       41

// ---- Touch: CST816D, I2C0 (shared with QMI8658 IMU @0x6B) ----
// Coord transform for landscape rotation 1: x' = raw_y, y' = 239 - raw_x
#define PIN_TOUCH_SDA   48
#define PIN_TOUCH_SCL   47
#define PIN_TOUCH_INT   46    // wired; vendor demos poll instead (chip NAKs while asleep)
#define TOUCH_I2C_ADDR  0x15

// ---- Game buttons: ALL on header P1, active low, common GND at P1 pin 13 ----
// P1 pin order (top to bottom): IO2, IO4, IO6, IO16, IO17, IO18, IO21, IO8,
// IO7, IO10, IO20, IO19, GND, 5V. IO16/IO21 already have 4.7K pullups on
// board (camera SCCB); the rest use internal pullups. Camera must stay
// unplugged (IO2/4/6/7/8/10 are DVP pins). GPIO0 (BOOT) stays as extra START.
#define PIN_BTN_UP      2   // P1 pin 1
#define PIN_BTN_DOWN    4   // P1 pin 2
#define PIN_BTN_LEFT    6   // P1 pin 3
#define PIN_BTN_RIGHT   16  // P1 pin 4  (hw pullup)
#define PIN_BTN_A       17  // P1 pin 5
#define PIN_BTN_B       18  // P1 pin 6
#define PIN_BTN_L       21  // P1 pin 7  (hw pullup)
#define PIN_BTN_R       8   // P1 pin 8
#define PIN_BTN_START   7   // P1 pin 9
#define PIN_BTN_SELECT  10  // P1 pin 10

#endif // MGS_BOARD_XIAO
