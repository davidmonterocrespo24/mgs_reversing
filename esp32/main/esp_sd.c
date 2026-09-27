/* microSD mount, so the disc data is not limited to the 8 MB flash partition.
 *
 * The retail STAGE.DIR is 68.6 MB across 96 stage blocks. Trimmed to what fits
 * in flash it holds nine, and PSRAM runs out before flash does because the
 * virtual CD copies everything into it at boot. A card removes both limits at
 * once: sectors are read on demand, so the data costs no PSRAM at all, and --
 * unlike the flash partition -- reading it does not disable the instruction
 * cache both cores execute from, which is the hazard that forced the copy in
 * the first place. Anything the game can address then fits.
 *
 * The card shares SPI2 with the LCD; lcd_init brings the bus up and parks
 * SD_CS high before the first panel byte, so only the sdspi device is added
 * here and the SPI driver serialises the two. Mounting is best effort: with no
 * card the virtual CD falls back to the flash partition and the board plays
 * whatever was packed into it.
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "board_pins.h"

int Mgs_SdCardInit(void) {
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = LCD_SPI_HOST;
    /* 20 MHz, and it is not conservatism -- it is measured. 40 MHz was tried
     * and every probe came back ESP_ERR_INVALID_CRC, so the card never mounted
     * and the board silently fell back to the nine stages in flash. That looked
     * like a huge speed-up (loads were suddenly reading PSRAM) which is exactly
     * how a wrong number survives a benchmark. This bus is shared with the
     * panel and its traces are not tuned for high-speed SD; leave it here.
     * Load time is won by reading in bigger bursts instead -- see cd_pump in
     * port/virtual_cd.c. */
    host.max_freq_khz = 20000;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_SD_CS;
    slot.host_id = LCD_SPI_HOST;

    esp_vfs_fat_sdmmc_mount_config_t cfg = {
        .format_if_mount_failed = false,
        .max_files = 8,
    };
    /* Retry: a card can need a couple of hundred milliseconds after power-up
     * before it answers CMD0 at all, and this runs early. A single probe that
     * lands inside that window reports ESP_ERR_TIMEOUT and the board silently
     * falls back to the nine stages in flash -- which looks exactly like "the
     * card is not working" when it simply was not ready yet. */
    sdmmc_card_t* card = NULL;
    esp_err_t err = ESP_FAIL;
    /* Be patient. The mount is intermittent on this board: the same card and
     * the same firmware sometimes come up first try and sometimes fail every
     * probe with ESP_ERR_TIMEOUT inside send_op_cond, and a board that falls
     * back to flash looks like a completely different bug -- nine stages
     * instead of ninety-six, and loads that appear ten times faster because
     * they are coming out of PSRAM. Wait before the FIRST attempt as well:
     * the card shares its rail with the panel, which lcd_init has just brought
     * up, and cards can want far longer than the spec's 1 ms to settle. */
    vTaskDelay(pdMS_TO_TICKS(250));
    for (int attempt = 0; attempt < 6; attempt++) {
        if (attempt) {
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        err = esp_vfs_fat_sdspi_mount("/sd", &host, &slot, &cfg, &card);
        if (err == ESP_OK) {
            break;
        }
        printf("mgs: microSD probe %d/6 failed (%s)\n", attempt + 1,
               esp_err_to_name(err));
    }
    if (err != ESP_OK) {
        printf("mgs: no microSD -- using the flash partition\n");
        return -1;
    }
    printf("mgs: microSD mounted at /sd (%llu MB)\n",
           ((unsigned long long)card->csd.capacity * card->csd.sector_size) /
               (1024 * 1024));
    return 0;
}
