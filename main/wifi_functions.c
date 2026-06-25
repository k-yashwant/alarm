//


#include <string.h>
#include "esp_wifi.h"
#include "esp_eap_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/event_groups.h"
#include "esp_err.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "wifi_functions.h"
#include <time.h>
#include <sys/time.h>
#include "esp_sntp.h"
#include "esp_netif_sntp.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "system_events.h"



#define HOME_WIFI_SSID "your_wifi_ssid"
#define HOME_WIFI_PASSWORD "your_wifi_password"

#define campus_username "your_campus_username"
#define campus_password "your_campus_password"
#define FIREBASE_DATABASE_URL "https://alarm-scheduler-webpage-default-rtdb.asia-southeast1.firebasedatabase.app/users/CquQEV5STZbTEd7OglH1SyUdYGB2.json?auth=PdT6EgfNCzxJJptRRO06zt6Bpred8EiMMQd0hyJH"
const char *firebase_root_cert = \
"-----BEGIN CERTIFICATE-----\n" \
"MIIFVzCCAz+gAwIBAgINAgPlk28xsBNJiGuiFzANBgkqhkiG9w0BAQwFADBHMQsw\n" \
"CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU\n" \
"MBIGA1UEAxMLR1RTIFJvb3QgUjEwHhcNMTYwNjIyMDAwMDAwWhcNMzYwNjIyMDAw\n" \
"MDAwWjBHMQswCQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZp\n" \
"Y2VzIExMQzEUMBIGA1UEAxMLR1RTIFJvb3QgUjEwggIiMA0GCSqGSIb3DQEBAQUA\n" \
"A4ICDwAwggIKAoICAQC2EQKLHuOhd5s73L+UPreVp0A8of2C+X0yBoJx9vaMf/vo\n" \
"27xqLpeXo4xL+Sv2sfnOhB2x+cWX3u+58qPpvBKJXqeqUqv4IyfLpLGcY9vXmX7w\n" \
"Cl7raKb0xlpHDU0QM+NOsROjyBhsS+z8CZDfnWQpJSMHobTSPS5g4M/SCYe7zUjw\n" \
"TcLCeoiKu7rPWRnWr4+wB7CeMfGCwcDfLqZtbBkOtdh+JhpFAz2weaSUKK0Pfybl\n" \
"qAj+lug8aJRT7oM6iCsVlgmy4HqMLnXWnOunVmSPlk9orj2XwoSPwLxAwAtcvfaH\n" \
"szVsrBhQf4TgTM2S0yDpM7xSma8ytSmzJSq0SPly4cpk9+aCEI3oncKKiPo4Zor8\n" \
"Y/kB+Xj9e1x3+naH+uzfsQ55lVe0vSbv1gHR6xYKu44LtcXFilWr06zqkUspzBmk\n" \
"MiVOKvFlRNACzqrOSbTqn3yDsEB750Orp2yjj32JgfpMpf/VjsPOS+C12LOORc92\n" \
"wO1AK/1TD7Cn1TsNsYqiA94xrcx36m97PtbfkSIS5r762DL8EGMUUXLeXdYWk70p\n" \
"aDPvOmbsB4om3xPXV2V4J95eSRQAogB/mqghtqmxlbCluQ0WEdrHbEg8QOB+DVrN\n" \
"VjzRlwW5y0vtOUucxD/SVRNuJLDWcfr0wbrM7Rv1/oFB2ACYPTrIrnqYNxgFlQID\n" \
"AQABo0IwQDAOBgNVHQ8BAf8EBAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4E\n" \
"FgQU5K8rJnEaK0gnhS9SZizv8IkTcT4wDQYJKoZIhvcNAQEMBQADggIBAJ+qQibb\n" \
"C5u+/x6Wki4+omVKapi6Ist9wTrYggoGxval3sBOh2Z5ofmmWJyq+bXmYOfg6LEe\n" \
"QkEzCzc9zolwFcq1JKjPa7XSQCGYzyI0zzvFIoTgxQ6KfF2I5DUkzps+GlQebtuy\n" \
"h6f88/qBVRRiClmpIgUxPoLW7ttXNLwzldMXG+gnoot7TiYaelpkttGsN/H9oPM4\n" \
"7HLwEXWdyzRSjeZ2axfG34arJ45JK3VmgRAhpuo+9K4l/3wV3s6MJT/KYnAK9y8J\n" \
"ZgfIPxz88NtFMN9iiMG1D53Dn0reWVlHxYciNuaCp+0KueIHoI17eko8cdLiA6Ef\n" \
"MgfdG+RCzgwARWGAtQsgWSl4vflVy2PFPEz0tv/bal8xa5meLMFrUKTX5hgUvYU/\n" \
"Z6tGn6D/Qqc6f1zLXbBwHSs09dR2CQzreExZBfMzQsNhFRAbd03OIozUhfJFfbdT\n" \
"6u9AWpQKXCBfTkBdYiJ23//OYb2MI3jSNwLgjt7RETeJ9r/tSQdirpLsQBqvFAnZ\n" \
"0E6yove+7u7Y/9waLd64NnHi/Hm3lCXRSHNboTXns5lndcEZOitHTtNCjv0xyBZm\n" \
"2tIMPNuzjsmhDYAPexZ3FL//2wmUspO8IFgV6dtxQ/PeEMMA3KgqlbbC1j+Qa3bb\n" \
"bP6MvPJwNQzcmRk13NfIRmPVNnGuV/u3gm3c\n" \
"-----END CERTIFICATE-----\n";

#define CAMPUS_WIFI_SSID "CAMPUS_SECURED"
#define EAP_METHOD ESP_EAP_TYPE_PEAP


#define EAP_ID campus_username
#define EAP_USERNAME campus_username
#define EAP_PASSWORD campus_password

#define BUFFER_SIZE 2048


static const char *TAG = "Wifi_Functions";
static bool wifi_ip = false;
EventGroupHandle_t s_wifi_event_group;
static bool is_wifi_initialized = false;
#define MAX_WIFI_RETRIES 5
static int s_retry_num = 0;


static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi Started, connecting...");
        s_retry_num = 0;
        esp_wifi_connect();
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        // 1. Clear the bit! This tells sync_time (and other tasks) we are offline
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        
        // 2. IMPORTANT: Update your boolean too
        wifi_ip = false; 
        
        if (s_retry_num < MAX_WIFI_RETRIES) {
            ESP_LOGI(TAG, "Disconnected. Retrying (%d/%d)...", s_retry_num + 1, MAX_WIFI_RETRIES);
            esp_wifi_connect();
            s_retry_num++;
        } else {
            ESP_LOGE(TAG, "Max retries (%d) reached. Failed to connect to WiFi.", MAX_WIFI_RETRIES);
        }
    } 
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Connected! Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        
        // 1. Set the boolean
        wifi_ip = true;

        // 2. Set the Event Bit! 
        // This effectively "signals" the sync_time function to wake up immediately.
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        s_retry_num = 0;
    }
}

void connect_campus_wifi(){

    if (is_wifi_initialized){
        ESP_LOGI(TAG, "Wifi Already Initialised");
        return;
    }
    ESP_LOGI(TAG, "Creating wifi event group");
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Creating Default Loop");
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CAMPUS_WIFI_SSID,
            // Password in wifi_config_t is ignored for Enterprise, 
            // but we leave pmf settings to standard
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        }
    };

    ESP_LOGI(TAG, "Setting WiFi configuration...");
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    ESP_ERROR_CHECK(esp_eap_client_set_identity((const unsigned char *) EAP_ID, strlen(EAP_ID)));
    ESP_ERROR_CHECK(esp_eap_client_set_username((const unsigned char *) EAP_USERNAME, strlen(EAP_USERNAME)));
    ESP_ERROR_CHECK(esp_eap_client_set_password((const unsigned char *) EAP_PASSWORD, strlen(EAP_PASSWORD)));
    ESP_ERROR_CHECK(esp_eap_client_set_eap_methods(EAP_METHOD));
    ESP_ERROR_CHECK(esp_wifi_sta_enterprise_enable());

    ESP_LOGI(TAG, "Starting WiFi...");
    ESP_ERROR_CHECK(esp_wifi_start());
    is_wifi_initialized = true; 
}

void connect_home_wifi(){

    if (is_wifi_initialized){
        ESP_LOGI(TAG, "Wifi Already Initialised");
        return;
    }
    ESP_LOGI(TAG, "Creating wifi event group");
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Creating Default Loop");
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    wifi_config_t wifi_config = {
    .sta = {
        .ssid = HOME_WIFI_SSID,
        .password = HOME_WIFI_PASSWORD,   // <-- Required for WPA2-Personal
        .threshold.authmode = WIFI_AUTH_WPA2_PSK,  // Optional but recommended
        .pmf_cfg = {
            .capable = true,
            .required = false
        },
    }
};

    ESP_LOGI(TAG, "Setting WiFi configuration...");

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    ESP_LOGI(TAG, "Starting WiFi...");
    ESP_ERROR_CHECK(esp_wifi_start());

    is_wifi_initialized = true;
}

void connect_wifi(int option){
    if (option == 0){
        connect_campus_wifi();
    }else if (option == 1){
        connect_home_wifi();
    }
}

static void time_sync_cb(struct timeval *tv)
{
    ESP_LOGI("NTP", "Time synchronized");
    xEventGroupSetBits(s_wifi_event_group, TIME_SYNCED_BIT);
    esp_netif_sntp_deinit();
}

void sync_time(void)
{
    ESP_LOGI(TAG, "Waiting for WiFi connection to sync time...");

    // Wait here until WIFI_CONNECTED_BIT is set, or 10 seconds pass.
    // pdFALSE = Don't clear the bit after exit (keep us known as connected)
    // pdTRUE = Wait for all bits (doesn't matter here as we only look for one)
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT,
            pdFALSE,
            pdTRUE,
            pdMS_TO_TICKS(20000)); // 10 Second Timeout

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi Connected. Initializing SNTP...");
        
        // Prevent re-initialization if called multiple times
        if(esp_sntp_enabled()){
            ESP_LOGI(TAG, "SNTP already initialized.");
            return;
        }

        // Clear the time synced event bit so we can wait for the new sync callback
        xEventGroupClearBits(s_wifi_event_group, TIME_SYNCED_BIT);

        setenv("TZ", "IST-5:30", 1);
        tzset();

        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("time.google.com");
        cfg.sync_cb = time_sync_cb;
        cfg.wait_for_sync = false; // Do not block here, let callback handle it
        
        esp_netif_sntp_init(&cfg);
        EventBits_t timebits = xEventGroupWaitBits(s_wifi_event_group,
                        TIME_SYNCED_BIT,
                        pdFALSE,        // Do not clear the bit (keep it set for others)
                        pdTRUE,         // WaitForAllBits (doesn't matter for single bit)
                        pdMS_TO_TICKS(20000));

        if (timebits & TIME_SYNCED_BIT){
            ESP_LOGI(TAG, "Successful Synchronization. Callback sync_time");
        }else{
            ESP_LOGE(TAG, "Couldn't sync. Timeout 20 seconds");
        }
    } else {
        ESP_LOGE(TAG, "Timeout: Failed to connect to WiFi within 20 seconds.");
    }
}


void parse_alarm_json(void) {
    ESP_LOGI(TAG, "Waiting for WiFi connection (Parse json)");

    // Wait here until WIFI_CONNECTED_BIT is set, or 10 seconds pass.
    // pdFALSE = Don't clear the bit after exit (keep us known as connected)
    // pdTRUE = Wait for all bits (doesn't matter here as we only look for one)
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT | TIME_SYNCED_BIT,
        pdFALSE,
        pdTRUE,
        pdMS_TO_TICKS(10000)); // 10 Second Timeout

    if (bits & WIFI_CONNECTED_BIT) {
        // 1. Initialize an empty config to return in case of failure
        alarm_storage_t result;
        result.count = 0; // Default to 0 alarms
        memset(result.alarms, 0, sizeof(result.alarms));

        // 2. Define a Static Buffer (No malloc!)
        // We make it static so it doesn't consume the Task Stack
        static char rx_buffer[BUFFER_SIZE+1]; 
        memset(rx_buffer, 0, BUFFER_SIZE);

        // 3. Configure HTTP Client
        esp_http_client_config_t config = {
            .url = FIREBASE_DATABASE_URL,
            .method = HTTP_METHOD_GET,
            .transport_type = HTTP_TRANSPORT_OVER_SSL,
            .cert_pem = firebase_root_cert,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        esp_err_t err = esp_http_client_open(client, 0);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open connection");
            esp_http_client_cleanup(client);
            return; // Return empty
        }

        // 4. Fetch Headers (Determine content length)
        int content_length = esp_http_client_fetch_headers(client);

        // Safety check: Is data too big for our static buffer?
        // Only check if content_length is positive/known.
        if (content_length >= BUFFER_SIZE) {
            ESP_LOGE(TAG, "Error: JSON too large (%d bytes) for buffer (%d bytes)\n", content_length, BUFFER_SIZE);
            esp_http_client_cleanup(client);
            return;
        }

        // 5. Read Data into Static Buffer
        int total_read_len = 0;
        int read_len = 0;

        // Loop until we read everything or buffer is full (leave 1 byte for null terminator)
        while (total_read_len < BUFFER_SIZE) {
            int read_target = BUFFER_SIZE - total_read_len;
            if (content_length > 0 && (content_length - total_read_len) < read_target) {
                read_target = content_length - total_read_len;
            }

            read_len = esp_http_client_read(client, rx_buffer + total_read_len, read_target);
            if (read_len < 0) {
                ESP_LOGE(TAG, "Error reading from HTTP client: %d", read_len);
                break;
            }
            if (read_len == 0) {
                // Connection closed or EOF
                break;
            }
            total_read_len += read_len;

            // If we know the content length and have read it all, we can stop
            if (content_length > 0 && total_read_len >= content_length) {
                break;
            }
        }
        
        // Null-terminate the string so cJSON can read it
        rx_buffer[total_read_len] = 0; 
        
        ESP_LOGI(TAG, "JSON Received: %s\n", rx_buffer);

        // 6. Parse JSON with cJSON
        cJSON *root = cJSON_Parse(rx_buffer);
        if (root == NULL) {
            printf("Error parsing JSON syntax\n");
            esp_http_client_cleanup(client);
            return;
        }

        cJSON *alarms_array = cJSON_GetObjectItemCaseSensitive(root, "alarms");
        // 7. Extract Data into Struct
        int array_size = cJSON_GetArraySize(alarms_array);
        
        // Cap at MAX_ALARMS
        if (array_size > MAX_ALARMS) array_size = MAX_ALARMS;

        result.count = array_size;
        for (int i = 0; i < array_size; i++) {
            cJSON *item = cJSON_GetArrayItem(alarms_array, i);
            cJSON *timeStr = cJSON_GetObjectItem(item, "time"); // "08:30"
            cJSON *days = cJSON_GetObjectItem(item, "days");    // 65 (int)

            if (timeStr && days) {
                int h, m;
                // Parse "HH:MM" string
                sscanf(timeStr->valuestring, "%d:%d", &h, &m);
                
                result.alarms[i].hour = (uint8_t)h;
                result.alarms[i].minute = (uint8_t)m;
                result.alarms[i].days = (uint8_t)days->valueint;
                
                printf("Parsed Alarm %d: %02d:%02d Days:%d\n", i, h, m, result.alarms[i].days);
            }
        }

        // 8. Cleanup
        cJSON_Delete(root); // Free cJSON internal memory
        esp_http_client_cleanup(client);

        current_alarm_config = result;
    }else{
        ESP_LOGE(TAG, "Couldn't connect to wifi. Timeout 20 seconds");
    }
}