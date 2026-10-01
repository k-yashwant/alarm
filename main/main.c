#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "esp_sleep.h"



// --- YOUR APP INCLUDES ---
#include "usb_led_functions.h"
#include "alarm_trigger_event.h"
#include "wifi_functions.h"
#include "nvs_manager.h"
#include "oled_display.h"
#include "sd_card.h"


RTC_DATA_ATTR uint8_t caliberate_status=0;
RTC_DATA_ATTR time_t nearest_alarm_timestamp;
RTC_DATA_ATTR uint8_t immediate_trigger = 0;
time_t now;
double trigger_time;
bool fetched_alarm;
static const char *TAG = "MAIN";

RTC_DATA_ATTR volatile bool is_alarm_active = false;

#define WIFI_OPTION 0 //campus

// Threshold definitions
#define PRE_ALARM_WINDOW_SEC       300.0  // Wake-up buffer: n minutes (5 minutes) before alarm
#define MAX_DEEP_SLEEP_SEC         7200LL // 2 hours max deep sleep duration
#define DEEP_SLEEP_BUFFER_SEC      PRE_ALARM_WINDOW_SEC  // Wake up n minutes before alarm
#define BUTTON_POLL_TIMEOUT_MS     (60 * 1000) // 1 minute button polling timeout
#define MIN_DEEP_SLEEP_SEC         60.0   // Minimum sleep duration to enter deep sleep
#define DEEP_SLEEP_THRESHOLD_SEC  (MAX_DEEP_SLEEP_SEC + (BUTTON_POLL_TIMEOUT_MS / 1000) + DEEP_SLEEP_BUFFER_SEC) // 7560 seconds
#define BOOT_DEBUG_HOLD_MS         3000   // USB attach window on normal boot (skipped on alarm wake)



static bool is_alarm_urgent_boot(esp_reset_reason_t reason)
{
    if (reason != ESP_RST_DEEPSLEEP) {
        return false;
    }
    if (esp_sleep_get_wakeup_causes() != ESP_SLEEP_WAKEUP_TIMER) {
        return false;
    }
    time_t now_ts = time(NULL);
    if (now_ts < 946684800) {
        return false; // Time invalid (< year 2000), require full NTP sync
    }
    if (immediate_trigger) {
        return true;
    }
    if (nearest_alarm_timestamp > 0) {
        double secs = difftime(nearest_alarm_timestamp, now_ts);
        return secs >= -60.0 && secs <= (PRE_ALARM_WINDOW_SEC + MIN_DEEP_SLEEP_SEC);
    }
    return false;
}

static void wait_until_alarm(void)
{
    time(&now);
    double remaining = difftime(nearest_alarm_timestamp, now);
    ESP_LOGI(TAG, "Waiting %.1f seconds until alarm...", remaining);
    while (remaining > 0.0) {
        uint32_t wait_ms = (remaining > 1.0) ? 1000 : (uint32_t)(remaining * 1000);
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
        time(&now);
        remaining = difftime(nearest_alarm_timestamp, now);
    }
}

static const char *reset_reason_to_str(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_POWERON: return "POWERON";
        case ESP_RST_EXT: return "EXTERNAL";
        case ESP_RST_SW: return "SOFTWARE";
        case ESP_RST_PANIC: return "PANIC";
        case ESP_RST_INT_WDT: return "INT_WDT";
        case ESP_RST_TASK_WDT: return "TASK_WDT";
        case ESP_RST_WDT: return "OTHER_WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_SDIO: return "SDIO";
        default: return "UNKNOWN";
    }
}

bool refresh_schedule() {
    ESP_LOGI(TAG, "Performing Full Schedule Refresh...");
    vTaskDelay(pdMS_TO_TICKS(100));
    connect_wifi(WIFI_OPTION);
    sync_time();
    oled_display_set_wifi_connected(true);   // WiFi + NTP sync succeeded
    caliberate_status = 1;
    if (parse_alarm_json()) {
        save_alarms_to_nvs();
    } else {
        ESP_LOGW(TAG, "Remote schedule refresh failed; trying saved alarms from NVS");
    }
    bool fetched = fetch_nearest_alarm_timestamp(&nearest_alarm_timestamp);
    if (fetched) {
        oled_display_set_next_alarm(nearest_alarm_timestamp);
    }
    return fetched;
}

void app_main(void)
{
    // Keep the SD module powered during normal operation.  It is switched
    // off only just before the device enters deep sleep.
    sd_card_power_on();

    // Set timezone to IST (UTC+5:30) so localtime_r() converts UTC→local correctly.
    // POSIX TZ format: name + negative-offset (IST is UTC+5:30, so offset is -5:30).
    setenv("TZ", "IST-5:30", 1);
    tzset();

    esp_reset_reason_t reset_reason = esp_reset_reason();
    bool urgent_boot = is_alarm_urgent_boot(reset_reason);
    esp_rom_printf("BOOT reset reason: %d\n", (int)reset_reason);

    // In ESP-IDF with CONFIG_ESP_TIME_FUNCS_USE_RTC_TIMER enabled, wall-clock
    // time automatically advances across deep sleep via the RTC timer hardware.
    time_t boot_time = time(NULL);
    ESP_LOGI(TAG, "System time on boot: %lld", (long long)boot_time);

    init_serial_tinyusb(urgent_boot);
    esp_err_t sd_test_err = sd_card_append_test();
    if (sd_test_err != ESP_OK) {
        ESP_LOGW(TAG, "SD-card append test skipped/failed: %s", esp_err_to_name(sd_test_err));
    }
    ESP_LOGW(TAG, "BOOT reset reason: %s (%d)", reset_reason_to_str(reset_reason), (int)reset_reason);
    if (urgent_boot) {
        ESP_LOGW(TAG, "Alarm-imminent boot: skipping debug delays");
    } else {
        ESP_LOGW(TAG, "BOOT debug hold: waiting %d ms before app startup", BOOT_DEBUG_HOLD_MS);
        vTaskDelay(pdMS_TO_TICKS(BOOT_DEBUG_HOLD_MS));
    }

    Button_Init();
    if (oled_display_init() != ESP_OK) {
        ESP_LOGE(TAG, "OLED initialization failed");
    }
    if (!urgent_boot) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "System Started - USB is robust");
    }

    // ------------------------------------------------
    // REST OF YOUR LOGIC
    // ------------------------------------------------

   bool schedule_ready = false;

   if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
        esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_causes();
        if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT1 || wakeup_reason == ESP_SLEEP_WAKEUP_UART) {
            ESP_LOGW(TAG, "Woke up from Deep Sleep via Button!");
            firebase_send_log("WAKEUP_BUTTON", "Woke from deep sleep via button press");
            immediate_trigger = 0;
            schedule_ready = refresh_schedule();
            if (!schedule_ready){
                ESP_LOGE(TAG, "Couldn't fetch nearest alarm");
                firebase_send_log("ERROR", "refresh_schedule failed after button wakeup");
                // notify the user (later version)
            }
        }else if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER){
            ESP_LOGI(TAG, "Successfully woken up from deep sleep via timer");
            if (!urgent_boot) {
                firebase_send_log("WAKEUP_TIMER", "Woke from deep sleep via timer");
                schedule_ready = refresh_schedule();
            } else {
                schedule_ready = nearest_alarm_timestamp > 0;
                if (schedule_ready) {
                    oled_display_set_next_alarm(nearest_alarm_timestamp);
                }
            }
        }else{
            ESP_LOGI(TAG, "WAKEUP: %d", (int) wakeup_reason);
            char wakeup_msg[32];
            snprintf(wakeup_msg, sizeof(wakeup_msg), "Unknown wakeup cause: %d", (int)wakeup_reason);
            firebase_send_log("WAKEUP_UNKNOWN", wakeup_msg);
            schedule_ready = refresh_schedule();
        }
    }else{
        ESP_LOGI(TAG, "Wakeup from unusual source");
        schedule_ready = refresh_schedule();
        firebase_send_log("BOOT", "Fresh boot or non-deep-sleep reset");
    }

    while (!schedule_ready) {
        ESP_LOGE(TAG, "No valid alarm schedule available; retrying in 60 seconds");
        firebase_send_log("ERROR", "No valid alarm schedule available; retrying");
        vTaskDelay(pdMS_TO_TICKS(60000));
        schedule_ready = refresh_schedule();
    }
    
    time(&now);
    oled_display_show_time(now);
    trigger_time = difftime(nearest_alarm_timestamp, now);

    while(1){
        time(&now);
        trigger_time = difftime(nearest_alarm_timestamp, now);
        ESP_LOGI(TAG, "Remaining time until alarm: %.1f sec", trigger_time);

        bool in_pre_alarm_window = (nearest_alarm_timestamp > 0) &&
                                   (trigger_time <= (PRE_ALARM_WINDOW_SEC + MIN_DEEP_SLEEP_SEC)) &&
                                   (trigger_time >= -60.0);

        if (in_pre_alarm_window || immediate_trigger){
            ESP_LOGI(TAG, "Alarm due in %.1f seconds (within pre-alarm window) - waiting to trigger without sleeping", trigger_time);
            immediate_trigger = 0;
            wait_until_alarm();

            is_alarm_active = true;
            oled_display_set_alarm_active(true);
            ESP_LOGI(TAG, "Triggering Alarm now");
            
            TriggerAlarm();

            ESP_LOGI(TAG, "Successfully executed last alarm");
            firebase_send_log("ALARM_CYCLE_DONE", "Alarm finished, fetching next alarm");
            is_alarm_active = false;
            oled_display_set_alarm_active(false);
            caliberate_status = 0;
            immediate_trigger = 0;
            fetched_alarm = refresh_schedule();
            if (!fetched_alarm){
                ESP_LOGE(TAG, "Couldn't fetch nearest alarm after refresh");
                firebase_send_log("ERROR", "refresh_schedule failed after alarm");
                while (!refresh_schedule()) {
                    ESP_LOGE(TAG, "No valid alarm schedule after alarm; retrying in 60 seconds");
                    vTaskDelay(pdMS_TO_TICKS(60000));
                }
                fetched_alarm = true;
            }
            time(&now);
            trigger_time = difftime(nearest_alarm_timestamp, now);
            continue;
        }else if (caliberate_status == 0){
            ESP_LOGI(TAG, "Calibrating time");
            connect_wifi(WIFI_OPTION);
            sync_time();
            caliberate_status = 1;
            time(&now);
            trigger_time = difftime(nearest_alarm_timestamp, now);
            if ((nearest_alarm_timestamp > 0) &&
                (trigger_time <= (PRE_ALARM_WINDOW_SEC + MIN_DEEP_SLEEP_SEC))) {
                continue;
            }
        }

        // Never wait idle or sleep if alarm is within the pre-alarm window
        if (nearest_alarm_timestamp > 0 && trigger_time <= (PRE_ALARM_WINDOW_SEC + MIN_DEEP_SLEEP_SEC)) {
            ESP_LOGI(TAG, "Alarm imminent (%.1f sec), staying awake and skipping idle sleep", trigger_time);
            continue;
        }

        ESP_LOGI(TAG, "Waiting for push button input for 1 minute before sleeping");
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BUTTON_POLL_TIMEOUT_MS))) {
            ESP_LOGI(TAG, "Button pressed during idle wait! Refreshing schedule...");
            refresh_schedule();
            time(&now);
            trigger_time = difftime(nearest_alarm_timestamp, now);
            continue;
        }

        // Recalculate remaining time after 1-minute button wait
        time(&now);
        trigger_time = difftime(nearest_alarm_timestamp, now);

        if (nearest_alarm_timestamp > 0 && trigger_time <= (PRE_ALARM_WINDOW_SEC + MIN_DEEP_SLEEP_SEC)) {
            ESP_LOGI(TAG, "Remaining time (%.1f sec) now within pre-alarm window, skipping sleep", trigger_time);
            continue;
        }

        if (trigger_time > DEEP_SLEEP_THRESHOLD_SEC){
            ESP_LOGI(TAG, "Alarm far in future (%.1f sec). Deep sleeping for 2 hrs", trigger_time);
            esp_sleep_enable_timer_wakeup((uint64_t) MAX_DEEP_SLEEP_SEC * 1000000ULL);
            caliberate_status = 0;
            immediate_trigger = 0;
        }else{
            double sleep_sec = trigger_time - DEEP_SLEEP_BUFFER_SEC;
            if (sleep_sec < MIN_DEEP_SLEEP_SEC) {
                ESP_LOGI(TAG, "Remaining sleep duration (%.1f sec) too short (< %.0f sec), staying awake for alarm",
                         sleep_sec, MIN_DEEP_SLEEP_SEC);
                continue;
            }
            ESP_LOGI(TAG, "Deep sleeping for %.1f sec (waking %.0f sec before alarm)",
                     sleep_sec, DEEP_SLEEP_BUFFER_SEC);
            esp_sleep_enable_timer_wakeup((uint64_t)sleep_sec * 1000000ULL);
            immediate_trigger = 1;
        }

        oled_display_set_wifi_connected(false);  // WiFi will be gone during sleep
        oled_display_off();
        flush_usb_logs();
        gpio_pullup_dis(PUSH_PIN);
        gpio_pulldown_en(PUSH_PIN);
        esp_sleep_enable_ext1_wakeup(GPIO_INPUT_PIN_SEL, ESP_EXT1_WAKEUP_ANY_HIGH);
        sd_card_power_off();
        esp_deep_sleep_start();
    }
}
