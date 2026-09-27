// XIAO ESP32S3 Sense + ILI9341 320x240 SPI panel — the handheld console.
//
// Every number here was verified on the assembled board with the shakedown
// firmware in port/consola/test/, not taken from a datasheet: the panel's
// controller, its rotation, the button map, the stick's rest point and travel,
// and the microSD's chip select were each measured and confirmed on screen.
// The wiring diagram that matches this file is esp32/hardware/HARDWARE.md.
//
// The XIAO exposes eleven pins, D0..D10, and all eleven are used. The silkscreen
// D-numbers are NOT the GPIO numbers; both are given below because mixing them
// up is the single easiest way to lose an afternoon here.
#pragma once

#include "driver/spi_master.h"
#include "hal/lcd_types.h"

// ---- LCD: ILI9341, 320x240 landscape, SPI2 (shared with the microSD) ----
//
// 320x240 is the game's NATIVE resolution: no scaling, no letterbox, and two
// bytes per pixel. That is half the per-frame traffic of the 480x320 ILI9488
// this console was originally planned around, and it costs nothing.
#define LCD_SPI_HOST    SPI2_HOST
// 40 MHz to start. The panel tolerates more but these are flying jumper wires
// with no ground plane; raise it once a frame-rate measurement says it matters.
#define LCD_SPI_HZ      (40 * 1000 * 1000)

#define PIN_LCD_SCLK    7     // D8
#define PIN_LCD_MOSI    9     // D10
#define PIN_LCD_CS      4     // D3
#define PIN_LCD_DC      43    // D6  (also the chip's UART TX; fine as a GPIO)
#define PIN_LCD_RST     5     // D4  — a real reset line, see below
#define PIN_LCD_BL      (-1)  // backlight strapped to 3V3; no GPIO control
#define LCD_BL_ON_LEVEL 1

// RST is a dedicated pin and that is load-bearing. Strapping it to 3V3 and
// relying on the software reset left the panel dead on this board, exactly as
// the user's own proven project (E:\Hardware\200LX) warned by reserving a whole
// GPIO for it. D4 is free for this because the Sense's card slot brings its own
// chip select on an internal pin.

// Landscape, rotated 180: MADCTL 0xE8 = MY|MX|MV|BGR. Determined by looking at
// the assembled unit — 0x28 read upside down in the case.
#define LCD_RGB_ORDER    LCD_RGB_ELEMENT_ORDER_BGR
#define LCD_INVERT_COLOR false     // ILI9341 is not an IPS panel; no inversion
#define LCD_MIRROR_X     true
#define LCD_MIRROR_Y     true
#define LCD_GAP_X        0
#define LCD_GAP_Y        0

// ---- microSD: the Sense expansion board's own slot ----
//
// Shares the LCD's SPI bus (Seeed's documentation gives SCK/MISO/MOSI as
// D8/D9/D10) with its chip select on GPIO21, which is internal to the
// expansion board and never appears on the header. Verified on hardware:
// mounts, 1910 MB, MGS/STAGE.DIR present at its full 71,892,992 bytes.
//
// Sharing means arbitrating — Mgs_SpiBusTake/Give, the same mutex the
// Waveshare board needed, including the drain of in-flight transfers before
// the bus is released.
#define PIN_SD_MISO     8     // D9
#define PIN_SD_CS       21    // internal to the Sense board

// ---- Controls ----
//
// Two buttons have their own pins; the other six share one ADC pin through a
// resistor ladder. The two direct ones are deliberately the pair a player
// holds down while doing something else — fire and aim — because a ladder can
// only report one press at a time.
#define PIN_BTN_A       6     // D5  — square / fire
#define PIN_BTN_B       44    // D7  — R1 / aim
#define PIN_BTN_LADDER  3     // D2  — six buttons, ADC1_CH2

// Ladder windows, measured on the assembled unit with a 10k pull-up:
//   circle 0R -> 0     x 2k2 -> 738    triangle 4k7 -> 1310
//   L1    10k -> 2048  START 22k -> 2816   SELECT 47k -> 3376   idle -> 4095
// Half a volt between steps, so 5% resistors are comfortable.
#define LADDER_CIRCLE_MAX    350
#define LADDER_CROSS_MAX    1000
#define LADDER_TRIANGLE_MAX 1700
#define LADDER_L1_MAX       2450
#define LADDER_START_MAX    3100
#define LADDER_SELECT_MAX   3700

// ---- Analog stick: a drone gimbal, not a console stick ----
//
// It rests wherever its springs leave it (2317 on this unit, not 2048) and its
// throw covers only part of the scale (X ran 141..2888). Both are handled by
// calibrating at boot rather than by assuming a range. The dead zone is four
// times the noise measured at rest (+/-15 counts); without it the crosshair
// drifted on its own. Both axes read backwards from the screen's sense, which
// depends on which end terminal went to 3V3 — a sign flip, not a resolder.
#define PIN_STICK_X     1     // D0 — ADC1_CH0
#define PIN_STICK_Y     2     // D1 — ADC1_CH1
#define STICK_DEADZONE  60
/* These are the signs the GAME needs, which are not the ones the shakedown
 * firmware used. That test drew a crosshair, and a screen's Y grows downward,
 * so it negated Y to make "push up, dot goes up" -- a display convention. The
 * game is handed direction BITS, where up is simply up, so carrying the screen
 * negation across inverted the axis a second time. X keeps its flip because
 * that one really is the potentiometer's wiring. Confirmed on the assembled
 * unit both times. */
#define STICK_INVERT_X  1
#define STICK_INVERT_Y  0
#define STICK_SAMPLES   8     // averaged per read; the ADC is noisy bare

// ---- No touch ----
// The panel has an XPT2046 but all eleven pins are spoken for. Its T_CS is
// strapped to 3V3, which is not optional: left floating it selects itself and
// drives the shared MISO line, corrupting card reads.
