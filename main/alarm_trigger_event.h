#ifndef ALARM_TRIGGER_EVENT_H
#define ALARM_TRIGGER_EVENT_H
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"


#define MINUTES_TO_MS(min)  ((min)*60000)
#define PUSH_PIN            2 
#define GPIO_INPUT_PIN_SEL  (1ULL<<PUSH_PIN)

extern TaskHandle_t xMainTaskHandle;
extern TaskHandle_t xBuzzerTaskHandle;
void gpio_isr_handler(void* arg);
void Button_Init();

void TriggerAlarm();
extern volatile bool is_alarm_active;
extern QueueHandle_t input_queue; 



#endif