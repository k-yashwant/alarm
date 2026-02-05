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

// --- TINYUSB INCLUDES (FROM EXAMPLE) ---
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "sdkconfig.h"

// --- YOUR APP INCLUDES ---
#include "alarm_trigger_event.h"
#include "wifi_functions.h"
#include "nvs_manager.h"
#include "led_strip.h" // Assuming this is needed for your LED

TaskHandle_t xMainTaskHandle = NULL;
RTC_DATA_ATTR uint8_t caliberate_status=0;
RTC_DATA_ATTR time_t nearest_alarm_timestamp;
RTC_DATA_ATTR uint8_t immediate_trigger = 0;
time_t now;
double trigger_time;
bool fetched_alarm;
static const char *TAG = "MAIN";
#define BLINK_PERIOD_MS     1000
#define BLINK_GPIO 18
RTC_DATA_ATTR volatile bool is_alarm_active = false;

#define ALARM_TRIGGER_TIME 10000
#define ALARM_TIME 
#define AWAKE_TIME 3000 * 60 // 3 minutes


// --- LOGGING CONFIGURATION ---
#define LOG_QUEUE_SIZE  50
#define LOG_MSG_MAX_LEN 256 // Increased to handle hex dumps if needed

static QueueHandle_t log_queue = NULL;

typedef struct {
    char buffer[LOG_MSG_MAX_LEN];
    size_t len;
} log_msg_t;

// 1. THE INTERCEPTOR (Sends ESP_LOG data to Queue)
int app_log_vprintf(const char *fmt, va_list args) {
    if (!log_queue) return 0;
    
    log_msg_t msg;
    // Format the string
    msg.len = vsnprintf(msg.buffer, LOG_MSG_MAX_LEN, fmt, args);
    
    // Send to Queue (Wait Time = 0). Non-blocking.
    xQueueSend(log_queue, &msg, 0);
    return msg.len;
}

// 2. THE USB TASK (Adapted from the Example)
// Instead of echoing RX data, this consumes TX data from your app
void usb_log_task(void *arg) {
    log_msg_t msg;
    
    while (1) {
        // Wait for logs from your app
        if (xQueueReceive(log_queue, &msg, portMAX_DELAY) == pdTRUE) {
            
            // --- LOGIC FROM EXAMPLE ---
            // We write to the queue. If USB is disconnected, this just fills 
            // the internal ringbuffer and moves on. It won't crash.
            tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (uint8_t*)msg.buffer, msg.len);
            
            // The Flush command pushes data to hardware IF connected
            esp_err_t err = tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
            
            // We ignore errors here. If it fails, it means USB is unplugged.
            // We just continue loop.
            (void)err; 
        }
    }
}

// 3. USB INITIALIZATION (EXACTLY AS PER EXAMPLE)
void setup_usb_from_example() {
    ESP_LOGI("MAIN", "USB initialization");
    
    // This macro was the missing key!
    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = NULL, // No Input needed for logs
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL
    };

    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&acm_cfg));
    ESP_LOGI("MAIN", "USB initialization DONE");
}

// ---------------------------------------------------------
// YOUR APP MAIN
// ---------------------------------------------------------

// Variables for your logic

// Placeholder for your LED functions (Add your implementation back)
static uint8_t s_led_state = 0;

static led_strip_handle_t led_strip;

static void blink_led(void)
{
        led_strip_set_pixel(led_strip, 0, 40, 16, 16);
        led_strip_refresh(led_strip);

        vTaskDelay(pdMS_TO_TICKS(500));
        led_strip_clear(led_strip);
}
static void configure_led(void)
{
    ESP_LOGI(TAG, "Example configured to blink addressable LED (RMT)!");
    
    /* LED strip initialization with the GPIO and pixels number */
    led_strip_config_t strip_config = {
        .strip_gpio_num = BLINK_GPIO,
        .max_leds = 1, // at least one LED on board
    };
    
    /* RMT Backend Configuration */
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
        .flags.with_dma = false,
    };
    
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    
    /* Set all LED off to clear all pixels */
    led_strip_clear(led_strip);
}

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
{
    // 1. Setup Logging Queue
    log_queue = xQueueCreate(LOG_QUEUE_SIZE, sizeof(log_msg_t));
    
    // 2. Start the USB Handler Task
    xTaskCreate(usb_log_task, "usb_log", 4096, NULL, 1, NULL);

    // 3. Redirect ESP_LOG system to our Queue
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_set_vprintf(app_log_vprintf);

    // 4. Initialize Hardware
    configure_led();
    blink_led();
    xMainTaskHandle = xTaskGetCurrentTaskHandle();
    Button_Init();

    // 5. Initialize USB (Using the robust Example logic)
    setup_usb_from_example();

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
        if (trigger_time < 300.0 || immediate_trigger){
            ESP_LOGI(TAG, "Less than 5 minutes for next alarm");
            if (trigger_time > 0.0){
                esp_sleep_enable_timer_wakeup((uint64_t) trigger_time * 1000000LL);
                esp_light_sleep_start();
                vTaskDelay(pdMS_TO_TICKS(50));
         
            }
            is_alarm_active = true;
            ESP_LOGI(TAG, "Triggering Alarm now");
            TriggerAlarm();

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