#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
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


RTC_DATA_ATTR uint8_t caliberate_status=0;
RTC_DATA_ATTR time_t nearest_alarm_timestamp;
RTC_DATA_ATTR uint8_t immediate_trigger = 0;
time_t now;
double trigger_time;
bool fetched_alarm;
static const char *TAG = "MAIN";

RTC_DATA_ATTR volatile bool is_alarm_active = false;

#define WIFI_OPTION 1

// Threshold definitions (Magic numbers refactored)
#define PRE_ALARM_SLEEP_WINDOW_SEC 300.0  // 5 minutes light sleep window before alarm
#define MAX_DEEP_SLEEP_SEC         7200LL // 2 hours max deep sleep duration
#define DEEP_SLEEP_BUFFER_SEC      60.0   // 1 minute buffer to wake up early before alarm
#define BUTTON_POLL_TIMEOUT_MS     (60 * 1000) // 1 minute button polling timeout
#define DEEP_SLEEP_THRESHOLD_SEC  (MAX_DEEP_SLEEP_SEC + (BUTTON_POLL_TIMEOUT_MS / 1000) + DEEP_SLEEP_BUFFER_SEC) // 7320 seconds
#define USB_REINIT_MARGIN_SEC      5.0    // Wake this early before alarm to re-init USB after light sleep
#define BOOT_DEBUG_HOLD_MS         3000   // USB attach window on normal boot (skipped on alarm wake)



static bool is_alarm_urgent_boot(esp_reset_reason_t reason)
{
    if (reason != ESP_RST_DEEPSLEEP) {
        return false;
    }
    if (esp_sleep_get_wakeup_causes() != ESP_SLEEP_WAKEUP_TIMER) {
        return false;
    }
    if (immediate_trigger) {
        return true;
    }
    if (nearest_alarm_timestamp > 0) {
        time_t now_ts;
        time(&now_ts);
        double secs = difftime(nearest_alarm_timestamp, now_ts);
        return secs >= 0.0 && secs <= PRE_ALARM_SLEEP_WINDOW_SEC;
    }
    return false;
}

static void wait_until_alarm(void)
{
    time(&now);
    double remaining = difftime(nearest_alarm_timestamp, now);
    if (remaining > 0.0) {
        ESP_LOGI(TAG, "Waiting %.1f seconds until alarm...", remaining);
        vTaskDelay(pdMS_TO_TICKS((uint32_t)(remaining * 1000)));
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
    esp_reset_reason_t reset_reason = esp_reset_reason();
    bool urgent_boot = is_alarm_urgent_boot(reset_reason);
    esp_rom_printf("BOOT reset reason: %d\n", (int)reset_reason);

    init_serial_tinyusb(urgent_boot);
    ESP_LOGW(TAG, "BOOT reset reason: %s (%d)", reset_reason_to_str(reset_reason), (int)reset_reason);
    if (urgent_boot) {
        ESP_LOGW(TAG, "Alarm-imminent boot: skipping debug delays");
    } else {
        ESP_LOGW(TAG, "BOOT debug hold: waiting %d ms before app startup", BOOT_DEBUG_HOLD_MS);
        vTaskDelay(pdMS_TO_TICKS(BOOT_DEBUG_HOLD_MS));
    }

    Button_Init();
    if (!urgent_boot) {
        if (oled_display_init() != ESP_OK) {
            ESP_LOGE(TAG, "OLED initialization failed");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP_LOGI(TAG, "System Started - USB is robust");
    }

    // ------------------------------------------------
    // REST OF YOUR LOGIC (Unchanged)
    // ------------------------------------------------

   bool schedule_ready = false;

   if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
        esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_causes();
        if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT1 || wakeup_reason == ESP_SLEEP_WAKEUP_UART) {
            ESP_LOGW(TAG, "Woke up from Deep Sleep via Button!");
            firebase_send_log("WAKEUP_BUTTON", "Woke from deep sleep via button press");
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
            }
            schedule_ready = nearest_alarm_timestamp > 0;
            if (schedule_ready) {
                oled_display_set_next_alarm(nearest_alarm_timestamp);
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
    if (!urgent_boot) {
        oled_display_show_time(now);
    }
    trigger_time = difftime(nearest_alarm_timestamp, now);

    while(1){
        ESP_LOGI(TAG,"Remaining time: %lf", trigger_time);
        if (trigger_time < PRE_ALARM_SLEEP_WINDOW_SEC || immediate_trigger){
            ESP_LOGI(TAG, "Less than 5 minutes for next alarm");
            if (trigger_time > USB_REINIT_MARGIN_SEC) {
                double sleep_time = trigger_time - USB_REINIT_MARGIN_SEC;
                ESP_LOGI(TAG, "Light sleeping for %.1f sec (%.1f sec margin for USB re-init)",
                         sleep_time, USB_REINIT_MARGIN_SEC);
                esp_sleep_enable_timer_wakeup((uint64_t)sleep_time * 1000000LL);
                oled_display_off();
                uninstall_usb();
                esp_light_sleep_start();
                vTaskDelay(pdMS_TO_TICKS(50));
                setup_usb_from_example();
                oled_display_on();
            }
            wait_until_alarm();

            if (oled_display_init() != ESP_OK) {
                ESP_LOGE(TAG, "OLED initialization failed");
            }
            is_alarm_active = true;
            oled_display_set_alarm_active(true);
            setup_usb_from_example();
            ESP_LOGI(TAG, "Triggering Alarm now");
            
            TriggerAlarm();

            ESP_LOGI(TAG, "Successfully executed last alarm");
            firebase_send_log("ALARM_CYCLE_DONE", "Alarm finished, fetching next alarm");
            is_alarm_active=false;
            oled_display_set_alarm_active(false);
            caliberate_status=0;
            connect_wifi(WIFI_OPTION);
            sync_time();
            ESP_LOGI(TAG, "Updated system time");
            caliberate_status=1;
            fetched_alarm = fetch_nearest_alarm_timestamp(&nearest_alarm_timestamp);
            if (fetched_alarm) {
                oled_display_set_next_alarm(nearest_alarm_timestamp);
            }
            immediate_trigger = 0;
            if (!fetched_alarm){
                ESP_LOGE(TAG, "Couldn't fetch nearest alarm");
                firebase_send_log("ERROR", "fetch_nearest_alarm_timestamp failed after alarm");
                while (!refresh_schedule()) {
                    ESP_LOGE(TAG, "No valid alarm schedule after alarm; retrying in 60 seconds");
                    vTaskDelay(pdMS_TO_TICKS(60000));
                }
                fetched_alarm = true;
                // notify the user (later version)
            }
            time(&now);
            trigger_time = difftime(nearest_alarm_timestamp, now);
            continue;
        }else if (caliberate_status == 0){
            ESP_LOGI(TAG, "Caliberating time");
            connect_wifi(WIFI_OPTION);
            sync_time();
            caliberate_status = 1;
            time(&now);
            trigger_time = difftime(nearest_alarm_timestamp, now);
        }
        if (trigger_time > DEEP_SLEEP_THRESHOLD_SEC){
            ESP_LOGI(TAG, "Waiting for any push button input for 1 minute");
            if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BUTTON_POLL_TIMEOUT_MS))) {
                refresh_schedule();
                time(&now);
                trigger_time = difftime(nearest_alarm_timestamp, now);
                continue;
            }
            ESP_LOGI(TAG, "Deep sleeping for 2 hrs");
            esp_sleep_enable_timer_wakeup((uint64_t) MAX_DEEP_SLEEP_SEC * 1000000LL);
            caliberate_status = 0;
        }else{
            if (trigger_time < 0) continue;
            if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BUTTON_POLL_TIMEOUT_MS))) {
                ESP_LOGI(TAG, "");
                refresh_schedule();
                time(&now);
                trigger_time = difftime(nearest_alarm_timestamp, now);
                continue;
            }
            ESP_LOGI(TAG, "Deep sleeping for less than 2 hrs");
            esp_sleep_enable_timer_wakeup((uint64_t) (trigger_time - DEEP_SLEEP_BUFFER_SEC) * 1000000LL);
            immediate_trigger = 1;
        }
        oled_display_off();
        esp_sleep_enable_ext1_wakeup(GPIO_INPUT_PIN_SEL, ESP_EXT1_WAKEUP_ANY_HIGH);
        esp_deep_sleep_start();
    }
}
