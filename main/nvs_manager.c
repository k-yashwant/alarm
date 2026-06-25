#include "nvs_flash.h"
#include "nvs.h"
#include "nvs_manager.h"
#include <time.h>
#include "system_events.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"

static char *TAG = "nvs_manager";
static char *TAG_fetch_nearest_alarm = "nvs_manager_fetch_nearest_alarm" ;
static char *TAG_trigger_time_remaining = "nvs_manager_tr_time_remain" ;

void init_nvs(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // If NVS partition was truncated, erase and retry
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

void save_alarms_to_nvs(void) {
    nvs_handle_t my_handle;
    esp_err_t err;

    // Open
    err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Error opening NVS handle!\n");
        return;
    }

    // Write the Blob (Binary Large Object)
    // We store the ENTIRE struct in one go.
    size_t required_size = sizeof(alarm_storage_t);
    err = nvs_set_blob(my_handle, "alarm_config", &current_alarm_config, required_size);

    if (err == ESP_OK) {
        // Commit changes
        err = nvs_commit(my_handle);
        ESP_LOGI(TAG, "Alarms saved to NVS successfully!\n");
    } else {
        ESP_LOGI(TAG, "Failed to save to NVS!\n");
    }

    // Close
    nvs_close(my_handle);
}

void print_alarms(void) {
    nvs_handle_t my_handle;
    esp_err_t err;

    err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return;

    // Check size
    size_t required_size = sizeof(alarm_storage_t);
    
    // Read the Blob
    err = nvs_get_blob(my_handle, "alarm_config", &current_alarm_config, &required_size);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Loaded %d alarms from NVS", current_alarm_config.count);
        for(int i=0; i<current_alarm_config.count; i++) {
             printf(" - %02d:%02d (Days: %d)\n", 
                    current_alarm_config.alarms[i].hour, 
                    current_alarm_config.alarms[i].minute, 
                    current_alarm_config.alarms[i].days);
        }
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "No saved alarms found (First boot?).");
        current_alarm_config.count = 0;
    }
    
    nvs_close(my_handle);
}

uint8_t get_next_day_mask(uint8_t current_mask) {
    uint8_t next = current_mask << 1;
    if (next > 64) {
        next = 1; // Wrap back to Monday
    }
    return next;
}

alarm_epoch_t trigger_time_remaining(alarm_entry_t *candidate, current_time_t *current_time){
    alarm_epoch_t alarm_epoch;
    alarm_epoch.time_remaining = UINT32_MAX;
    if (candidate -> days == 0) return alarm_epoch;

    uint16_t alarm_mins = candidate->hour * 60 + candidate->minute;
    ESP_LOGD(TAG_trigger_time_remaining, "%d", alarm_mins);
    uint8_t check_day_mask = current_time->day_bitmask;
    uint32_t diff_time = UINT32_MAX;
    int day_offset=0;

    for (day_offset=0; day_offset<=7; day_offset++){     
        ESP_LOGD(TAG_trigger_time_remaining, "%d and %d", candidate->days, check_day_mask);
        if (candidate->days & check_day_mask){
            if (day_offset == 0){ // alarm scheduled for current day
                if (alarm_mins > current_time->minutes_since_day){
                    diff_time = alarm_mins - current_time->minutes_since_day;
                    ESP_LOGD(TAG_trigger_time_remaining, "Zero day offset");
                    break;
                }
            }else{
                uint32_t mins_left_today = MINUTES_IN_A_DAY - current_time->minutes_since_day;
                uint32_t full_days_mins = (day_offset - 1) * MINUTES_IN_A_DAY;
                
                diff_time = mins_left_today + full_days_mins + alarm_mins;
                ESP_LOGD(TAG_trigger_time_remaining, "%d day offset", day_offset);
                break;
            }
        }
        check_day_mask = get_next_day_mask(check_day_mask);
    } 
    ESP_LOGD(TAG_trigger_time_remaining, "Time remaining %lu, day offset %d", diff_time, day_offset);
    alarm_epoch.time_remaining = diff_time;
    alarm_epoch.day_offset = day_offset;

    return alarm_epoch;
}

bool fetch_nearest_alarm_timestamp(time_t *nearest_alarm_timestamp){ // get the time, and date on which next alarm rings
    
    xEventGroupWaitBits(s_wifi_event_group,
    WIFI_CONNECTED_BIT | TIME_SYNCED_BIT,
    pdFALSE,
    pdTRUE,
    pdMS_TO_TICKS(10000));

    nvs_handle_t my_handle;
    esp_err_t err;
    size_t required_size = sizeof(alarm_storage_t);    
    err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err != ESP_OK || required_size != sizeof(alarm_storage_t)){ 
        ESP_LOGE(TAG_fetch_nearest_alarm, "NVS file read error");
        return false;
    }
    
    err = nvs_get_blob(my_handle, "alarm_config", &current_alarm_config, &required_size);   // get current alarm configuration stored in the device
    nvs_close(my_handle); // close nvs
    ESP_LOGI(TAG_fetch_nearest_alarm, "fetched %d alarms", current_alarm_config.count);
    time_t now_ts;
    time(&now_ts);
    struct tm alarm_tm = *localtime(&now_ts); 
    ESP_LOGI(TAG, "CURRENT_TIME: %d:%d:%d", alarm_tm.tm_hour, alarm_tm.tm_min, alarm_tm.tm_sec);

    current_time_t current_time; //to pass on to trigger_time_remaining funcition
    current_time.day_bitmask = 1 << (alarm_tm.tm_wday + 6) % 7;
    current_time.minutes_since_day = alarm_tm.tm_hour*60 + alarm_tm.tm_min;

    alarm_entry_t* nearest_alarm =  NULL; // nearest alarm; our target
    uint32_t min_diff_minutes = UINT32_MAX; // time difference between an alarm time and the current time (used in the iteration below)
    uint8_t day_offset = UINT8_MAX;
    alarm_epoch_t alarm_epoch;

    for (int i=0; i< current_alarm_config.count; i++){
        alarm_entry_t *candidate = (alarm_entry_t*) &current_alarm_config.alarms[i];
        alarm_epoch = trigger_time_remaining(candidate, &current_time);
        uint32_t diff_time = alarm_epoch.time_remaining;
        ESP_LOGD(TAG_fetch_nearest_alarm, "%lu time remaining", (unsigned long)diff_time);
        if (diff_time == UINT32_MAX) continue;

        if (diff_time < min_diff_minutes){
            min_diff_minutes = diff_time;
            nearest_alarm = candidate;
            day_offset = alarm_epoch.day_offset;
        }
    }
    ESP_LOGI(TAG_fetch_nearest_alarm, "Nearest alarm in %lu minutes (%lu seconds)", (unsigned long)min_diff_minutes, (unsigned long)(min_diff_minutes * 60));
    if (min_diff_minutes < UINT32_MAX){
        // *next_nearest_alarm = *nearest_alarm;
       // make an alarm timestamp of next nearest alarm//
       alarm_tm.tm_hour = nearest_alarm->hour;
       alarm_tm.tm_min = nearest_alarm->minute;
       alarm_tm.tm_sec = 0;
       alarm_tm.tm_mday += day_offset;

       *nearest_alarm_timestamp = mktime(&alarm_tm);
    }else{
        ESP_LOGE(TAG_fetch_nearest_alarm, "No alarm defined");
        return false;
    }
    ESP_LOGI(TAG_fetch_nearest_alarm, "NEAREST ALARM ON %02d:%02d, %d days from today", alarm_tm.tm_hour, alarm_tm.tm_min, day_offset);
    return true;
}
