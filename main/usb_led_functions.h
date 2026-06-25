#ifndef USB_LED_FUNCTIONS_H
#define USB_LED_FUNCTIONS_H

#include "esp_log.h"


#include "led_strip.h" // Assuming this is needed for your LED

// Placeholder for your LED functions (Add your implementation back)
extern uint8_t s_led_state;

extern led_strip_handle_t led_strip;

#define BLINK_PERIOD_MS     1000
#define BLINK_GPIO 18

// --- TINYUSB INCLUDES (FROM EXAMPLE) ---
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "sdkconfig.h"

// --- LOGGING CONFIGURATION ---
#define LOG_QUEUE_SIZE  50
#define LOG_MSG_MAX_LEN 256 // Increased to handle hex dumps if needed
#define INPUT_QUEUE_SIZE 128 //

extern uint8_t rx_buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE + 1];
extern QueueHandle_t log_queue;
extern QueueHandle_t input_queue;

typedef struct {
    char buffer[LOG_MSG_MAX_LEN];
    size_t len;
} log_msg_t;


void init_serial_tinyusb();

void tinyusb_cdc_rx_callback(int itf, cdcacm_event_t *event);

int app_log_vprintf(const char *fmt, va_list args);

void usb_log_task(void *arg);

void setup_usb_from_example();
void uninstall_usb();

#endif 