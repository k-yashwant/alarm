#include "usb_led_functions.h"

static const char *TAG = "USB_LED_FUNCTIONS";

uint8_t rx_buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE + 1];

QueueHandle_t log_queue = NULL;
QueueHandle_t input_queue;

TaskHandle_t xMainTaskHandle = NULL;


uint8_t s_led_state;
led_strip_handle_t led_strip;
static bool s_usb_installed = false;

static void blink_led(void);
static void configure_led(void);

void init_serial_tinyusb(bool fast_boot)
{
    log_queue = xQueueCreate(LOG_QUEUE_SIZE, sizeof(log_msg_t));
    input_queue = xQueueCreate(INPUT_QUEUE_SIZE, sizeof(char));

    xTaskCreate(usb_log_task, "usb_log", 4096, NULL, 1, NULL);

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_set_vprintf(app_log_vprintf);
    xMainTaskHandle = xTaskGetCurrentTaskHandle();

    setup_usb_from_example();

    configure_led();
    if (!fast_boot) {
        blink_led();
    }
}


// Called from the TinyUSB task when host sends serial data (not ISR context).
void tinyusb_cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    if (input_queue == NULL) {
        return;
    }

    size_t rx_size = 0;
    do {
        esp_err_t ret = tinyusb_cdcacm_read(itf, rx_buf, sizeof(rx_buf) - 1, &rx_size);
        if (ret != ESP_OK || rx_size == 0) {
            break;
        }

        for (size_t i = 0; i < rx_size; i++) {
            xQueueSend(input_queue, &rx_buf[i], 0);
        }
    } while (rx_size == sizeof(rx_buf) - 1);
}

// 1. THE INTERCEPTOR (Sends ESP_LOG data to Queue)
int app_log_vprintf(const char *fmt, va_list args) {
    if (!log_queue) return 0;
    
    log_msg_t msg;
    // Format the string
    msg.len = vsnprintf(msg.buffer, LOG_MSG_MAX_LEN - 2, fmt, args);
    
    // FIX FOR "SAME LINE" ISSUE:
    // Append \r (Carriage Return) before the \n (Line Feed)
    // Most logs end in \n. We replace \n with \r\n.
    if (msg.len > 0 && msg.buffer[msg.len - 1] == '\n') {
        msg.buffer[msg.len - 1] = '\r';
        msg.buffer[msg.len] = '\n';
        msg.len += 1; // We added a char
        msg.buffer[msg.len] = 0; // Null terminate
    }

    xQueueSend(log_queue, &msg, 0);
    return msg.len;
}

// 2. THE USB TASK (Adapted from the Example)
// Instead of echoing RX data, this consumes TX data from your app
void usb_log_task(void *arg) {
    log_msg_t msg;
    while (1) {
        if (xQueueReceive(log_queue, &msg, portMAX_DELAY) == pdTRUE) {
            tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (uint8_t*)msg.buffer, msg.len);
            tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, 0);
        }
    }
}


// 3. USB INITIALIZATION (EXACTLY AS PER EXAMPLE)
void setup_usb_from_example() {
    if (s_usb_installed) {
        ESP_LOGI("MAIN", "USB driver already installed, skipping install");
        return;
    }
    ESP_LOGI("MAIN", "USB initialization");
    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = &tinyusb_cdc_rx_callback, // <--- REGISTERED CALLBACK HERE
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL
    };

    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&acm_cfg));
    s_usb_installed = true;
    ESP_LOGI("MAIN", "USB initialization DONE");
}

void flush_usb_logs() {
    if (log_queue) {
        int retries = 0;
        // Wait up to 200ms (20 * 10ms) for the log queue to be processed by usb_log_task
        while (uxQueueMessagesWaiting(log_queue) > 0 && retries < 20) {
            vTaskDelay(pdMS_TO_TICKS(10));
            retries++;
        }
    }
    // Flush the TinyUSB hardware buffer
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(50));
    vTaskDelay(pdMS_TO_TICKS(50));
}

void uninstall_usb() {
    if (s_usb_installed) {
        flush_usb_logs();
        tinyusb_cdcacm_deinit(TINYUSB_CDC_ACM_0);
        tinyusb_driver_uninstall();
        s_usb_installed = false;
    }
}

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
