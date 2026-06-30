#include "oled_display.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "system_events.h"

static const char *TAG = "OLED_DISPLAY";

#define OLED_I2C_BUS_PORT 0
#define OLED_I2C_HW_ADDR 0x3C
#define OLED_PIXEL_CLOCK_HZ (400 * 1000)
#define OLED_H_RES 128
#define OLED_V_RES 64
#define OLED_RST_GPIO -1
#define OLED_CMD_BITS 8
#define OLED_PARAM_BITS 8
#define OLED_ROTATE_180 1
#define CHARGE_STATUS_GPIO 3

static i2c_master_bus_handle_t s_i2c_bus;
static esp_lcd_panel_io_handle_t s_io_handle;
static esp_lcd_panel_handle_t s_panel_handle;
static TaskHandle_t s_time_task_handle;
static bool s_initialized;
static bool s_powered;
static bool s_alarm_active;
static time_t s_next_alarm_time;
static uint8_t s_framebuffer[OLED_H_RES * OLED_V_RES / 8];

static const uint8_t s_font_5x7[][5] = {
    [' '] = {0x00, 0x00, 0x00, 0x00, 0x00},
    ['!'] = {0x00, 0x00, 0x5f, 0x00, 0x00},
    ['"'] = {0x00, 0x07, 0x00, 0x07, 0x00},
    ['#'] = {0x14, 0x7f, 0x14, 0x7f, 0x14},
    ['$'] = {0x24, 0x2a, 0x7f, 0x2a, 0x12},
    ['%'] = {0x23, 0x13, 0x08, 0x64, 0x62},
    ['&'] = {0x36, 0x49, 0x55, 0x22, 0x50},
    ['\''] = {0x00, 0x05, 0x03, 0x00, 0x00},
    ['('] = {0x00, 0x1c, 0x22, 0x41, 0x00},
    [')'] = {0x00, 0x41, 0x22, 0x1c, 0x00},
    ['*'] = {0x14, 0x08, 0x3e, 0x08, 0x14},
    ['+'] = {0x08, 0x08, 0x3e, 0x08, 0x08},
    [','] = {0x00, 0x50, 0x30, 0x00, 0x00},
    ['-'] = {0x08, 0x08, 0x08, 0x08, 0x08},
    ['.'] = {0x00, 0x60, 0x60, 0x00, 0x00},
    ['/'] = {0x20, 0x10, 0x08, 0x04, 0x02},
    ['0'] = {0x3e, 0x51, 0x49, 0x45, 0x3e},
    ['1'] = {0x00, 0x42, 0x7f, 0x40, 0x00},
    ['2'] = {0x42, 0x61, 0x51, 0x49, 0x46},
    ['3'] = {0x21, 0x41, 0x45, 0x4b, 0x31},
    ['4'] = {0x18, 0x14, 0x12, 0x7f, 0x10},
    ['5'] = {0x27, 0x45, 0x45, 0x45, 0x39},
    ['6'] = {0x3c, 0x4a, 0x49, 0x49, 0x30},
    ['7'] = {0x01, 0x71, 0x09, 0x05, 0x03},
    ['8'] = {0x36, 0x49, 0x49, 0x49, 0x36},
    ['9'] = {0x06, 0x49, 0x49, 0x29, 0x1e},
    [':'] = {0x00, 0x36, 0x36, 0x00, 0x00},
    [';'] = {0x00, 0x56, 0x36, 0x00, 0x00},
    ['<'] = {0x08, 0x14, 0x22, 0x41, 0x00},
    ['='] = {0x24, 0x24, 0x24, 0x24, 0x24},
    ['>'] = {0x00, 0x41, 0x22, 0x14, 0x08},
    ['?'] = {0x02, 0x01, 0x51, 0x09, 0x06},
    ['@'] = {0x32, 0x49, 0x79, 0x41, 0x3e},
    ['A'] = {0x7e, 0x11, 0x11, 0x11, 0x7e},
    ['B'] = {0x7f, 0x49, 0x49, 0x49, 0x36},
    ['C'] = {0x3e, 0x41, 0x41, 0x41, 0x22},
    ['D'] = {0x7f, 0x41, 0x41, 0x22, 0x1c},
    ['E'] = {0x7f, 0x49, 0x49, 0x49, 0x41},
    ['F'] = {0x7f, 0x09, 0x09, 0x09, 0x01},
    ['G'] = {0x3e, 0x41, 0x49, 0x49, 0x7a},
    ['H'] = {0x7f, 0x08, 0x08, 0x08, 0x7f},
    ['I'] = {0x00, 0x41, 0x7f, 0x41, 0x00},
    ['J'] = {0x20, 0x40, 0x41, 0x3f, 0x01},
    ['K'] = {0x7f, 0x08, 0x14, 0x22, 0x41},
    ['L'] = {0x7f, 0x40, 0x40, 0x40, 0x40},
    ['M'] = {0x7f, 0x02, 0x0c, 0x02, 0x7f},
    ['N'] = {0x7f, 0x04, 0x08, 0x10, 0x7f},
    ['O'] = {0x3e, 0x41, 0x41, 0x41, 0x3e},
    ['P'] = {0x7f, 0x09, 0x09, 0x09, 0x06},
    ['Q'] = {0x3e, 0x41, 0x51, 0x21, 0x5e},
    ['R'] = {0x7f, 0x09, 0x19, 0x29, 0x46},
    ['S'] = {0x46, 0x49, 0x49, 0x49, 0x31},
    ['T'] = {0x01, 0x01, 0x7f, 0x01, 0x01},
    ['U'] = {0x3f, 0x40, 0x40, 0x40, 0x3f},
    ['V'] = {0x1f, 0x20, 0x40, 0x20, 0x1f},
    ['W'] = {0x7f, 0x20, 0x18, 0x20, 0x7f},
    ['X'] = {0x63, 0x14, 0x08, 0x14, 0x63},
    ['Y'] = {0x07, 0x08, 0x70, 0x08, 0x07},
    ['Z'] = {0x61, 0x51, 0x49, 0x45, 0x43},
    ['~'] = {0x0c, 0x0e, 0x1b, 0x38, 0x70}, // Lightning bolt (thunder symbol)
};

static void oled_set_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= OLED_H_RES || y < 0 || y >= OLED_V_RES) {
        return;
    }

#if OLED_ROTATE_180
    x = OLED_H_RES - 1 - x;
    y = OLED_V_RES - 1 - y;
#endif

    uint8_t *byte = &s_framebuffer[(y / 8) * OLED_H_RES + x];
    uint8_t bit = 1 << (y % 8);
    if (on) {
        *byte |= bit;
    } else {
        *byte &= ~bit;
    }
}

static void string_to_upper(char *str)
{
    while (*str) {
        if (*str >= 'a' && *str <= 'z') {
            *str = *str - 'a' + 'A';
        }
        str++;
    }
}

static void oled_draw_char(int x, int y, char c, int scale)
{
    uint8_t idx = (uint8_t)c;
    size_t font_size = sizeof(s_font_5x7) / sizeof(s_font_5x7[0]);
    if (idx >= font_size) {
        return;
    }

    const uint8_t *glyph = s_font_5x7[idx];

    for (int col = 0; col < 5; col++) {
        for (int row = 0; row < 7; row++) {
            bool on = glyph[col] & (1 << row);
            for (int sx = 0; sx < scale; sx++) {
                for (int sy = 0; sy < scale; sy++) {
                    oled_set_pixel(x + col * scale + sx, y + row * scale + sy, on);
                }
            }
        }
    }
}

static void oled_draw_text(int x, int y, const char *text, int scale)
{
    while (*text) {
        oled_draw_char(x, y, *text, scale);
        x += 6 * scale;
        text++;
    }
}

static void oled_flush(void)
{
    if (!s_initialized || !s_powered) {
        return;
    }

    esp_lcd_panel_draw_bitmap(s_panel_handle, 0, 0, OLED_H_RES, OLED_V_RES, s_framebuffer);
}

static void oled_time_task(void *arg)
{
    while (1) {
        if (s_powered) {
            time_t now;
            time(&now);
            oled_display_show_time(now);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

#define BAT_ADC_UNIT           ADC_UNIT_2
#define BAT_ADC_CHANNEL        ADC_CHANNEL_0  // GPIO 11 is ADC2_CHANNEL_0
#define BAT_CTRL_GPIO          12             // GPIO 12 switches the divider

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_cali_handle = NULL;
static bool s_adc_initialized = false;
static bool s_cali_initialized = false;
static int s_cached_battery_percentage = -1;
static time_t s_last_battery_read_time = 0;

static void init_battery_adc(void) {
    // Configure GPIO 12 as output
    gpio_reset_pin(BAT_CTRL_GPIO);
    gpio_set_direction(BAT_CTRL_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BAT_CTRL_GPIO, 0); // Keep it OFF initially

    // Initialize ADC2
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = BAT_ADC_UNIT,
        .clk_src = 0,
    };
    esp_err_t err = adc_oneshot_new_unit(&init_config, &s_adc_handle);
    if (err == ESP_OK) {
        adc_oneshot_chan_cfg_t config = {
            .bitwidth = ADC_BITWIDTH_13,
            .atten = ADC_ATTEN_DB_0,
        };
        err = adc_oneshot_config_channel(s_adc_handle, BAT_ADC_CHANNEL, &config);
        if (err == ESP_OK) {
            s_adc_initialized = true;
        }
    }

    // Initialize Calibration
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = BAT_ADC_UNIT,
        .atten = ADC_ATTEN_DB_0,
        .bitwidth = ADC_BITWIDTH_13,
    };
    if (adc_cali_create_scheme_line_fitting(&cali_config, &s_cali_handle) == ESP_OK) {
        s_cali_initialized = true;
    }
#endif
}

int read_battery_percentage(void) {
    if (!s_adc_initialized) {
        return -1;
    }

    // Skip ADC2 read if WiFi is active — ADC2 is shared with the WiFi hardware.
    // Reading during WiFi operation will panic the chip.
    if (s_wifi_event_group != NULL) {
        EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGD(TAG, "WiFi active, skipping ADC2 battery read");
            return s_cached_battery_percentage; // return last known value
        }
    }

    // 1. Turn ON the voltage divider circuit
    gpio_set_level(BAT_CTRL_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(5));

    // 2. Read the raw ADC value
    int raw_val = 0;
    int samples = 10;
    int sum = 0;
    int valid_samples = 0;
    for (int i = 0; i < samples; i++) {
        int val;
        esp_err_t err = adc_oneshot_read(s_adc_handle, BAT_ADC_CHANNEL, &val);
        if (err == ESP_OK) {
            sum += val;
            valid_samples++;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    // 3. Turn OFF the voltage divider circuit to save power
    gpio_set_level(BAT_CTRL_GPIO, 0);

    if (valid_samples == 0) {
        ESP_LOGE(TAG, "ADC read failed (Wi-Fi might be active)");
        return -1;
    }
    raw_val = sum / valid_samples;

    // 4. Convert raw reading to millivolts
    int voltage_mv = 0;
    if (s_cali_initialized) {
        adc_cali_raw_to_voltage(s_cali_handle, raw_val, &voltage_mv);
    } else {
        voltage_mv = (raw_val * 750) / 8191;
    }

    // 5. Convert to battery voltage (divider ratio: (51k + 10k) / 10k = 6.1)
    int bat_voltage_mv = (int)(voltage_mv * 6.1);
    ESP_LOGI(TAG, "Measured battery voltage: %d.%03d V (ADC: %d mV, raw: %d), charging status GPIO3: %d", 
             bat_voltage_mv / 1000, bat_voltage_mv % 1000, voltage_mv, raw_val, gpio_get_level(CHARGE_STATUS_GPIO));

    // 6. Map to percentage (3.4V is 0%, 4.2V is 100%)
    int percentage = (bat_voltage_mv - 3400) / 8;
    if (percentage > 100) percentage = 100;
    if (percentage < 0) percentage = 0;

    return percentage;
}

int get_battery_percentage(void) {
    time_t now = time(NULL);
    if (s_cached_battery_percentage == -1 || (now - s_last_battery_read_time) >= 60) {
        int read_val = read_battery_percentage();
        if (read_val >= 0) {
            s_cached_battery_percentage = read_val;
            s_last_battery_read_time = now;
        }
    }
    return s_cached_battery_percentage;
}

esp_err_t oled_display_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    gpio_reset_pin(OLED_POWER_GPIO);
    gpio_set_direction(OLED_POWER_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(OLED_POWER_GPIO, 1);
    s_powered = true;
    vTaskDelay(pdMS_TO_TICKS(100));

    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .i2c_port = OLED_I2C_BUS_PORT,
        .sda_io_num = OLED_I2C_SDA_GPIO,
        .scl_io_num = OLED_I2C_SCL_GPIO,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &s_i2c_bus), TAG, "I2C bus init failed");

    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = OLED_I2C_HW_ADDR,
        .scl_speed_hz = OLED_PIXEL_CLOCK_HZ,
        .control_phase_bytes = 1,
        .lcd_cmd_bits = OLED_CMD_BITS,
        .lcd_param_bits = OLED_PARAM_BITS,
        .dc_bit_offset = 6,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c_bus, &io_config, &s_io_handle),
                        TAG, "panel IO init failed");

    esp_lcd_panel_dev_config_t panel_config = {
        .bits_per_pixel = 1,
        .reset_gpio_num = OLED_RST_GPIO,
    };
    esp_lcd_panel_ssd1306_config_t ssd1306_config = {
        .height = OLED_V_RES,
    };
    panel_config.vendor_config = &ssd1306_config;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ssd1306(s_io_handle, &panel_config, &s_panel_handle),
                        TAG, "SSD1306 init failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel_handle), TAG, "panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel_handle), TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel_handle, true), TAG, "panel on failed");

    init_battery_adc();

    // Configure GPIO 3 for battery charging status detection (active low)
    gpio_reset_pin(CHARGE_STATUS_GPIO);
    gpio_set_direction(CHARGE_STATUS_GPIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(CHARGE_STATUS_GPIO, GPIO_PULLUP_ONLY);

    s_initialized = true;
    xTaskCreate(oled_time_task, "oled_time", 4096, NULL, 1, &s_time_task_handle);
    oled_display_show_time(time(NULL));
    return ESP_OK;
}

void oled_display_on(void)
{
    if (!s_initialized) {
        return;
    }

    gpio_set_level(OLED_POWER_GPIO, 1);
    s_powered = true;
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_lcd_panel_reset(s_panel_handle);
    esp_lcd_panel_init(s_panel_handle);
    esp_lcd_panel_disp_on_off(s_panel_handle, true);
    oled_display_show_time(time(NULL));
}

void oled_display_off(void)
{
    if (s_initialized && s_powered) {
        esp_lcd_panel_disp_on_off(s_panel_handle, false);
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    s_powered = false;
    gpio_set_level(OLED_POWER_GPIO, 0);
}

void oled_display_set_alarm_active(bool active)
{
    s_alarm_active = active;
    oled_display_show_time(time(NULL));
}

void oled_display_set_next_alarm(time_t alarm_time)
{
    s_next_alarm_time = alarm_time;
    oled_display_show_time(time(NULL));
}

void oled_display_show_time(time_t now)
{
    if (!s_initialized || !s_powered) {
        return;
    }

    struct tm timeinfo;
    localtime_r(&now, &timeinfo);

    char time_text[16];
    strftime(time_text, sizeof(time_text), "%H:%M:%S", &timeinfo);

    // 1. Get battery percentage and charging status (from GPIO 3, active low)
    int bat_pct = get_battery_percentage();
    bool is_charging = (gpio_get_level(CHARGE_STATUS_GPIO) == 0);

    static bool s_last_charging_state = false;
    if (is_charging != s_last_charging_state) {
        ESP_LOGI(TAG, "Charging state changed: %s (GPIO3 level: %d)", 
                 is_charging ? "CHARGING" : "NOT CHARGING", gpio_get_level(CHARGE_STATUS_GPIO));
        s_last_charging_state = is_charging;
    }

    char bat_text[16];
    if (bat_pct >= 0) {
        if (is_charging) {
            snprintf(bat_text, sizeof(bat_text), "%d%%~", bat_pct);
        } else {
            snprintf(bat_text, sizeof(bat_text), "%d%%", bat_pct);
        }
    } else {
        if (is_charging) {
            snprintf(bat_text, sizeof(bat_text), "---~");
        } else {
            snprintf(bat_text, sizeof(bat_text), "---");
        }
    }
    int bat_x = 127 - (strlen(bat_text) * 6) - 8;

    // 2. Format next alarm time (using 3-letter day symbol and HH:MM)
    char next_alarm_text[16] = "";
    if (s_next_alarm_time > 0) {
        struct tm alarm_timeinfo;
        localtime_r(&s_next_alarm_time, &alarm_timeinfo);
        strftime(next_alarm_text, sizeof(next_alarm_text), "%a %H:%M", &alarm_timeinfo);
        string_to_upper(next_alarm_text);
    }

    // 3. Format current date (e.g. "MON, 28 JAN 2008")
    char date_text[32];
    strftime(date_text, sizeof(date_text), "%a, %d %b %Y", &timeinfo);
    string_to_upper(date_text);
    int date_x = (128 - (strlen(date_text) * 6)) / 2;

    // 4. Render Layout
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
    
    // Top Left: Next alarm time
    if (next_alarm_text[0] != '\0') {
        oled_draw_text(8, 4, next_alarm_text, 1);
    }
    
    // Top Right: Charge status/battery
    oled_draw_text(bat_x, 4, bat_text, 1);
    
    // Middle: Current time
    oled_draw_text(16, 25, time_text, 2);
    
    // Bottom: Centered date
    oled_draw_text(date_x, 53, date_text, 1);
    
    oled_flush();
}
