/* Metal Gear Solid on the ESP32-S3 — entry point.
 *
 * The game's own main() lives in source/main/main.c and is renamed at compile
 * time so it can be called from here. Everything the PSX kernel used to provide
 * is in ../port/: the platform hooks, the software GPU, and the thread model on
 * FreeRTOS.
 */

#include <stdio.h>
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"

int mgs_main(void);                  /* source/main/main.c, renamed */
void Mgs_SetDataRoot(const char* p); /* port/platform_headless.c */
int lcd_init(void);                  /* lcd_esp32.c, from the SOTN port */
int Mgs_SdCardInit(void);            /* esp_sd.c -- the game has its
                                        own sd_init for the sound driver */

void app_main(void) {
    printf("\n=== Metal Gear Solid / ESP32-S3 ===\n");
    printf("internal free: %u B   PSRAM free: %u B\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* the game opens "cdrom:\MGS\NAME;1"; mount the data partition and point
     * the path translator at it */
    {
        /* fatfsgen.py produces a read-only image, and the game only reads its
         * assets -- mounting read-write fails with "No filesystem detected". */
        const esp_vfs_fat_mount_config_t cfg = {
            .max_files = 8,
            .format_if_mount_failed = false,
        };
        esp_err_t err =
            esp_vfs_fat_spiflash_mount_ro("/flash", "storage", &cfg);
        if (err != ESP_OK) {
            printf("mgs: data partition mount failed (%s) -- flash "
                   "mgs_data.bin at 0x610000\n",
                   esp_err_to_name(err));
        } else {
            printf("mgs: data partition mounted at /flash\n");
        }
    }
    Mgs_SetDataRoot("/flash");

    /* bring up the panel before the game starts drawing: Psyz_VideoVSync
     * pushes each finished frame through lcd_present() */
    if (lcd_init() != 0) {
        printf("mgs: LCD init FAILED\n");
    } else {
        printf("mgs: LCD ready\n");
    }

    /* Paint a marker into VRAM before the game touches it. If this reaches the
     * panel, the whole display chain -- rasterizer memory, scanout origin, SPI
     * driver -- is proven good, and a black screen afterwards can only mean the
     * game has not drawn yet. Without it, "black" is ambiguous. */
    {
        extern unsigned short g_RawVram[];
        int y, x;
        for (y = 0; y < 240; y++) {
            for (x = 0; x < 320; x++) {
                /* BGR555: a blue-to-green ramp with a white grid every 32 px */
                unsigned short c = (unsigned short)(((y >> 3) & 31) << 5 |
                                                    ((x >> 3) & 31));
                if ((x % 32) == 0 || (y % 32) == 0) {
                    c = 0x7FFF;
                }
                g_RawVram[y * 1024 + x] = c;
            }
        }
        printf("mgs: test pattern painted into VRAM\n");
    }

    /* The card shares SPI2 with the panel, so it can only be brought up once
     * lcd_init has created the bus. When one is present the virtual CD reads
     * its stage data from there instead of the flash partition. */
    Mgs_SdCardInit();

    /* the vblank the PSX gave for free; mts blocks on it during boot.
     * It also drives the panel, so VRAM reaches the screen continuously. */
    {
        void Mgs_StartVblank(void);
        Mgs_StartVblank();
    }

    /* Pull the disc data into PSRAM here, on the main task: it is the last
     * moment when nothing else is running, and after this point the port
     * never touches flash again. */
    extern void Mgs_CdInit(void);
    Mgs_CdInit();

    printf("mgs: entering main()\n");
    mgs_main();
    printf("mgs: main() returned\n");
}
