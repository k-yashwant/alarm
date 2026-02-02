#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "alarm_trigger_event.h"
#include "wifi_functions.h"
#include "nvs_manager.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "driver/gpio.h"


#include <time.h>

// RTC_DATA_ATTR uint8_t alarm_trigger_status=0;
RTC_DATA_ATTR uint8_t caliberate_status=0;
RTC_DATA_ATTR time_t nearest_alarm_timestamp;
RTC_DATA_ATTR uint8_t immediate_trigger = 0;
RTC_DATA_ATTR int alarm_ringing_state = 0;

TaskHandle_t xMainTaskHandle = NULL;
time_t now;
double trigger_time;
RTC_DATA_ATTR volatile bool is_alarm_active = false;
bool fetched_alarm;

static const char *TAG = "MAIN";

// #define MINUTES_TO_MS(min) ((min)*60000)
#define ALARM_TRIGGER_TIME 10000
#define ALARM_TIME 
#define AWAKE_TIME 3000 * 60 // 3 minutes
#define LED_PIN 18


bool refresh_schedule() {
    ESP_LOGI(TAG, "Performing Full Schedule Refresh...");
    vTaskDelay(pdMS_TO_TICKS(100));
    connect_campus_wifi();
    sync_time();
    caliberate_status = 1;
    parse_alarm_json();
    save_alarms_to_nvs();
    return fetch_nearest_alarm_timestamp(&nearest_alarm_timestamp);
}


void app_main(void)
{   vTaskDelay(pdMS_TO_TICKS(50));
    if (alarm_ringing_state == 1){
        ESP_LOGI(TAG, "Serial Interrupt During Alarm Trigger Event");
        TriggerAlarm();
        alarm_ringing_state = 0;
        is_alarm_active = false;
    }
    vTaskDelay(pdMS_TO_TICKS(4000));


    xMainTaskHandle = xTaskGetCurrentTaskHandle();
    Button_Init();
    ESP_LOGI(TAG, "Initialised Push Button");

    if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
        esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_causes();
        if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT1) {
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
        if (trigger_time < 300.0 || immediate_trigger){
            ESP_LOGI(TAG, "Less than 5 minutes for next alarm");
            if (trigger_time > 0.0){
                esp_sleep_enable_timer_wakeup((uint64_t) trigger_time * 1000000LL);
                esp_light_sleep_start();
                vTaskDelay(pdMS_TO_TICKS(50));
         
            }
            is_alarm_active = true;
            alarm_ringing_state = 1;
            ESP_LOGI(TAG, "Triggering Alarm now");
            TriggerAlarm();
            alarm_ringing_state=0;

            ESP_LOGI(TAG, "Successfully executed last alarm");
            is_alarm_active=false;
            caliberate_status=0;
            connect_campus_wifi();
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
            connect_campus_wifi();
            sync_time();
            caliberate_status = 1;
            time(&now);
            trigger_time = difftime(nearest_alarm_timestamp, now);
        }
        if (trigger_time > 7320.0){
            ESP_LOGI(TAG, "Waiting for any push button input for 1 minute");
            if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(60*1000))) {
                refresh_schedule();
                time(&now);
                trigger_time = difftime(nearest_alarm_timestamp, now);
                continue;
            }
            ESP_LOGI(TAG, "Deep sleeping for 2 hrs");
            esp_sleep_enable_timer_wakeup((uint64_t) 7200LL * 1000000LL);
            caliberate_status = 0;
        }else{
            if (trigger_time < 0) continue;
            if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(60*1000))) {
                ESP_LOGI(TAG, "");
                refresh_schedule();
                time(&now);
                trigger_time = difftime(nearest_alarm_timestamp, now);
                continue;
            }
            ESP_LOGI(TAG, "Deep sleeping for less than 2 hrs");
            esp_sleep_enable_timer_wakeup((uint64_t) (trigger_time-60) * 1000000LL);
            immediate_trigger = 1;
        }
        esp_sleep_enable_ext1_wakeup(GPIO_INPUT_PIN_SEL, ESP_EXT1_WAKEUP_ANY_HIGH);
        esp_deep_sleep_start();
    }

    
}