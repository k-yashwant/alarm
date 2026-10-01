#pragma once

#include "esp_err.h"

/* SPI wiring for the microSD module.  MOSO is kept as an alias for the
 * original name; it is the card's DO/MISO pin. */
#define SD_CARD_MISO_GPIO 37
#define SD_CARD_MOSI_GPIO 35
#define SD_CARD_SCK_GPIO  36
#define SD_CARD_CS_GPIO   34
#define SD_CARD_POWER_GPIO 16
#define SD_CARD_SWITCH SD_CARD_POWER_GPIO
#define MOSO SD_CARD_MISO_GPIO
#define MOSI SD_CARD_MOSI_GPIO
#define SCK  SD_CARD_SCK_GPIO
#define CS   SD_CARD_CS_GPIO

/*
 * Mounts an already-FAT32-formatted card, appends one test line to
 * /sdcard/append_test.txt, flushes it to the card, and unmounts it.
 * It never formats the card and never opens a file in a truncating mode.
 */
void sd_card_power_on(void);
void sd_card_power_off(void);
esp_err_t sd_card_append_test(void);
