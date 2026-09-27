// SOTN ESP32-S3 — F3 scanout: g_RawVram display area -> ST7789 over SPI DMA.
//
// Same proven pipeline as the Descent/OpenLara ports on this board: 16-line
// ping-pong chunks, conversion overlapped against the in-flight transfer.
// Source is PSX RGB5551 (R in bits 0-4, G 5-9, B 10-14) with a 1024-px row
// stride; output is byteswapped RGB565. The PSX 256-px-wide frame sits
// pillarboxed in the 320-px panel (black bars pushed once at init).

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_attr.h"

#include "board_pins.h"
#ifdef MGS_BOARD_XIAO
#include "esp_lcd_ili9341.h"
#endif

/* The PSX resolution this game actually renders at.
 *
 * 256 is SOTN's, and this file came from that port. MGS runs 320x240
 * (SCREEN_WIDTH/SCREEN_HEIGHT in source/include/common.h), so scanning out 256
 * columns threw away 64 of them -- 20% of the picture -- and PILLAR_X then
 * centred what was left, putting a 32-pixel black band down each side. Walking
 * towards either edge, the player disappeared into it. */
#define PSX_W 320
#define PSX_H 240
#define PILLAR_X ((320 - PSX_W) / 2)
#define VRAM_STRIDE 1024

#define CHUNK_LINES 8  // 8 lines: 5KB per band, leaves DRAM for the SEL stage
#define CHUNK_BYTES (PSX_W * CHUNK_LINES * 2)

static const char* TAG = "lcd";

// F5-flicker: the scan task DMAs from a SNAPSHOT, never from live VRAM, so
// nothing the game writes mid-transfer can reach the panel. sFreeze holds
// the last snapshot (diagnostic: if a frozen image still flickers, the
// artifact is panel-side, not content-side).
static EXT_RAM_BSS_ATTR uint16_t sSnap[2][PSX_H][PSX_W];
static QueueHandle_t sScanQ; // snapshot index handed to the DMA stage
volatile int sotn_lcd_freeze;
// 0 = DMA straight from the finished VRAM frame (no copy); 1 = safety copy
volatile int sotn_lcd_snapshot;
unsigned sotn_snap_cycles;

static esp_lcd_panel_handle_t sPanel;
static esp_lcd_panel_io_handle_t sIO;
static SemaphoreHandle_t sTransDone;
// signalled by the scan task once the frame snapshot is safely copied
static SemaphoreHandle_t sSnapDone;
static uint16_t* sChunk[2];

/* The panel and the microSD are two devices on ONE bus (SPI2), and they cannot
 * be left to the SPI driver's own arbitration here.
 *
 * esp_lcd_panel_io_spi is asynchronous: draw_bitmap queues a transaction and
 * returns, so a frame is 30 queued transfers that outlive the call. The driver
 * hands the bus over between devices assuming the previous one is finished,
 * and asserts spi_ll_get_running_cmd(hw) == 0 when it is not. With an empty
 * card nothing ever read from it and the two never overlapped; the moment the
 * card carried the stage data, the loader started reading sectors in the
 * middle of a frame and the board went into a reboot loop on that assert.
 *
 * So the bus is owned explicitly: a whole frame, or a whole SD read, at a
 * time. Frames are chunked into 8-line bands and a stage read is capped at 16
 * sectors, so neither side holds it long enough to starve the other. */
static SemaphoreHandle_t sBusMux;

void Mgs_SpiBusTake(void) {
    if (sBusMux) {
        xSemaphoreTake(sBusMux, portMAX_DELAY);
    }
}

void Mgs_SpiBusGive(void) {
    if (sBusMux) {
        xSemaphoreGive(sBusMux);
    }
}

static bool IRAM_ATTR onTransDone(esp_lcd_panel_io_handle_t io,
                                  esp_lcd_panel_io_event_data_t* ev,
                                  void* user) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(sTransDone, &woken);
    return woken == pdTRUE;
}

int lcd_init(void) {
    /* The backlight may not have a pin at all. On the handheld it is strapped
     * to 3V3 and PIN_LCD_BL is -1; shifting by a negative count is undefined
     * behaviour, so the mask has to be built conditionally rather than just
     * OR-ing both pins in. */
    uint64_t out_pins = 1ULL << PIN_SD_CS;
#if PIN_LCD_BL >= 0
    out_pins |= 1ULL << PIN_LCD_BL;
#endif
    gpio_config_t bl = { .pin_bit_mask = out_pins, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&bl);
#if PIN_LCD_BL >= 0
    gpio_set_level(PIN_LCD_BL, !LCD_BL_ON_LEVEL);   // dark until the first frame
#endif
    gpio_set_level(PIN_SD_CS, 1); // keep the microSD off the shared bus

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_LCD_SCLK,
        .mosi_io_num = PIN_LCD_MOSI,
        .miso_io_num = PIN_SD_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 320 * CHUNK_LINES * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = PIN_LCD_DC,
        .cs_gpio_num = PIN_LCD_CS,
        .pclk_hz = LCD_SPI_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 8,
        .on_color_trans_done = onTransDone,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &sIO));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ORDER,
        .bits_per_pixel = 16,
    };
#ifdef MGS_BOARD_XIAO
    /* The handheld's panel is an ILI9341, which ESP-IDF does not ship a driver
     * for -- only ST7789 and NT35510 are built in -- so it comes from the
     * component registry (see idf_component.yml). The two controllers share
     * most of the MIPI command set and it is tempting to drive one with the
     * other's driver; they differ in the init sequence and in whether display
     * inversion is on, which is exactly the sort of "nearly works" that costs
     * an evening. */
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(sIO, &panel_cfg, &sPanel));
#else
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(sIO, &panel_cfg, &sPanel));
#endif

    ESP_ERROR_CHECK(esp_lcd_panel_reset(sPanel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(sPanel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(sPanel, LCD_INVERT_COLOR));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(sPanel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(sPanel, LCD_MIRROR_X, LCD_MIRROR_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(sPanel, LCD_GAP_X, LCD_GAP_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(sPanel, true));

    sBusMux = xSemaphoreCreateMutex();
    sTransDone = xSemaphoreCreateCounting(2, 2);
    sSnapDone = xSemaphoreCreateBinary();
    sScanQ = xQueueCreate(1, sizeof(int));
    for (int i = 0; i < 2; i++) {
        sChunk[i] = heap_caps_malloc(320 * CHUNK_LINES * 2,
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!sChunk[i]) {
            ESP_LOGE(TAG, "no DMA memory for chunk buffers");
            return -1;
        }
    }

    // full-width black frame: paints the pillarbox bars, then backlight on
    memset(sChunk[0], 0, 320 * CHUNK_LINES * 2);
    for (int y = 0; y < 240; y += CHUNK_LINES) {
        xSemaphoreTake(sTransDone, portMAX_DELAY);
        esp_lcd_panel_draw_bitmap(sPanel, 0, y, 320, y + CHUNK_LINES,
                                  sChunk[0]);
    }
    for (int i = 0; i < 2; i++) xSemaphoreTake(sTransDone, portMAX_DELAY);
    for (int i = 0; i < 2; i++) xSemaphoreGive(sTransDone);
#if PIN_LCD_BL >= 0
    gpio_set_level(PIN_LCD_BL, LCD_BL_ON_LEVEL);
#endif

    ESP_LOGI(TAG, "%s up: %d Hz SPI, %d-line chunks",
#ifdef MGS_BOARD_XIAO
             "ILI9341",
#else
             "ST7789",
#endif
             LCD_SPI_HZ, CHUNK_LINES);
    return 0;
}

// same conversion but reading VRAM directly (1024-px stride)
static inline void convertChunkStride(const uint16_t* restrict src,
                                      uint16_t* restrict dst, int rows);

// PSX 5551 -> byteswapped RGB565, two pixels per 32-bit store
static inline void convertChunk(const uint16_t* restrict src,
                                uint16_t* restrict dst, int rows) {
    uint32_t* restrict d = (uint32_t*)dst;
    for (int y = 0; y < rows; y++) {
        const uint16_t* row = src + y * PSX_W;
        for (int x = 0; x < PSX_W; x += 2) {
            uint16_t c0 = row[x], c1 = row[x + 1];
            uint16_t p0 = ((c0 & 0x1F) << 11) | (((c0 >> 5) & 0x1F) << 6) |
                          ((c0 >> 10) & 0x1F);
            uint16_t p1 = ((c1 & 0x1F) << 11) | (((c1 >> 5) & 0x1F) << 6) |
                          ((c1 >> 10) & 0x1F);
            p0 = (p0 >> 8) | (p0 << 8);
            p1 = (p1 >> 8) | (p1 << 8);
            *d++ = (uint32_t)p0 | ((uint32_t)p1 << 16);
        }
    }
}

// src points at the top-left of the finished frame inside g_RawVram.
// The rows outside the stage clip are NOT overscan: the HUD lives there,
// so everything gets sent as the game drew it.
static const uint16_t* sDirectSrc;
/* Say when the picture goes blank, and what colour it went.
 *
 * "It went white" has been arriving as a sentence from the far side of the
 * panel, minutes after the last telemetry block, so the state at the moment it
 * happened has never been in a log. The frame about to be sent is right here
 * and its content is a fact, so let the board report it: sample a grid, and if
 * almost every sample is the same colour, print that colour once per episode
 * along with how long it lasted.
 *
 * The colour matters as much as the event. 0x7FFF is white drawn deliberately;
 * a strange non-white constant is a fill or a clear gone wrong; alternating
 * values are the reader desyncing again. Each points somewhere different. */
static void watchBlankFrame(const uint16_t* src, int stride) {
    enum { GX = 32, GY = 24, N = GX * GY };
    unsigned same = 0;
    /* Compare against the CENTRE, not pixel 0, and at 60% rather than 95%.
     * The first version asked whether the whole frame was one colour and so
     * said nothing about the case actually being reported: the HUD, the credit
     * text and the edges of the room all stayed visible while something large
     * and bright covered the middle. A test that only fires on a perfectly
     * uniform frame cannot see a thing painted OVER a scene, which is exactly
     * what a searchlight cone is. */
    uint16_t first = src[(PSX_H / 2) * stride + PSX_W / 2];
    static unsigned blank_run, episodes;

    for (int gy = 0; gy < GY; gy++) {
        const uint16_t* row = src + (gy * (PSX_H / GY)) * stride;
        for (int gx = 0; gx < GX; gx++) {
            if (row[gx * (PSX_W / GX)] == first) same++;
        }
    }

    if (same * 100u >= N * 60u) {
        /* Black is the ordinary between-stages clear and says nothing. */
        if (first == 0) return;
        if (blank_run++ == 0 && episodes < 24) {
            episodes++;
            printf("[blank] frame is %u%% colour 0x%04X\n",
                   same * 100u / N, first);
        }
    } else if (blank_run) {
        if (episodes <= 24) {
            printf("[blank] ended after %u frames\n", blank_run);
        }
        blank_run = 0;
    }
}

static void presentSync(int snapIdx) {
    int buf = 0;
    if (sotn_lcd_snapshot) {
        watchBlankFrame(&sSnap[snapIdx][0][0], PSX_W);
    } else {
        watchBlankFrame(sDirectSrc, VRAM_STRIDE);
    }
    Mgs_SpiBusTake();
    for (int y = 0; y < PSX_H; y += CHUNK_LINES) {
        xSemaphoreTake(sTransDone, portMAX_DELAY);
        if (sotn_lcd_snapshot) {
            convertChunk(&sSnap[snapIdx][y][0], sChunk[buf], CHUNK_LINES);
        } else {
            convertChunkStride(sDirectSrc + y * VRAM_STRIDE, sChunk[buf],
                               CHUNK_LINES);
        }
        esp_lcd_panel_draw_bitmap(sPanel, PILLAR_X, y, PILLAR_X + PSX_W,
                                  y + CHUNK_LINES, sChunk[buf]);
        buf ^= 1;
    }
    /* The last two transfers are still queued: the bus is not free until they
     * land, and handing it to the card before then is exactly what tripped the
     * driver's assert. Drain, then restore the counting semaphore. */
    for (int i = 0; i < 2; i++) xSemaphoreTake(sTransDone, portMAX_DELAY);
    for (int i = 0; i < 2; i++) xSemaphoreGive(sTransDone);
    Mgs_SpiBusGive();
}

// F4: scanout runs on core 1 so the game loop on core 0 never waits for SPI.
// The game double-buffers in VRAM (display area != draw area), so converting
// the displayed buffer while the other one is being drawn is tear-free by
// construction — same contract as the real PSX CRT scanout.
static TaskHandle_t sScanTask;
static const uint16_t* volatile sScanSrc;

static inline void convertChunkStride(const uint16_t* restrict src,
                                      uint16_t* restrict dst, int rows) {
    uint32_t* restrict d = (uint32_t*)dst;
    for (int y = 0; y < rows; y++) {
        const uint16_t* row = src + y * VRAM_STRIDE;
        for (int x = 0; x < PSX_W; x += 2) {
            uint16_t c0 = row[x], c1 = row[x + 1];
            uint16_t p0 = ((c0 & 0x1F) << 11) | (((c0 >> 5) & 0x1F) << 6) |
                          ((c0 >> 10) & 0x1F);
            uint16_t p1 = ((c1 & 0x1F) << 11) | (((c1 >> 5) & 0x1F) << 6) |
                          ((c1 >> 10) & 0x1F);
            p0 = (p0 >> 8) | (p0 << 8);
            p1 = (p1 >> 8) | (p1 << 8);
            *d++ = (uint32_t)p0 | ((uint32_t)p1 << 16);
        }
    }
}

static void dmaTask(void* arg) {
    (void)arg;
    for (;;) {
        int idx;
        if (xQueueReceive(sScanQ, &idx, portMAX_DELAY) == pdTRUE) {
            presentSync(idx);
        }
    }
}

// Copy stage: takes the coherent snapshot and releases the game immediately,
// then hands the buffer to the DMA stage. Two snapshots mean the copy never
// waits for the panel transfer of the previous frame (that serialisation
// cost 10ms/frame when both lived in one task).
static void copyTask(void* arg) {
    (void)arg;
    int cur = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const uint16_t* src = sScanSrc;
        if (!src) {
            continue;
        }
        if (!sotn_lcd_freeze && sotn_lcd_snapshot) {
            extern void Soft_VisibleRows(int* y0, int* y1);
            int vy0, vy1;
            Soft_VisibleRows(&vy0, &vy1);
            if (vy0 < 0) vy0 = 0;
            if (vy1 > PSX_H - 1) vy1 = PSX_H - 1;
            unsigned t0, t1;
            __asm__ volatile("rsr.ccount %0" : "=a"(t0));
            for (int y = vy0; y <= vy1; y++) {
                memcpy(&sSnap[cur][y][0], src + y * VRAM_STRIDE, PSX_W * 2);
            }
            __asm__ volatile("rsr.ccount %0" : "=a"(t1));
            sotn_snap_cycles += t1 - t0;
        }
        sDirectSrc = src;
        xSemaphoreGive(sSnapDone); // the game may draw again
        int idx = cur;
        if (xQueueSend(sScanQ, &idx, 0) == pdTRUE) {
            cur ^= 1; // only flip when the DMA stage accepted this one
        }
    }
}

void lcd_present(const uint16_t* src) {
    if (!sChunk[0]) {
        return; // init failed or not run: stay headless
    }
    if (!sScanTask) {
        xTaskCreatePinnedToCore(dmaTask, "lcd_dma", 4096, NULL, 4, NULL, 1);
        xTaskCreatePinnedToCore(copyTask, "lcd_copy", 4096, NULL, 6,
                                &sScanTask, 1);
    }
    sScanSrc = src;
    // pending notifications coalesce: if core 1 is mid-frame we simply drop
    // to its pace instead of queueing stale frames
    xTaskNotifyGive(sScanTask);
}

// Called by the game thread after its frame-pacing delay: the copy on core 1
// normally finished long before (it overlaps the wait), so this is free on
// light frames and only costs the leftover on heavy ones - but it guarantees
// the game never draws into VRAM while the snapshot is still being taken.
void lcd_wait_snapshot(void) {
    if (sSnapDone && sScanTask) {
        xSemaphoreTake(sSnapDone, pdMS_TO_TICKS(50));
    }
}
