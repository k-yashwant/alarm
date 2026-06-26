#include <stdio.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include <string.h>
#include "alarm_trigger_event.h"
#include "esp_log.h"
#include "wifi_functions.h"  // for firebase_send_log

#define ALARM_PIN           33

#define BEEP_DURATION_1     400  // ms ON
#define BEEP_INTERVAL_1     700  // ms OFF

#define BEEP_DURATION_2     300
#define BEEP_INTERVAL_2     200


#define SNOOZE_TIME         MINUTES_TO_MS(10)

#define STOP_MESSAGE        "I am awake!"

#define BUZZER_ON           0
#define BUZZER_OFF          1

#define VIBRATION_PIN       38   // Change to your desired GPIO pin for the vibration motor
#define VIBRATION_ON        1    // Typically 1 (HIGH) turns on the transistor/switch
#define VIBRATION_OFF       0    // 0 (LOW) turns off the transistor/switch

#define SNOOZE_MINUTES      0.1

static const char *TAG = "ALARM_TRIGGER_FUNCTION";

TaskHandle_t xBuzzerTaskHandle = NULL;
volatile bool keep_running = true;

// Handle to control the buzzer task

// ------------------------------------------------------------------------
// 1. INTERRUPT HANDLER (ISR)
// ------------------------------------------------------------------------

static TimerHandle_t snooze_timer;

static bool is_stop_command(const char *line)
{
    char normalized[sizeof("i am awake!")];
    size_t out = 0;

    while (*line && isspace((unsigned char)*line)) {
        line++;
    }

    for (; *line != '\0' && out < sizeof(normalized) - 1; line++) {
        if (*line == '!') {
            continue;
        }
        normalized[out++] = (char)tolower((unsigned char)*line);
    }

    while (out > 0 && isspace((unsigned char)normalized[out - 1])) {
        out--;
    }
    normalized[out] = '\0';

    return strcmp(normalized, "i am awake") == 0 ||
           strcmp(normalized, "stop") == 0 ||
           strcmp(normalized, "awake") == 0;
}

static void stop_alarm_from_serial(void)
{
    ESP_LOGI("ALARM", "Stopping the Alarm! Good Morning.");
    keep_running = false;
    if (xBuzzerTaskHandle != NULL) {
        xTaskNotifyGive(xBuzzerTaskHandle);
    }
}

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
// 2. THE BUZZER & VIBRATION FUNCTION (TASK)
// ------------------------------------------------------------------------
void buzzer_pattern_task(void* arg){

    // Configure the Buzzer Pin
    gpio_reset_pin(ALARM_PIN);
    gpio_set_direction(ALARM_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(ALARM_PIN, BUZZER_OFF);

    // Configure the Vibration Motor Pin
    gpio_reset_pin(VIBRATION_PIN);
    gpio_set_direction(VIBRATION_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(VIBRATION_PIN, VIBRATION_OFF);
    // Note: Button ISR already registered at boot via Button_Init() in main.c

    // --- PHASE 1: Initial alarm — button snoozed, serial dismisses ---
    firebase_send_log("ALARM_TRIGGERED", "Alarm started buzzing");
    ESP_LOGI(TAG, "Alarm! Press button to snooze once, or type the stop message to dismiss.");
    while (keep_running) {
        gpio_set_level(ALARM_PIN, BUZZER_ON);
        gpio_set_level(VIBRATION_PIN, VIBRATION_ON);
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BEEP_DURATION_1)) > 0) {
            break;  // woken by button press OR serial input
        }
        gpio_set_level(ALARM_PIN, BUZZER_OFF);
        gpio_set_level(VIBRATION_PIN, VIBRATION_OFF);
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BEEP_INTERVAL_1)) > 0) {
            break;
        }
    }
    gpio_set_level(ALARM_PIN, BUZZER_OFF);
    gpio_set_level(VIBRATION_PIN, VIBRATION_OFF);

    // If serial dismissed during phase 1, skip snooze entirely
    if (!keep_running) goto cleanup;

    // --- SNOOZE PHASE ---
    xTaskNotifyStateClear(NULL);  // flush any extra notifications
    firebase_send_log("ALARM_SNOOZED", "Button pressed – alarm snoozed");
    ESP_LOGI(TAG, "Snoozed for %.1f minutes. No more snoozes after this.", SNOOZE_MINUTES);
    for (int i = 0; i < (int)(SNOOZE_MINUTES * 60); i++) {
        if (!keep_running) goto cleanup;  // serial dismissed during snooze
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // --- PHASE 2: Post-snooze — only serial can stop it, button is ignored ---
    xTaskNotifyStateClear(NULL);  // flush button presses that came in during snooze
    ESP_LOGI(TAG, "Snooze over! You must type the stop message to dismiss.");
    while (keep_running) {
        gpio_set_level(ALARM_PIN, BUZZER_ON);
        gpio_set_level(VIBRATION_PIN, VIBRATION_ON);
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BEEP_DURATION_2)) > 0) {
            if (!keep_running) break;   // serial set keep_running=false → stop
            // button press during phase 2 → ignore, keep beeping (no more snooze)
        }
        gpio_set_level(ALARM_PIN, BUZZER_OFF);
        gpio_set_level(VIBRATION_PIN, VIBRATION_OFF);
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BEEP_INTERVAL_2)) > 0) {
            if (!keep_running) break;   // serial dismissed → stop
        }
    }

cleanup:
    // --- CLEANUP ---
    gpio_set_level(ALARM_PIN, BUZZER_OFF);
    gpio_set_level(VIBRATION_PIN, VIBRATION_OFF);
    firebase_send_log("ALARM_DISMISSED", "Alarm dismissed successfully");
    ESP_LOGI(TAG, "Alarm dismissed.");
    is_alarm_active = false;
    xBuzzerTaskHandle = NULL;  // clear BEFORE deleting so ISR won't notify a dead task
    vTaskDelete(NULL);         // delete self (NULL = calling task)
}

void TriggerAlarm(){
    keep_running = true;   // Reset so buzzer task runs fresh on every alarm
    is_alarm_active = true;
    xTaskCreate(buzzer_pattern_task, "buzzer_task", 4096, NULL, 5, &xBuzzerTaskHandle);
    
    char line_buffer[64];
    int line_pos = 0;

    ESP_LOGI(TAG, "Enter you message here:");

    if (input_queue != NULL) {
        xQueueReset(input_queue);
    }

    while (true){
        char c;

        if (xQueueReceive(input_queue, &c, pdMS_TO_TICKS(50)) == pdTRUE) {

            // Handle Backspace (127 or 8)
            if ((c == 127 || c == 8) && line_pos > 0) {
                line_pos--;
                continue;
            }

            // Handle Newline (End of command)
            if (c == '\n' || c == '\r') {
                line_buffer[line_pos] = 0; // Null terminate
                
                // Only process if we have data
                if (line_pos > 0) {
                    ESP_LOGI("ALARM", "Received: %s", line_buffer);

                    if (is_stop_command(line_buffer)) {
                        stop_alarm_from_serial();
                        break;
                    } else {
                        ESP_LOGI("ALARM", "Incorrect command.");
                    }
                }
                
                line_pos = 0; // Reset buffer
            } 
            else if (line_pos < sizeof(line_buffer) - 1) {
                // Add char to buffer
                line_buffer[line_pos++] = c;
                line_buffer[line_pos] = 0;
                if (is_stop_command(line_buffer)) {
                    ESP_LOGI("ALARM", "Received: %s", line_buffer);
                    stop_alarm_from_serial();
                    break;
                }
            }
        }
        
    }
    gpio_reset_pin(ALARM_PIN);
    ESP_LOGI(TAG, "Finished Alarm Trigger.");
}
