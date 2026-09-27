/* Hardware shakedown for the MGS handheld console.
 *
 * XIAO ESP32S3 + Hosyond 3.5" ILI9488 (SPI) + microSD on the display board +
 * analog stick + 2 direct buttons + 6-button resistor ladder. This firmware
 * exists to answer, one piece at a time, "is it wired right?" BEFORE the game
 * ever runs on this board, so a game bug is never confused with a loose wire.
 *
 * What it verifies, in boot order:
 *   1. PSRAM     - the game needs all 8 MB; reported on serial
 *   2. Display   - full-screen R, G, B, W flashes (panel + colour channels),
 *                  then a live dashboard
 *   3. microSD   - mounted read-only, size + first entries printed
 *   4. Stick     - crosshair follows it inside a box, raw values on screen
 *   5. Buttons   - 8 labelled boxes light up while held; the ladder's raw
 *                  ADC value is drawn as a bar with the detection windows
 *
 * Serial (USB, 115200) mirrors everything with exact numbers - the screen
 * shows it works, the serial says by how much.
 *
 * Pin map (matches esp32/hardware/HARDWARE.md exactly):
 *   D0 GPIO1  stick X (ADC1_CH0)      D6 GPIO43 LCD DC
 *   D1 GPIO2  stick Y (ADC1_CH1)      D7 GPIO44 button B (R1)
 *   D2 GPIO3  ladder  (ADC1_CH2)      D8 GPIO7  SCK  (LCD+SD)
 *   D3 GPIO4  LCD CS                  D9 GPIO8  MISO (SD)
 *   D4 GPIO5  SD CS                   D10 GPIO9 MOSI (LCD+SD)
 *   D5 GPIO6  button A (square)
 *   LCD RST and backlight are strapped to 3V3, so reset is done by software
 *   command (SWRESET) - that line is load-bearing, not decoration.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_vfs_fat.h"
#include "esp_heap_caps.h"
#include "sdmmc_cmd.h"

/* ---- pins ---------------------------------------------------------------- */
#define PIN_SCK      7
#define PIN_MISO     8
#define PIN_MOSI     9
#define PIN_LCD_CS   4
#define PIN_LCD_DC   43
#define PIN_SD_CS    5
#define PIN_BTN_A    6      /* square: fire   */
#define PIN_BTN_B    44     /* R1: aim        */
#define ADC_CH_X     ADC_CHANNEL_0   /* GPIO1 */
#define ADC_CH_Y     ADC_CHANNEL_1   /* GPIO2 */
#define ADC_CH_LAD   ADC_CHANNEL_2   /* GPIO3 */
/* The panel's reset borrows the microSD's chip select: see the long note at
 * lcd_hw_reset() for why it is D4 and not D7. */
#define PIN_LCD_RST  PIN_SD_CS       /* D4 / GPIO5 */

/* Start SLOW on purpose.
 *
 * Two independent reasons, and both were learned the hard way here. The
 * ILI9488's READ cycle is far slower than its write cycle -- roughly 6 MHz
 * ceiling against 20+ for writes -- so an ID probe at 20 MHz returns garbage
 * from a panel that is perfectly healthy. And flying jumper wires with no
 * ground plane ring badly at 20 MHz. At 1 MHz a full screen takes ~0.4 s,
 * which is useless for a game and ideal for answering "is it wired right".
 * The game build raises this once the wiring is proven. */
#define LCD_HZ       (1 * 1000 * 1000)

/* Two candidate panels, decided at runtime rather than guessed.
 *
 * The console diagram assumed a 3.5" ILI9488 (480x320, RGB666, 3 bytes per
 * pixel over SPI). The board's OWN proven project -- E:\HardwareLX, whose
 * pins.h says "320x240 ILI9341 panel" and whose build_opt.h sets
 * ILI9341_DRIVER -- uses a different controller entirely. Sending ILI9488 init
 * to an ILI9341 means the wrong pixel format, commands it does not implement,
 * and drawing windows past the end of its memory: a white screen, exactly what
 * was seen. Rather than pick one and be wrong a third time, alternate and
 * announce. Whichever paints is the answer.
 *
 * ILI9341 at 320x240 is also the game's native resolution -- no scaling, no
 * letterbox, and two bytes per pixel instead of three. */
#define W_MAX 480
static int W = 320, H = 240;   /* current candidate */
static int BPP = 2;            /* 2 = RGB565, 3 = RGB666 */

/* ---- tiny 5x7 font (only the glyphs the dashboard needs) ----------------- */
/* column-major, LSB = top row; same encoding every 5x7 font uses */
typedef struct { char c; unsigned char col[5]; } Glyph;
static const Glyph FONT[] = {
    {'0',{0x3E,0x51,0x49,0x45,0x3E}}, {'1',{0x00,0x42,0x7F,0x40,0x00}},
    {'2',{0x42,0x61,0x51,0x49,0x46}}, {'3',{0x21,0x41,0x45,0x4B,0x31}},
    {'4',{0x18,0x14,0x12,0x7F,0x10}}, {'5',{0x27,0x45,0x45,0x45,0x39}},
    {'6',{0x3C,0x4A,0x49,0x49,0x30}}, {'7',{0x01,0x71,0x09,0x05,0x03}},
    {'8',{0x36,0x49,0x49,0x49,0x36}}, {'9',{0x06,0x49,0x49,0x29,0x1E}},
    {'A',{0x7E,0x11,0x11,0x11,0x7E}}, {'B',{0x7F,0x49,0x49,0x49,0x36}},
    {'D',{0x7F,0x41,0x41,0x22,0x1C}}, {'E',{0x7F,0x49,0x49,0x49,0x41}},
    {'L',{0x7F,0x40,0x40,0x40,0x40}}, {'O',{0x3E,0x41,0x41,0x41,0x3E}},
    {'S',{0x46,0x49,0x49,0x49,0x31}}, {'T',{0x01,0x01,0x7F,0x01,0x01}},
    {'X',{0x63,0x14,0x08,0x14,0x63}}, {'Y',{0x07,0x08,0x70,0x08,0x07}},
    {'K',{0x7F,0x08,0x14,0x22,0x41}}, {'-',{0x08,0x08,0x08,0x08,0x08}},
    {'G',{0x3E,0x41,0x49,0x49,0x7A}}, {'V',{0x1F,0x20,0x40,0x20,0x1F}},
    {'R',{0x7F,0x09,0x19,0x29,0x46}}, {'C',{0x3E,0x41,0x41,0x41,0x22}},
    {'I',{0x00,0x41,0x7F,0x41,0x00}}, {'M',{0x7F,0x02,0x0C,0x02,0x7F}},
    {'N',{0x7F,0x04,0x08,0x10,0x7F}}, {'P',{0x7F,0x09,0x09,0x09,0x06}},
    {'U',{0x3F,0x40,0x40,0x40,0x3F}}, {'Z',{0x61,0x51,0x49,0x45,0x43}},
    {' ',{0x00,0x00,0x00,0x00,0x00}},
};

/* ---- LCD: raw SPI, blocking - clarity over speed for a test -------------- */
static spi_device_handle_t s_lcd;

static void lcd_cmd(unsigned char c) {
    gpio_set_level(PIN_LCD_DC, 0);
    spi_transaction_t t = { .length = 8, .tx_buffer = &c };
    spi_device_polling_transmit(s_lcd, &t);
}
static void lcd_data(const unsigned char *d, int n) {
    if (n <= 0) return;
    gpio_set_level(PIN_LCD_DC, 1);
    spi_transaction_t t = { .length = n * 8, .tx_buffer = d };
    spi_device_polling_transmit(s_lcd, &t);
}
static void lcd_c1(unsigned char c, unsigned char a) { lcd_cmd(c); lcd_data(&a,1); }

static void lcd_window(int x0, int y0, int x1, int y1) {
    unsigned char ca[] = { x0>>8, x0, x1>>8, x1 };
    unsigned char pa[] = { y0>>8, y0, y1>>8, y1 };
    lcd_cmd(0x2A); lcd_data(ca, 4);
    lcd_cmd(0x2B); lcd_data(pa, 4);
    lcd_cmd(0x2C);
}

/* one line of RGB666 (3 bytes/pixel: the ONLY format ILI9488 accepts on SPI) */
static unsigned char s_line[W_MAX * 3];

static void lcd_fill(int x, int y, int w, int h, unsigned char r,
                     unsigned char g, unsigned char b) {
    if (w <= 0 || h <= 0) return;
    lcd_window(x, y, x + w - 1, y + h - 1);
    if (BPP == 3) {
        for (int i = 0; i < w; i++) {
            s_line[i*3+0] = r; s_line[i*3+1] = g; s_line[i*3+2] = b;
        }
    } else {
        unsigned short p = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
        for (int i = 0; i < w; i++) {        /* SPI sends high byte first */
            s_line[i*2+0] = p >> 8; s_line[i*2+1] = p & 0xFF;
        }
    }
    gpio_set_level(PIN_LCD_DC, 1);
    for (int row = 0; row < h; row++) {
        spi_transaction_t t = { .length = w * BPP * 8, .tx_buffer = s_line };
        spi_device_polling_transmit(s_lcd, &t);
    }
}

static void lcd_char(int x, int y, char ch, int scale, unsigned char r,
                     unsigned char g, unsigned char b) {
    /* Blank for anything not in the table. This used to index FONT[22] as "the
     * space glyph", which stopped being true the moment glyphs were added
     * above it -- the kind of constant that rots silently. */
    static const unsigned char blank[5] = {0,0,0,0,0};
    const unsigned char *col = blank;
    for (unsigned i = 0; i < sizeof FONT / sizeof FONT[0]; i++)
        if (FONT[i].c == ch) { col = FONT[i].col; break; }
    for (int cx = 0; cx < 5; cx++)
        for (int cy = 0; cy < 7; cy++)
            if (col[cx] & (1 << cy))
                lcd_fill(x + cx*scale, y + cy*scale, scale, scale, r, g, b);
}
static void lcd_text(int x, int y, const char *s, int scale, unsigned char r,
                     unsigned char g, unsigned char b) {
    for (; *s; s++, x += 6 * scale) lcd_char(x, y, *s, scale, r, g, b);
}
static void lcd_num(int x, int y, int v, int scale, unsigned char r,
                    unsigned char g, unsigned char b) {
    char buf[8];
    snprintf(buf, sizeof buf, "%4d", v);
    lcd_text(x, y, buf, scale, r, g, b);
}

/* ---- bit-banged SPI: the tie-breaker ------------------------------------
 *
 * The panel resets when D4 is pulsed -- the flash is visible -- so power and
 * at least one wire are good, yet no init is ever obeyed. Two very different
 * causes remain and nothing so far separates them: a broken data wire, or the
 * SPI peripheral driving something other than what we think it is.
 *
 * Clocking the bits by hand answers it without a multimeter. This path uses
 * nothing but gpio_set_level, holds CS low across the whole command-plus-data
 * sequence (the hardware driver releases it between transactions, which some
 * panels treat as an aborted memory write), and runs at a few kHz where no
 * amount of wire capacitance matters.
 *
 *   bit-bang paints, hardware SPI does not  -> wires fine, SPI config is wrong
 *   neither paints                          -> a data wire is broken
 */
static void lcd_hw_reset(void);          /* defined with the panel inits below */

/* Can we actually drive each pin?
 *
 * Everything paints white in shades, which is what a panel does when it gets
 * data but never a valid command -- the signature of a DC line that is not
 * moving. And DC sits on D6 = GPIO43 = U0TXD, the chip's UART transmit pin: if
 * anything still owns it, gpio_set_level writes into the void and the panel
 * sees every byte as pixel data.
 *
 * An output pin can be read back through the GPIO input register, so the chip
 * can check its own work: drive 0, read; drive 1, read. A pin that will not
 * follow is held by another peripheral or shorted, and that is a fact rather
 * than a theory. */
static void pin_selftest(void) {
    struct { int pin; const char *name; } p[] = {
        { PIN_SCK,    "SCK  D8/GPIO7"  },
        { PIN_MOSI,   "MOSI D10/GPIO9" },
        { PIN_LCD_CS, "CS   D3/GPIO4"  },
        { PIN_LCD_DC, "DC   D6/GPIO43" },
        { PIN_LCD_RST,"RST  D4/GPIO5"  },
    };
    printf("\n[self] can the chip drive each pin?\n");
    for (unsigned i = 0; i < sizeof p / sizeof p[0]; i++) {
        gpio_reset_pin(p[i].pin);
        gpio_set_direction(p[i].pin, GPIO_MODE_INPUT_OUTPUT);  /* readable */
        gpio_set_level(p[i].pin, 0); vTaskDelay(pdMS_TO_TICKS(2));
        int lo = gpio_get_level(p[i].pin);
        gpio_set_level(p[i].pin, 1); vTaskDelay(pdMS_TO_TICKS(2));
        int hi = gpio_get_level(p[i].pin);
        printf("[self] %-16s write0->read%d  write1->read%d  %s\n",
               p[i].name, lo, hi,
               (lo == 0 && hi == 1) ? "OK" :
               (lo == hi) ? "<-- STUCK: another peripheral owns it, or there is a short"
                          : "<-- INVERTED?");
        gpio_set_level(p[i].pin, 0);
    }
}

static void bb_pins(void) {
    gpio_config_t io = { .pin_bit_mask = (1ULL << PIN_SCK) | (1ULL << PIN_MOSI) |
                                         (1ULL << PIN_LCD_CS) | (1ULL << PIN_LCD_DC),
                         .mode = GPIO_MODE_OUTPUT };
    gpio_config(&io);
    gpio_set_level(PIN_LCD_CS, 1);
    gpio_set_level(PIN_SCK, 0);
}

static inline void bb_byte(unsigned char v) {
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(PIN_MOSI, (v >> i) & 1);
        gpio_set_level(PIN_SCK, 1);          /* mode 0: sampled on rising */
        gpio_set_level(PIN_SCK, 0);
    }
}
static void bb_cmd(unsigned char c) {
    gpio_set_level(PIN_LCD_DC, 0); gpio_set_level(PIN_LCD_CS, 0);
    bb_byte(c);
    gpio_set_level(PIN_LCD_CS, 1);
}
static void bb_dat(const unsigned char *d, int n) {
    gpio_set_level(PIN_LCD_DC, 1); gpio_set_level(PIN_LCD_CS, 0);
    while (n--) bb_byte(*d++);
    gpio_set_level(PIN_LCD_CS, 1);
}
static void bb_c1(unsigned char c, unsigned char a) { bb_cmd(c); bb_dat(&a, 1); }

/* Paint the whole 320x240 panel one colour, CS held low throughout the pixel
 * stream exactly as a panel expects for a continuous memory write. */
static void bb_fill565(unsigned short colour) {
    unsigned char ca[] = {0, 0, (319 >> 8), (319 & 0xFF)};
    unsigned char pa[] = {0, 0, (239 >> 8), (239 & 0xFF)};
    bb_cmd(0x2A); bb_dat(ca, 4);
    bb_cmd(0x2B); bb_dat(pa, 4);
    bb_cmd(0x2C);
    gpio_set_level(PIN_LCD_DC, 1); gpio_set_level(PIN_LCD_CS, 0);
    for (long i = 0; i < 320L * 240L; i++) {
        bb_byte(colour >> 8);
        bb_byte(colour & 0xFF);
        if ((i & 2047) == 0) vTaskDelay(1);   /* feed the watchdog */
    }
    gpio_set_level(PIN_LCD_CS, 1);
}

static void bb_try(void) {
    printf("\n[bb] === SOFTWARE SPI: slow init by hand ===\n");
    bb_pins();
    lcd_hw_reset();
    vTaskDelay(pdMS_TO_TICKS(120));      /* settle: the 200LX project needs it */
    bb_cmd(0x01); vTaskDelay(pdMS_TO_TICKS(150));
    bb_cmd(0x11); vTaskDelay(pdMS_TO_TICKS(150));
    bb_c1(0x3A, 0x55);                   /* RGB565 */
    bb_c1(0x36, 0x28);                   /* landscape */
    bb_cmd(0x29); vTaskDelay(pdMS_TO_TICKS(50));
    printf("[bb] painting RED (takes a few seconds)\n");
    bb_fill565(0xF800);
    vTaskDelay(pdMS_TO_TICKS(2500));
    printf("[bb] painting GREEN\n");
    bb_fill565(0x07E0);
    vTaskDelay(pdMS_TO_TICKS(2500));
    printf("[bb] end of bit-bang\n");
}

/* Bus setup is separate from panel setup on purpose: the retry loop re-inits
 * the PANEL many times, and spi_bus_initialize() may only ever run once. */
static void lcd_bus_init(void) {
    gpio_config_t io = { .pin_bit_mask = 1ULL << PIN_LCD_DC,
                         .mode = GPIO_MODE_OUTPUT };
    gpio_config(&io);

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCK, .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = sizeof s_line,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = LCD_HZ, .mode = 0,
        .spics_io_num = PIN_LCD_CS, .queue_size = 4,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &s_lcd));
}

/* Pulse a real reset line.
 *
 * The console diagram strapped RST to 3V3 and relied on SWRESET (0x01). The
 * board's own proven project does not: pins.h reserves a whole GPIO for it
 * ("NOTHING else may use GPIO 8") and the library drives it low at startup.
 * That is the one difference between a setup known to work on this exact panel
 * and this one, which flickers when the lines are wiggled -- so it is powered
 * and connected -- yet ignores every init.
 *
 * Driving this costs nothing when RST is still strapped high: the pin simply
 * drives into no load. Move the display's RST wire from 3V3 to D4 and it
 * becomes a real reset.
 *
 * D4 and not D7. D7 carries button B, and a button shorts its pin to GND when
 * pressed -- driving that pin high would put the output driver into a dead
 * short every time the button was touched, and every press would reset the
 * panel as a bonus. D4 is the microSD's chip select and the card is not wired
 * yet, so it is genuinely free right now. When the card goes in, this needs a
 * decision: all eleven pins are spoken for, and a panel that truly requires a
 * reset line means giving something up (or moving to the XIAO Plus, which has
 * nine more pads on the back). PIN_LCD_RST is defined up with the other pins. */

static void lcd_hw_reset(void) {
    gpio_reset_pin(PIN_LCD_RST);
    gpio_set_direction(PIN_LCD_RST, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_LCD_RST, 1); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_LCD_RST, 0); vTaskDelay(pdMS_TO_TICKS(20));  /* reset */
    gpio_set_level(PIN_LCD_RST, 1); vTaskDelay(pdMS_TO_TICKS(150));
}

/* ILI9341: 320x240, RGB565. This is what E:\Hardware\200LX drives on this very
 * hardware, so it is the candidate with evidence behind it. */
static void lcd_init_9341(void) {
    W = 320; H = 240; BPP = 2;
    lcd_hw_reset();
    lcd_cmd(0x01); vTaskDelay(pdMS_TO_TICKS(150));   /* software reset */
    lcd_cmd(0x11); vTaskDelay(pdMS_TO_TICKS(120));   /* sleep out       */

    lcd_c1(0x3A, 0x55);                  /* 16-bit pixel format */
    /* MADCTL: landscape, rotated 180, BGR.
     *   0x20 MV  row/column exchange -> landscape
     *   0x40 MX  mirror columns  \  together: turn the picture upside down,
     *   0x80 MY  mirror rows     /  which is how this panel sits in the case
     *   0x08 BGR the panel's own channel order (verified: R/G/B bands correct)
     * 0x28 gave a picture that read upside down on the bench. */
    lcd_c1(0x36, 0xE8);
    lcd_c1(0xC0, 0x23);                  /* power control 1     */
    lcd_c1(0xC1, 0x10);
    { unsigned char d[] = {0x3E,0x28}; lcd_cmd(0xC5); lcd_data(d,2); }
    lcd_c1(0xC7, 0x86);
    { unsigned char d[] = {0x00,0x18}; lcd_cmd(0xB1); lcd_data(d,2); }
    { unsigned char d[] = {0x08,0x82,0x27}; lcd_cmd(0xB6); lcd_data(d,3); }
    lcd_cmd(0x29); vTaskDelay(pdMS_TO_TICKS(20));    /* display on */
}

/* ILI9488: 480x320, RGB666 -- three bytes per pixel, the only format its SPI
 * interface accepts. Kept as the second candidate. */
static void lcd_init_9488(void) {
    W = 480; H = 320; BPP = 3;
    lcd_hw_reset();
    lcd_cmd(0x01); vTaskDelay(pdMS_TO_TICKS(150));
    lcd_cmd(0x11); vTaskDelay(pdMS_TO_TICKS(120));

    lcd_c1(0x3A, 0x66);
    lcd_c1(0x36, 0x28);
    { unsigned char d[] = {0x17,0x15}; lcd_cmd(0xC0); lcd_data(d,2); }
    lcd_c1(0xC1, 0x41);
    { unsigned char d[] = {0x00,0x12,0x80}; lcd_cmd(0xC5); lcd_data(d,3); }
    lcd_c1(0xB0, 0x00);
    lcd_c1(0xB1, 0xA0);
    lcd_c1(0xB4, 0x02);
    { unsigned char d[] = {0x02,0x02,0x3B}; lcd_cmd(0xB6); lcd_data(d,3); }
    lcd_c1(0xE9, 0x00);
    { unsigned char d[] = {0xA9,0x51,0x2C,0x82}; lcd_cmd(0xF7); lcd_data(d,4); }
    lcd_cmd(0x29); vTaskDelay(pdMS_TO_TICKS(20));
}

static void lcd_init(void) { lcd_init_9341(); }

/* Ask the panel who it is. RDID4 (0xD3) answers ..,00,94,88 on a real
 * ILI9488, using the SDO line that is already wired for the microSD. This
 * turns "the screen is white" from a mystery into a verdict: ID present means
 * the wiring is good so look at power or init; ID absent means the panel
 * never heard us, so look at SCK/MOSI/CS/DC and VCC. */
static int lcd_probe(void) {
    unsigned char rx[4] = {0};
    lcd_cmd(0xD3);
    gpio_set_level(PIN_LCD_DC, 1);
    spi_transaction_t t = { .length = 32, .rxlength = 32, .rx_buffer = rx };
    spi_device_polling_transmit(s_lcd, &t);
    printf("[lcd] RDID4 = %02X %02X %02X %02X %s\n",
           rx[0], rx[1], rx[2], rx[3],
           (rx[2] == 0x94 && rx[3] == 0x88) ? "-> ILI9488 RESPONDS" :
           "-> NO RESPONSE (check SCK=D8 MOSI=D10 CS=D3 DC=D6 and VCC)");
    return rx[2] == 0x94 && rx[3] == 0x88;
}

/* ---- ladder decoding: same windows as the wiring diagram ----------------- */
typedef struct { const char *name; int lo, hi; } LadderBtn;
static const LadderBtn LADDER[] = {
    { "O  (action)",     0,  350 },
    { "X  (crouch)",   350, 1000 },
    { "T  (triangle)",1000, 1700 },
    { "L1",            1700, 2450 },
    { "START",         2450, 3100 },
    { "SELECT",        3100, 3700 },
};
static int ladder_decode(int raw) {
    for (unsigned i = 0; i < sizeof LADDER / sizeof LADDER[0]; i++)
        if (raw >= LADDER[i].lo && raw < LADDER[i].hi) return i;
    return -1;                                        /* nothing pressed */
}

/* ---- dashboard geometry -------------------------------------------------- */
/* Laid out for 320x240, the panel's real size. The first version was drawn for
 * 480x320 and put the last two button boxes at x=419 -- off the right edge of a
 * 320-wide screen, so two of the eight would simply never have appeared. */
#define STICK_X0 8
#define STICK_Y0 62
#define STICK_SZ 110
#define BTN_Y    186
#define BTN_SZ   32
#define BTN_STEP 39      /* 8 boxes: 8 + 7*39 + 32 = 313, fits in 320 */

static const char *BTN_LABEL[8] = {"O","X","T","L1","ST","SE","A","B"};

/* Wiggle one wire at a time, slowly, and say which one.
 *
 * "Still white" has three very different causes -- logic unpowered, a broken
 * wire, or a module that is not what we think it is -- and nothing said so far
 * separates them. This does: each LCD signal is driven as a plain GPIO at 2 Hz
 * for five seconds while its name is printed. Put a multimeter on DC volts
 * against GND, touch the pin ON THE DISPLAY SIDE, and watch:
 *
 *   swinging 0 <-> 3.3 V  the wire is good all the way to the panel
 *   stuck at 0 or 3.3     that wire is broken, or soldered to the wrong pad
 *   swinging on the XIAO but flat on the display: the wire itself is the fault
 *
 * Measuring at the DISPLAY end is the whole point: it tests the wire, not the
 * chip. Without a meter an LED with a 1k resistor to GND blinks just as well.
 */
static void pin_walk(void) {
    struct { int pin; const char *name; } sig[] = {
        { PIN_SCK,    "SCK  (D8 -> display SCK)"  },
        { PIN_MOSI,   "MOSI (D10 -> display SDI)" },
        { PIN_LCD_CS, "CS   (D3 -> display CS)"   },
        { PIN_LCD_DC, "DC   (D6 -> display DC/RS)"},
    };
    printf("\n=== PIN WALK: measure on the DISPLAY side ===\n");
    for (unsigned i = 0; i < sizeof sig / sizeof sig[0]; i++) {
        gpio_reset_pin(sig[i].pin);
        gpio_set_direction(sig[i].pin, GPIO_MODE_OUTPUT);
        printf("[pin] NOW: %s  -- blinking at 2 Hz for 5 s\n", sig[i].name);
        for (int n = 0; n < 10; n++) {
            gpio_set_level(sig[i].pin, n & 1);
            vTaskDelay(pdMS_TO_TICKS(250));
        }
        gpio_set_level(sig[i].pin, 0);
    }
    printf("=== end of walk; back to display mode ===\n\n");
}

void app_main(void) {
    printf("\n=== MGS CONSOLE TEST (XIAO ESP32S3) ===\n");

    /* 1 -- PSRAM: the game cannot live without it */
    size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    printf("[psram] %u bytes %s\n", (unsigned)psram,
           psram >= 8000000 ? "OK (8 MB)" : psram ? "PRESENT but small" : "MISSING");

    /* 2 -- display.
     *
     * lcd_bus_init() FIRST, and it is easy to lose: an earlier revision split
     * bus setup out of lcd_init() and never added the call, so the SPI bus was
     * never initialised, every transmit failed silently and the panel stayed
     * white -- a firmware bug that looked exactly like bad soldering. The ID
     * probe dutifully reported "no response" and it was right for the wrong
     * reason.
     *
     * Then paint REGARDLESS of what the probe says: the write path is what
     * matters and it is judged with eyes, not with a read many of these
     * modules do not implement. Ten slow cycles, enough to wiggle a wire and
     * watch it change. */
    /* pin_walk() is deliberately NOT run here any more. Wiggling the SPI lines
     * as plain GPIOs made the panel flicker, and that flicker was mistaken for
     * the colour test succeeding -- it cost a round trip. Call it by hand when
     * hunting a broken wire, not on every boot. */
    pin_selftest();      /* can the chip even drive them? before anything else */

    /* Software SPI first: it is the tie-breaker, and it must run before the
     * peripheral claims the pins. */
    /* bb_try(); -- the tie-break is already settled: call it by hand if it is
     * ever needed again to tell a broken wire from a misconfigured SPI */

    lcd_bus_init();

    /* Colour identity card.
     *
     * "Red then green then blue" in sequence asks the viewer to remember an
     * order and report it back, and the answer came back ambiguous -- which is
     * the test's fault, not the viewer's. Three bands at once, each stamped
     * with the letter of the colour it is SUPPOSED to be, needs no memory: if
     * the band marked R looks blue, the panel is reading the channels in the
     * other order and the BGR bit is wrong. One glance settles it. */
    lcd_init_9341();
    lcd_fill(0, 0, W, H, 0, 0, 0);
    { int bh = H / 3;
      lcd_fill(0, 0,      W, bh, 255, 0, 0);
      lcd_fill(0, bh,     W, bh, 0, 255, 0);
      lcd_fill(0, bh * 2, W, bh, 0, 0, 255);
      lcd_text(W/2 - 12, bh/2 - 14,     "R", 4, 255,255,255);
      lcd_text(W/2 - 12, bh + bh/2 - 14, "G", 4, 0,0,0);
      lcd_text(W/2 - 12, bh*2 + bh/2 - 14, "B", 4, 255,255,255);
      lcd_text(4, 4, "R RED   G GREEN   B BLUE", 1, 255,255,255);
    }
    printf("\n[color] three bands: top RED, middle GREEN, bottom BLUE\n");
    printf("[color] if they do not match, the channel order is swapped\n");
    vTaskDelay(pdMS_TO_TICKS(4000));

    /* Controller settled: ILI9341, 320x240, RGB565, channels in the right
     * order -- read off the panel with the three labelled bands, R red, G
     * green, B blue. The alternating ILI9488 probe that used to sit here has
     * done its job and is gone. Worth recording why it confused things: what
     * looked like "greenish blue" was that probe sending three bytes per pixel
     * to a panel expecting two, which shifts every channel by a byte. */
    lcd_init_9341();
    printf("[lcd] ILI9341 320x240 RGB565 confirmed; on to the dashboard\n");
    lcd_fill(0,0,W,H, 0,0,0);

    /* colour bars: if any bar is missing or the order is wrong, the panel is
     * mis-strapped (RGB/BGR) or a data line is bad */
    { unsigned char bars[8][3] = {{255,0,0},{0,255,0},{0,0,255},{0,255,255},
                                  {255,0,255},{255,255,0},{255,255,255},{60,60,60}};
      for (int i = 0; i < 8; i++)
          lcd_fill(i*40, 0, 40, 28, bars[i][0], bars[i][1], bars[i][2]); }

    /* 3 -- microSD: the Sense expansion board's own slot.
     *
     * This is the XIAO ESP32S3 SENSE, and its card slot is already wired --
     * through the board-to-board connector, on GPIO21/47/48/22, none of which
     * appear on the D0..D10 header. That settles the pin budget that had been
     * looming: the card needs no header pin at all, so D4 stays with the
     * panel's reset and nothing has to be given up.
     *
     * It shares the display's SPI bus: Seeed's own documentation for the Sense
     * gives SCK/MISO/MOSI as D8/D9/D10 -- the very pins the panel uses -- with
     * the chip select on GPIO21. An earlier attempt here put the card on its
     * own SPI3 host over GPIO21/47/48/22, from a search result that turned out
     * to be wrong; it failed with ESP_ERR_INVALID_ARG, which is the error for a
     * bad configuration rather than a missing card, and that is what gave it
     * away. Sharing a bus means the two need arbitrating, exactly as the main
     * port already does with its take/give mutex. */
    { esp_vfs_fat_sdmmc_mount_config_t mc = { .max_files = 4 };
      sdmmc_host_t host = SDSPI_HOST_DEFAULT();
      host.slot = SPI2_HOST;               /* the bus the panel already opened */
      host.max_freq_khz = 10000;           /* gentle to start; raise once proven */
      sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
      slot.gpio_cs = 21;                   /* internal to the Sense board */
      slot.host_id = SPI2_HOST;
      sdmmc_card_t *card = NULL;
      esp_err_t err = esp_vfs_fat_sdspi_mount("/sd", &host, &slot, &mc, &card);
      if (err == ESP_OK) {
          printf("[sd] mounted: %llu MB  (%s)\n",
                 (unsigned long long)card->csd.capacity * card->csd.sector_size
                     / (1024*1024),
                 card->cid.name);
          lcd_text(240, 34, "SD OK", 1, 0,255,0);
          /* Does the game's data actually live on it? A mounted card with no
           * MGS folder is a card the port cannot use, and saying so here is
           * cheaper than discovering it from a black screen later. */
          FILE *f = fopen("/sd/MGS/STAGE.DIR", "rb");
          if (f) {
              fseek(f, 0, SEEK_END);
              long n = ftell(f);
              fclose(f);
              printf("[sd] MGS/STAGE.DIR present: %ld bytes\n", n);
              lcd_text(240, 46, "MGS OK", 1, 0,255,0);
          } else {
              printf("[sd] MGS/STAGE.DIR missing -- copy the MGS folder to the root\n");
              lcd_text(240, 46, "NO MGS", 1, 255,160,0);
          }
      } else {
          printf("[sd] FAILED (%s) -- card inserted? formatted as FAT32?\n",
                 esp_err_to_name(err));
          lcd_text(240, 34, "SD BAD", 1, 255,0,0);
      }
    }

    /* 4 -- ADC: stick + ladder */
    adc_oneshot_unit_handle_t adc;
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&ucfg, &adc));
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12,
                                    .bitwidth = ADC_BITWIDTH_12 };
    adc_oneshot_config_channel(adc, ADC_CH_X, &ccfg);
    adc_oneshot_config_channel(adc, ADC_CH_Y, &ccfg);
    adc_oneshot_config_channel(adc, ADC_CH_LAD, &ccfg);

    /* 5 -- direct buttons, internal pull-ups */
    { gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_BTN_A) | (1ULL << PIN_BTN_B),
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
      gpio_config(&io);

      /* Both should read HIGH when nothing is pressed: the internal pull-up
       * does that on its own, wire or no wire. One that reads LOW at rest is
       * either shorted to ground or held by something else -- and B sits on
       * GPIO44, the chip's UART receive pin, which is exactly the sort of pin
       * that can be held. Saying which it is beats guessing from a stuck box
       * on screen. */
      vTaskDelay(pdMS_TO_TICKS(10));
      printf("[btn] at rest: A(D5)=%d  B(D7)=%d   (1 = released, correct)\n",
             gpio_get_level(PIN_BTN_A), gpio_get_level(PIN_BTN_B));
      if (!gpio_get_level(PIN_BTN_B))
          printf("[btn] B reads PRESSED without being touched: its wire touches GND, or\n"
                 "      the switch is wired across two legs of the same pair\n");
    }

    /* static dashboard chrome */
    lcd_text(STICK_X0, STICK_Y0 - 16, "STICK", 1, 200,200,200);
    lcd_fill(STICK_X0-2, STICK_Y0-2, STICK_SZ+4, 2, 90,90,90);
    lcd_fill(STICK_X0-2, STICK_Y0+STICK_SZ, STICK_SZ+4, 2, 90,90,90);
    lcd_fill(STICK_X0-2, STICK_Y0, 2, STICK_SZ, 90,90,90);
    lcd_fill(STICK_X0+STICK_SZ, STICK_Y0, 2, STICK_SZ, 90,90,90);
    lcd_text(132, STICK_Y0 - 16, "LADDER D2", 1, 200,200,200);
    for (int i = 0; i < 8; i++) {
        int bx = STICK_X0 + i * BTN_STEP;
        lcd_fill(bx-2, BTN_Y-2, BTN_SZ+4, 2, 90,90,90);
        lcd_fill(bx-2, BTN_Y+BTN_SZ, BTN_SZ+4, 2, 90,90,90);
        lcd_fill(bx-2, BTN_Y, 2, BTN_SZ, 90,90,90);
        lcd_fill(bx+BTN_SZ, BTN_Y, 2, BTN_SZ, 90,90,90);
        lcd_text(bx + 6, BTN_Y + BTN_SZ + 6, BTN_LABEL[i], 1, 200,200,200);
    }
    printf("[ready] move the stick and press each button; everything prints here\n");
    printf("[legend] boxes: O X T L1 ST SE = ladder | A=D5 B=D7 direct\n");

    int px = -1, py = -1;                 /* previous crosshair, for erase   */
    int prev_state[8] = {0};              /* previous lit state of each box  */
    int prev_lad = -2, prev_x = -1, prev_y = -1;
    int beat = 0;

    for (;;) {
        /* Average eight samples per axis.
         *
         * The ESP32's ADC is noisy enough on its own to make a crosshair
         * twitch, and a stick wired with flying leads adds more. A single
         * reading was making the dot flicker between two places, which reads
         * as a wiring fault and is not one. Eight samples cost nothing at this
         * loop rate and settle it. */
        int rx = 0, ry = 0, rl = 0;
        for (int n = 0; n < 8; n++) {
            int a = 0, b = 0, c = 0;
            adc_oneshot_read(adc, ADC_CH_X, &a);
            adc_oneshot_read(adc, ADC_CH_Y, &b);
            adc_oneshot_read(adc, ADC_CH_LAD, &c);
            rx += a; ry += b; rl += c;
        }
        rx >>= 3; ry >>= 3; rl >>= 3;

        /* Range over a RECENT window, not since boot.
         *
         * The first version accumulated min and max forever, so it reported
         * 0..4095 on both axes -- which looked like proof that both were
         * working and proved nothing at all: at power-up, before anything has
         * settled, a floating input reads both extremes on its own. A window
         * that clears every three seconds describes what the stick is doing
         * NOW, so any capture is meaningful without having to coordinate it
         * with someone's hand. */
        { static int nx = 4095, xx = 0, ny = 4095, xy = 0, beat2 = 0;
          if (rx < nx) nx = rx;
          if (rx > xx) xx = rx;
          if (ry < ny) ny = ry;
          if (ry > xy) xy = ry;
          if ((++beat2 % 25) == 0) {
              /* Name the fault instead of printing numbers to be interpreted.
               * Three readings, three different wiring mistakes, and the
               * difference between them is what the axis DOES, not what it
               * reads: pinned at an end means the ADC wire is on an end
               * terminal rather than the wiper; parked mid-scale and still
               * means that pot has no 3V3/GND across it. */
              const char *dx = (xx - nx) > 200 ? "OK" :
                               (rx > 3900 || rx < 200)
                                 ? "PINNED: D0 is on an end terminal, not the wiper"
                                 : "STILL: that pot is missing 3V3/GND";
              const char *dy = (xy - ny) > 200 ? "OK" :
                               (ry > 3900 || ry < 200)
                                 ? "PINNED: D1 is on an end terminal, not the wiper"
                                 : "STILL: that pot is missing 3V3/GND";
              lcd_fill(0, H - 22, W, 22, 0, 0, 0);
              lcd_text(2, H - 20, (xx-nx) > 200 ? "X OK" : "X BAD", 1,
                       (xx-nx) > 200 ? 0 : 255, (xx-nx) > 200 ? 255 : 80, 0);
              lcd_text(46, H - 20, (xy-ny) > 200 ? "Y OK" : "Y BAD", 1,
                       (xy-ny) > 200 ? 0 : 255, (xy-ny) > 200 ? 255 : 80, 0);
              printf("[stick] X %4d (3s: %4d..%4d = %4d) %s\n"
                     "        Y %4d (3s: %4d..%4d = %4d) %s\n",
                     rx, nx, xx, xx - nx, dx, ry, ny, xy, xy - ny, dy);
              if (beat2 >= 150) {        /* ~3 s at 20 ms per turn */
                  beat2 = 0;
                  nx = xx = rx;
                  ny = xy = ry;
              }
          }
        }
        int lad = ladder_decode(rl);
        int btnA = !gpio_get_level(PIN_BTN_A);        /* active low */
        int btnB = !gpio_get_level(PIN_BTN_B);

        /* Crosshair, calibrated.
         *
         * Mapping the raw 0..4095 straight to the box is wrong for this stick
         * twice over: a drone gimbal rests wherever its springs put it (not at
         * 2048) and its throw covers only part of the scale, so the dot sat
         * off-centre and never reached the edges. Centre is whatever the stick
         * read at boot, and each half is scaled by the travel actually seen in
         * that direction -- so the dot sits dead centre at rest and reaches the
         * corners at full deflection, whatever the hardware's range happens to
         * be. The game will want exactly this, so it is worth getting right
         * here rather than papering over it with a fudge factor.
         *
         * Both axes are negated. Which way a potentiometer counts depends on
         * which of its two end terminals went to 3V3, and on this gimbal both
         * ended up the opposite way round from the screen's sense -- pushing up
         * moved the dot down, pushing right moved it left. Flipping the sign
         * here costs nothing and beats resoldering four wires; if the stick is
         * ever rewired the other way, these two minus signs are the whole
         * change. */
        static int cal_cx, cal_cy, cal_lo_x, cal_hi_x, cal_lo_y, cal_hi_y, cal_done;
        if (!cal_done) {           /* first pass: the stick is at rest */
            cal_cx = rx; cal_cy = ry;
            cal_lo_x = cal_hi_x = rx;
            cal_lo_y = cal_hi_y = ry;
            cal_done = 1;
            printf("[cal] rest center: X %d  Y %d\n", cal_cx, cal_cy);
        }
        if (rx < cal_lo_x) cal_lo_x = rx;
        if (rx > cal_hi_x) cal_hi_x = rx;
        if (ry < cal_lo_y) cal_lo_y = ry;
        if (ry > cal_hi_y) cal_hi_y = ry;

        /* Dead zone, and a span floor that respects it.
         *
         * Measured at rest on this board: the reading wanders about +/-15
         * counts even with nobody touching the stick -- ordinary ADC noise
         * plus flying leads. The first version's span floor of 64 turned that
         * into twelve pixels of jitter, so the crosshair drifted on its own.
         *
         * DEADZONE is four times the observed noise: inside it the stick reads
         * exactly centred, which is what "not touching it" should mean. Beyond
         * it the remaining travel is scaled so the dot still reaches the edge
         * at full deflection -- subtracting the dead zone from both sides of
         * the fraction, rather than just clamping, keeps the motion smooth
         * instead of jumping as it leaves the centre. */
        enum { DEADZONE = 60, SPAN_FLOOR = 500 };
        int half = (STICK_SZ - 8) / 2;
        int dx = rx - cal_cx, dy = ry - cal_cy;
        int spanx = dx >= 0 ? (cal_hi_x - cal_cx) : (cal_cx - cal_lo_x);
        int spany = dy >= 0 ? (cal_hi_y - cal_cy) : (cal_cy - cal_lo_y);
        if (spanx < SPAN_FLOOR) spanx = SPAN_FLOOR;
        if (spany < SPAN_FLOOR) spany = SPAN_FLOOR;

        int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
        int ox = 0, oy = 0;
        if (ax > DEADZONE)
            ox = ((ax - DEADZONE) * half) / (spanx - DEADZONE) * (dx < 0 ? -1 : 1);
        if (ay > DEADZONE)
            oy = ((ay - DEADZONE) * half) / (spany - DEADZONE) * (dy < 0 ? -1 : 1);
        if (ox >  half) ox =  half;
        if (ox < -half) ox = -half;
        if (oy >  half) oy =  half;
        if (oy < -half) oy = -half;
        int cx = STICK_X0 + half - ox;   /* negated: right on the stick, right on screen */
        int cy = STICK_Y0 + half - oy;   /* negated: up on the stick, up on screen    */
        if (cx != px || cy != py) {
            if (px >= 0) lcd_fill(px, py, 8, 8, 0,0,0);
            lcd_fill(cx, cy, 8, 8, 255,255,0);
            px = cx; py = cy;
        }
        if ((beat & 7) == 0 && (rx != prev_x || ry != prev_y)) {
            lcd_fill(STICK_X0, STICK_Y0+STICK_SZ+6, 118, 14, 0,0,0);
            lcd_num(STICK_X0,      STICK_Y0+STICK_SZ+6, rx, 2, 255,255,0);
            lcd_num(STICK_X0+60,   STICK_Y0+STICK_SZ+6, ry, 2, 255,255,0);
            prev_x = rx; prev_y = ry;
        }

        /* ladder bar + raw value; tick marks at the window edges */
        if ((beat & 3) == 0 && rl != prev_lad) {
            int bw = (rl * 180) / 4095;
            lcd_fill(132, STICK_Y0, 180, 14, 30,30,30);
            lcd_fill(132, STICK_Y0, bw, 14, 0,180,255);
            for (unsigned i = 0; i < sizeof LADDER / sizeof LADDER[0]; i++) {
                int tx = 132 + (LADDER[i].hi * 180) / 4095;
                lcd_fill(tx, STICK_Y0, 1, 14, 255,255,255);
            }
            lcd_fill(132, STICK_Y0+22, 70, 14, 0,0,0);
            lcd_num(132, STICK_Y0+22, rl, 2, 0,180,255);
            prev_lad = rl;
        }

        /* the eight boxes: ladder buttons 0..5, then A and B */
        for (int i = 0; i < 8; i++) {
            int on = (i < 6) ? (lad == i) : (i == 6 ? btnA : btnB);
            if (on != prev_state[i]) {
                int bx = STICK_X0 + i * BTN_STEP;
                if (on) lcd_fill(bx, BTN_Y, BTN_SZ, BTN_SZ, 0,220,0);
                else    lcd_fill(bx, BTN_Y, BTN_SZ, BTN_SZ, 0,0,0);
                prev_state[i] = on;
                printf("[button] %s %s (ladder=%d)\n",
                       i < 6 ? LADDER[i].name : (i == 6 ? "A direct D5" : "B direct D7"),
                       on ? "PRESSED" : "released", rl);
            }
        }

        if ((++beat % 100) == 0)
            printf("[raw] stick %4d,%4d ladder %4d A=%d B=%d\n",
                   rx, ry, rl, btnA, btnB);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
