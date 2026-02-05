#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include <string.h>
#include "alarm_trigger_event.h"
#include "esp_log.h"  

#define ALARM_PIN           33



#define BEEP_DURATION_1     400  // ms ON
#define BEEP_INTERVAL_1     700  // ms OFF

#define BEEP_DURATION_2     300
#define BEEP_INTERVAL_2     200


#define SNOOZE_TIME         MINUTES_TO_MS(10)

#define STOP_MESSAGE        "I am awake!"

#define BUZZER_ON           0
#define BUZZER_OFF          1

#define SNOOZE_MINUTES      0.1

static const char *TAG = "ALARM_TRIGGER_FUNCTION";

TaskHandle_t xBuzzerTaskHandle = NULL;
volatile bool keep_running = true;

// Handle to control the buzzer task

// ------------------------------------------------------------------------
// 1. INTERRUPT HANDLER (ISR)
// ------------------------------------------------------------------------

static TimerHandle_t snooze_timer;

void snooze_timer_cb(TimerHandle_t xTimer)
{
    ESP_LOGI(TAG, "Snooze complete");
    // wake alarm again
}

void start_snooze()
{
    snooze_timer = xTimerCreate(
        "snooze",
        pdMS_TO_TICKS(SNOOZE_MINUTES * 60 * 1000),
        pdFALSE,
        NULL,
        snooze_timer_cb
    );

    xTimerStart(snooze_timer, 0);
}

void IRAM_ATTR gpio_isr_handler(void* arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    if (is_alarm_active) {
        // --- MODE 1: SNOOZE ALARM ---
        if(xBuzzerTaskHandle != NULL) {
            vTaskNotifyGiveFromISR(xBuzzerTaskHandle, &xHigherPriorityTaskWoken);
        }
    } else {
        // --- MODE 2: NORMAL FUNCTION ---
        // Notify the main loop (or a dedicated handler task) to run the function.
        // We do not run complex logic inside the ISR directly.
        if (xMainTaskHandle != NULL) {
            vTaskNotifyGiveFromISR(xMainTaskHandle, &xHigherPriorityTaskWoken);
        }
    }

    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

// Initialise Push Button Function
void Button_Init(){
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_POSEDGE; // Trigger on rising edge (Press)
    io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_down_en = 1; // Enable internal pull-up
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);
    
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PUSH_PIN, gpio_isr_handler, NULL);
}


// ------------------------------------------------------------------------
// 2. THE BUZZER FUNCTION (TASK)
// ------------------------------------------------------------------------
void buzzer_pattern_task(void* arg){

    // Configure the Buzzer Pin
    gpio_reset_pin(ALARM_PIN);
    gpio_set_direction(ALARM_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(ALARM_PIN, BUZZER_OFF); 

    //Configure Push Button

    Button_Init();

    ESP_LOGI(TAG, "Alarm Started!. Press Button to Snooze or enter the code in serial");

    while (keep_running) {

        gpio_set_level(ALARM_PIN, BUZZER_ON);

        // // Wait Duration OR Button Press
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BEEP_DURATION_1)) > 0) {
            break;
        }
        
        gpio_set_level(ALARM_PIN, BUZZER_OFF);
        
        // Wait for BEEP_INTERVAL. Check for button press again.
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BEEP_INTERVAL_1)) > 0) {
            break;
        }
    }

        gpio_set_level(ALARM_PIN, BUZZER_OFF);
        xTaskNotifyStateClear(NULL);
    
    gpio_reset_pin(ALARM_PIN);
    gpio_set_direction(ALARM_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(ALARM_PIN, BUZZER_OFF);
    ESP_LOGI(TAG, "You have chosen to snooze for sometime");
    for(int i = 0; i < (SNOOZE_MINUTES * 60); i++) {
        if(!keep_running) break; 
        vTaskDelay(pdMS_TO_TICKS(1000)); // Wait 1 second
    }

    while(keep_running) {
        // --- STATE: ON ---
        gpio_set_level(ALARM_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(BEEP_DURATION_2));

        // --- STATE: OFF ---
        gpio_set_level(ALARM_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(BEEP_INTERVAL_2));
    }


    // --- CLEANUP ---
    // Ensure buzzer is definitively OFF before quitting
    gpio_set_level(ALARM_PIN, 1);
    ESP_LOGI(TAG, "Buzzer stopped by input.\n");
    if (xBuzzerTaskHandle != NULL){
        vTaskDelete(xBuzzerTaskHandle); 
        xBuzzerTaskHandle = NULL; // Prevent ISR from notifying a dead task
    }
    is_alarm_active = false;
}

void TriggerAlarm(){
    is_alarm_active = true;
    xTaskCreate(buzzer_pattern_task, "buzzer_task", 4096, NULL, 5, &xBuzzerTaskHandle);
    
    char rx_buffer[128];
    ESP_LOGI(TAG, "Enter you message here:");

    while (true){
        char *line = fgets(rx_buffer, sizeof(rx_buffer), stdin);

        if (line != NULL){
            line[strcspn(line, "\r\n")] = 0;
            
            if (strcmp(line, STOP_MESSAGE) == 0){
                ESP_LOGI(TAG, "Stopping the Alarm! Good Morning.\n Hope you have a great day!!!");
                keep_running = false;
                if(xBuzzerTaskHandle != NULL) xTaskNotifyGive(xBuzzerTaskHandle);
                break;
            }
            else{
            ESP_LOGI(TAG, "Unknown command!");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
    gpio_reset_pin(ALARM_PIN);
    ESP_LOGI(TAG, "Finished Alarm Trigger.");
}
