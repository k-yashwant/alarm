#ifndef OLED_DISPLAY_H
#define OLED_DISPLAY_H

#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

#define OLED_POWER_GPIO 4
#define OLED_I2C_SCL_GPIO 1
#define OLED_I2C_SDA_GPIO 44

esp_err_t oled_display_init(void);
void oled_display_on(void);
void oled_display_off(void);
void oled_display_set_alarm_active(bool active);
void oled_display_set_next_alarm(time_t alarm_time);
void oled_display_set_wifi_connected(bool connected);
void oled_display_show_time(time_t now);

#endif
