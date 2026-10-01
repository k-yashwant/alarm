#include "sd_card.h"

#include <stdio.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

static const char *TAG = "SD_CARD";
static const char *MOUNT_POINT = "/sdcard";
static const char *TEST_FILE = "/sdcard/append_test.txt";

void sd_card_power_on(void)
{
    gpio_reset_pin(SD_CARD_POWER_GPIO);
    gpio_set_direction(SD_CARD_POWER_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(SD_CARD_POWER_GPIO, 1);
    ESP_LOGI(TAG, "SD module power ON (GPIO %d = HIGH)", SD_CARD_POWER_GPIO);
}

void sd_card_power_off(void)
{
    gpio_set_level(SD_CARD_POWER_GPIO, 0);
    ESP_LOGI(TAG, "SD module power OFF (GPIO %d = LOW)", SD_CARD_POWER_GPIO);
}

esp_err_t sd_card_append_test(void)
{
    // The module's power switch is active high.  Do not turn it off after
    // the test: it remains powered during normal operation.
    sd_card_power_on();
    // Let the regulator and card reach a stable supply voltage before SPI
    // traffic starts.  Many microSD modules need tens of milliseconds here.
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "Starting append-only FAT32 test: %s", TEST_FILE);

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_CARD_MOSI_GPIO,
        .miso_io_num = SD_CARD_MISO_GPIO,
        .sclk_io_num = SD_CARD_SCK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };

    // Keep chip-select inactive while the card and SPI peripheral initialize.
    gpio_set_direction(SD_CARD_CS_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(SD_CARD_CS_GPIO, 1);

    // These internal pull-ups help when a breakout board has weak/no pull-ups.
    // External 3.3 V pull-ups on SD CS, MOSI, MISO and SCK are still preferred.
    gpio_set_pull_mode(SD_CARD_MISO_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(SD_CARD_MOSI_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(SD_CARD_SCK_GPIO, GPIO_PULLUP_ONLY);

    ESP_LOGI(TAG, "Power settled; initializing SPI: MISO=%d MOSI=%d SCK=%d CS=%d",
             SD_CARD_MISO_GPIO, SD_CARD_MOSI_GPIO,
             SD_CARD_SCK_GPIO, SD_CARD_CS_GPIO);
    esp_err_t err = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_CARD_CS_GPIO;
    slot_config.host_id = host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 1,
        .allocation_unit_size = 0,
    };
    sdmmc_card_t *card = NULL;

    ESP_LOGI(TAG, "Mounting existing FAT32 card at %s (format disabled)", MOUNT_POINT);
    err = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config,
                                  &mount_config, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Mount failed (card is not changed): %s", esp_err_to_name(err));
        spi_bus_free(host.slot);
        return err;
    }
    ESP_LOGI(TAG, "SD card connected and mounted successfully");
    sdmmc_card_print_info(stdout, card);

    FILE *file = fopen(TEST_FILE, "a");
    if (file == NULL) {
        ESP_LOGE(TAG, "Cannot open %s for append", TEST_FILE);
        err = ESP_FAIL;
    } else {
        static const char test_line[] = "ESP32-S2 append-only SD-card test\r\n";
        long previous_size = ftell(file);
        ESP_LOGI(TAG, "Appending %u bytes at file offset %ld", (unsigned)(sizeof(test_line) - 1), previous_size);
        size_t written = fwrite(test_line, 1, sizeof(test_line) - 1, file);
        if (written != sizeof(test_line) - 1 || fflush(file) != 0 || fsync(fileno(file)) != 0) {
            ESP_LOGE(TAG, "Append or sync failed");
            err = ESP_FAIL;
        } else {
            ESP_LOGI(TAG, "Write complete and synced: %u bytes appended to %s", (unsigned)written, TEST_FILE);
            err = ESP_OK;
        }
        fclose(file);
    }

    ESP_LOGI(TAG, "Unmounting SD card");
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
    spi_bus_free(host.slot);
    ESP_LOGI(TAG, "SD append test finished: %s", esp_err_to_name(err));
    return err;
}
