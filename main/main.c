#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "esp_sleep.h"



// --- YOUR APP INCLUDES ---
#include "usb_led_functions.h"
#include "alarm_trigger_event.h"
#include "wifi_functions.h"
#include "nvs_manager.h"


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
#define MIN_LIGHT_SLEEP_TIME_SEC   30.0   // Do not light sleep if alarm is closer than 30 seconds



bool refresh_schedule() {
    ESP_LOGI(TAG, "Performing Full Schedule Refresh...");
    vTaskDelay(pdMS_TO_TICKS(100));
    connect_wifi(WIFI_OPTION);
    sync_time();
    caliberate_status = 1;
    parse_alarm_json();
    save_alarms_to_nvs();
    return fetch_nearest_alarm_timestamp(&nearest_alarm_timestamp);
}

void app_main(void)
{
    init_serial_tinyusb();
    Button_Init();
    
    


    // Delay to let host see the device (Optional)
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "System Started - USB is robust");

    // ------------------------------------------------
    // REST OF YOUR LOGIC (Unchanged)
    // ------------------------------------------------

   if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
        esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_causes();
        if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT1 || wakeup_reason == ESP_SLEEP_WAKEUP_UART) {
            ESP_LOGW(TAG, "Woke up from Deep Sleep via Button!");
            if (!refresh_schedule()){
                ESP_LOGE(TAG, "Couldn't fetch nearest alarm");
                // notify the user (later version)
            }
        }else if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER){
            ESP_LOGI(TAG, "Successfully woken up from deep sleep via timer");
        }else{
            ESP_LOGI(TAG, "WAKEUP: %d", (int) wakeup_reason);
        }
    }else{
        ESP_LOGI(TAG, "Wakeup from unusual source");
        refresh_schedule();
    }
    
    time(&now);
   trigger_time = difftime(nearest_alarm_timestamp, now);

    while(1){
        ESP_LOGI(TAG,"Remaining time: %lf", trigger_time);
        if (trigger_time < PRE_ALARM_SLEEP_WINDOW_SEC || immediate_trigger){
            ESP_LOGI(TAG, "Less than 5 minutes for next alarm");
            if (trigger_time > MIN_LIGHT_SLEEP_TIME_SEC){
                double sleep_time = trigger_time - MIN_LIGHT_SLEEP_TIME_SEC;
                ESP_LOGI(TAG, "Light sleeping for %lf seconds (waking up %lf seconds early)", sleep_time, MIN_LIGHT_SLEEP_TIME_SEC);
                esp_sleep_enable_timer_wakeup((uint64_t) sleep_time * 1000000LL);
                uninstall_usb();             
                esp_light_sleep_start();
                vTaskDelay(pdMS_TO_TICKS(50));
                setup_usb_from_example();
                ESP_LOGI(TAG, "Waiting %lf seconds with active USB before alarm...", MIN_LIGHT_SLEEP_TIME_SEC);
                vTaskDelay(pdMS_TO_TICKS(MIN_LIGHT_SLEEP_TIME_SEC * 1000));
            } else if (trigger_time > 0.0) {
                ESP_LOGI(TAG, "Alarm is very close (%lf sec). Waiting without sleeping...", trigger_time);
                vTaskDelay(pdMS_TO_TICKS(trigger_time * 1000));
            }
            is_alarm_active = true;
            setup_usb_from_example();
            vTaskDelay(pdMS_TO_TICKS(1000));
            ESP_LOGI(TAG, "Triggering Alarm now");
            
            TriggerAlarm();

            ESP_LOGI(TAG, "Successfully executed last alarm");
            is_alarm_active=false;
            caliberate_status=0;
            connect_wifi(WIFI_OPTION);
            sync_time();
            ESP_LOGI(TAG, "Updated system time");
            caliberate_status=1;
            fetched_alarm = fetch_nearest_alarm_timestamp(&nearest_alarm_timestamp);
            immediate_trigger = 0;
            if (!fetched_alarm){
                ESP_LOGE(TAG, "Couldn't fetch nearest alarm");
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
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
        esp_sleep_enable_ext1_wakeup(GPIO_INPUT_PIN_SEL, ESP_EXT1_WAKEUP_ANY_HIGH);
        esp_deep_sleep_start();
    }
}