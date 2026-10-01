//


#include <string.h>
#include "esp_wifi.h"
#include "esp_eap_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "freertos/event_groups.h"
#include "esp_err.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "wifi_functions.h"
#include "firebase_secrets.h"
#include "wifi_secrets.h"
#include <time.h>
#include <stdlib.h>
#include <sys/time.h>
#include "esp_sntp.h"
#include "esp_netif_sntp.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "system_events.h"

extern const char firebase_roots_pem_start[] asm("_binary_firebase_roots_pem_start");
extern const char firebase_roots_pem_end[]   asm("_binary_firebase_roots_pem_end");


#define FIREBASE_ID_TOKEN_SIZE 2048
#define FIREBASE_URL_BUFFER_SIZE 2300
#define FIREBASE_AUTH_RESPONSE_BUFFER_SIZE 4096
#define FIREBASE_HTTP_RX_BUFFER_SIZE 2048
#define FIREBASE_HTTP_TX_BUFFER_SIZE 2048

// Mutex to serialize all HTTPS/TLS operations — prevents concurrent handshakes
// that exhaust the ESP32-S2's limited internal SRAM (~320 KB).
static SemaphoreHandle_t s_firebase_http_mutex = NULL;

static void init_firebase_http_mutex(void)
{
    if (s_firebase_http_mutex == NULL) {
        s_firebase_http_mutex = xSemaphoreCreateMutex();
    }
}

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
static char firebase_id_token[FIREBASE_ID_TOKEN_SIZE];
static time_t firebase_token_expires_at = 0;
#define MAX_WIFI_RETRIES 5
static int s_retry_num = 0;


static bool firebase_auth_is_configured(void)
{
    return strcmp(FIREBASE_WEB_API_KEY, "PUT_FIREBASE_WEB_API_KEY_HERE") != 0 &&
           strcmp(FIREBASE_AUTH_EMAIL, "PUT_FIREBASE_EMAIL_HERE") != 0 &&
           strcmp(FIREBASE_AUTH_PASSWORD, "PUT_FIREBASE_PASSWORD_HERE") != 0;
}

static bool http_read_all(esp_http_client_handle_t client, char *buffer, size_t buffer_size)
{
    int total_read_len = 0;

    while (total_read_len < (int)buffer_size - 1) {
        int read_len = esp_http_client_read(client,
                                            buffer + total_read_len,
                                            buffer_size - 1 - total_read_len);
        if (read_len < 0) {
            ESP_LOGE(TAG, "HTTP read failed: %d", read_len);
            buffer[0] = '\0';
            return false;
        }
        if (read_len == 0) {
            break;
        }
        total_read_len += read_len;
    }

    buffer[total_read_len] = '\0';
    return true;
}

static bool firebase_refresh_id_token(void)
{
    if (!firebase_auth_is_configured()) {
        ESP_LOGE(TAG, "Firebase Auth is not configured. Set FIREBASE_WEB_API_KEY, FIREBASE_AUTH_EMAIL, and FIREBASE_AUTH_PASSWORD.");
        return false;
    }

    time_t now;
    time(&now);
    if (firebase_id_token[0] != '\0' && now < firebase_token_expires_at - 60) {
        return true;
    }

    ESP_LOGI(TAG, "Firebase Auth: refreshing ID token via Google Identity Toolkit...");

    static char auth_url[256];
    snprintf(auth_url, sizeof(auth_url),
             "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=%s",
             FIREBASE_WEB_API_KEY);

    static char post_body[512];
    snprintf(post_body, sizeof(post_body),
             "{\"email\":\"%s\",\"password\":\"%s\",\"returnSecureToken\":true}",
             FIREBASE_AUTH_EMAIL, FIREBASE_AUTH_PASSWORD);

    if (s_firebase_http_mutex != NULL) {
        xSemaphoreTake(s_firebase_http_mutex, portMAX_DELAY);
    }

    esp_http_client_config_t config = {
        .url = auth_url,
        .method = HTTP_METHOD_POST,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .cert_pem = firebase_roots_pem_start,
        .buffer_size = FIREBASE_HTTP_RX_BUFFER_SIZE,
        .buffer_size_tx = FIREBASE_HTTP_TX_BUFFER_SIZE,
        .timeout_ms = 15000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Firebase Auth: failed to init HTTP client");
        if (s_firebase_http_mutex != NULL) xSemaphoreGive(s_firebase_http_mutex);
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");

    ESP_LOGI(TAG, "Firebase Auth: connecting to identitytoolkit.googleapis.com...");
    esp_err_t err = esp_http_client_open(client, strlen(post_body));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Firebase Auth: open failed (%s)", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        if (s_firebase_http_mutex != NULL) xSemaphoreGive(s_firebase_http_mutex);
        return false;
    }

    int written = esp_http_client_write(client, post_body, strlen(post_body));
    if (written < 0) {
        ESP_LOGE(TAG, "Firebase Auth: write failed");
        esp_http_client_cleanup(client);
        if (s_firebase_http_mutex != NULL) xSemaphoreGive(s_firebase_http_mutex);
        return false;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);

    static char auth_response[FIREBASE_AUTH_RESPONSE_BUFFER_SIZE];
    bool read_ok = http_read_all(client, auth_response, sizeof(auth_response));
    esp_http_client_cleanup(client);
    if (s_firebase_http_mutex != NULL) xSemaphoreGive(s_firebase_http_mutex);




    if (!read_ok) {
        return false;
    }

    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "Firebase Auth failed: HTTP %d, response: %s", status, auth_response);
        return false;
    }

    cJSON *root = cJSON_Parse(auth_response);
    if (root == NULL) {
        ESP_LOGE(TAG, "Firebase Auth: invalid JSON response");
        return false;
    }

    cJSON *id_token = cJSON_GetObjectItemCaseSensitive(root, "idToken");
    cJSON *expires_in = cJSON_GetObjectItemCaseSensitive(root, "expiresIn");
    cJSON *local_id = cJSON_GetObjectItemCaseSensitive(root, "localId");
    if (!cJSON_IsString(id_token) || strlen(id_token->valuestring) >= sizeof(firebase_id_token)) {
        ESP_LOGE(TAG, "Firebase Auth: missing or oversized idToken");
        cJSON_Delete(root);
        return false;
    }

    strlcpy(firebase_id_token, id_token->valuestring, sizeof(firebase_id_token));
    int expires_sec = cJSON_IsString(expires_in) ? atoi(expires_in->valuestring) : 3600;
    time(&now);
    firebase_token_expires_at = now + expires_sec;

    ESP_LOGI(TAG, "Firebase Auth: signed in as %s", FIREBASE_AUTH_EMAIL);
    if (cJSON_IsString(local_id)) {
        ESP_LOGI(TAG, "Firebase Auth UID: %s", local_id->valuestring);
    }
    cJSON_Delete(root);
    return true;
}

static bool firebase_build_rtdb_url(const char *path, char *url, size_t url_size)
{
    if (!firebase_refresh_id_token()) {
        return false;
    }

    int written = snprintf(url, url_size,
                           "https://%s/users/%s%s.json?auth=%s",
                           FIREBASE_HOST,
                           FIREBASE_USER_UID,
                           path,
                           firebase_id_token);
    if (written < 0 || written >= (int)url_size) {
        ESP_LOGE(TAG, "Firebase RTDB URL is too long");
        return false;
    }

    return true;
}


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
        if (s_wifi_event_group != NULL) {
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
        
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
        if (s_wifi_event_group != NULL) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
        s_retry_num = 0;
    }
}

void connect_campus_wifi(){

    init_firebase_http_mutex();

    if (is_wifi_initialized){
        ESP_LOGI(TAG, "Wifi Already Initialised");
        EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
        if (!wifi_ip || (bits & WIFI_CONNECTED_BIT) == 0) {
            ESP_LOGI(TAG, "WiFi initialized but disconnected — reconnecting...");
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            s_retry_num = 0;
            esp_wifi_connect();
        }
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

    init_firebase_http_mutex();

    if (is_wifi_initialized){
        ESP_LOGI(TAG, "Wifi Already Initialised");
        EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
        if (!wifi_ip || (bits & WIFI_CONNECTED_BIT) == 0) {
            ESP_LOGI(TAG, "WiFi initialized but disconnected — reconnecting...");
            xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            s_retry_num = 0;
            esp_wifi_connect();
        }
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
        esp_sntp_setservername(1, "pool.ntp.org");
        esp_sntp_setservername(2, "time.nist.gov");

        EventBits_t timebits = xEventGroupWaitBits(s_wifi_event_group,
                        TIME_SYNCED_BIT,
                        pdFALSE,        // Do not clear the bit (keep it set for others)
                        pdTRUE,         // WaitForAllBits (doesn't matter for single bit)
                        pdMS_TO_TICKS(20000));

        if (timebits & TIME_SYNCED_BIT){
            ESP_LOGI(TAG, "Successful Synchronization. Callback sync_time");
        }else{
            ESP_LOGE(TAG, "Couldn't sync. Timeout 20 seconds");
            time_t now;
            time(&now);
            struct tm timeinfo;
            localtime_r(&now, &timeinfo);
            char time_str[64];
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
            ESP_LOGW(TAG, "NTP sync failed. Current system clock: %s", time_str);
        }
    } else {
        ESP_LOGE(TAG, "Timeout: Failed to connect to WiFi within 20 seconds.");
    }
}


bool parse_alarm_json(void) {
    ESP_LOGI(TAG, "Waiting for WiFi connection (Parse json)");

    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "WiFi event group not initialized");
        return false;
    }

    // Wait for WiFi only — TIME_SYNCED_BIT may already be set from boot SNTP,
    // or may never be set again on wakeup cycles. Don't block on it.
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_CONNECTED_BIT,
        pdFALSE,
        pdTRUE,
        pdMS_TO_TICKS(10000)); // 10 Second Timeout

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "parse_alarm_json: WiFi connected, resolving Firebase URL...");

        // 1. Initialize an empty config to return in case of failure
        alarm_storage_t result;
        result.count = 0; // Default to 0 alarms
        memset(result.alarms, 0, sizeof(result.alarms));

        // 2. Define a Static Buffer (No malloc!)
        // We make it static so it doesn't consume the Task Stack
        static char rx_buffer[BUFFER_SIZE+1]; 
        memset(rx_buffer, 0, BUFFER_SIZE);

        static char firebase_url[FIREBASE_URL_BUFFER_SIZE];
        if (!firebase_build_rtdb_url("/alarms", firebase_url, sizeof(firebase_url))) {
            ESP_LOGE(TAG, "parse_alarm_json: firebase_build_rtdb_url failed");
            return false;
        }

        // Serialize TLS — taken after token refresh completes to prevent recursion/deadlock
        if (s_firebase_http_mutex != NULL) {
            xSemaphoreTake(s_firebase_http_mutex, portMAX_DELAY);
        }

        ESP_LOGI(TAG, "parse_alarm_json: connecting to Firebase RTDB...");

        // 3. Configure HTTP Client
        esp_http_client_config_t config = {
            .url = firebase_url,
            .method = HTTP_METHOD_GET,
            .transport_type = HTTP_TRANSPORT_OVER_SSL,
            .cert_pem = firebase_roots_pem_start,
            .buffer_size = FIREBASE_HTTP_RX_BUFFER_SIZE,
            .buffer_size_tx = FIREBASE_HTTP_TX_BUFFER_SIZE,
            .timeout_ms = 15000,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client == NULL) {
            ESP_LOGE(TAG, "Failed to initialize HTTP client");
            if (s_firebase_http_mutex != NULL) xSemaphoreGive(s_firebase_http_mutex);
            return false;
        }

        esp_err_t err = esp_http_client_open(client, 0);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open connection (%s)", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            if (s_firebase_http_mutex != NULL) xSemaphoreGive(s_firebase_http_mutex);
            return false;
        }


        // 4. Fetch Headers (Determine content length)
        int content_length = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status < 200 || status >= 300) {
            if (http_read_all(client, rx_buffer, sizeof(rx_buffer))) {
                ESP_LOGE(TAG, "Firebase alarm fetch denied/failed: HTTP %d, response: %s", status, rx_buffer);
            } else {
                ESP_LOGE(TAG, "Firebase alarm fetch denied/failed: HTTP %d", status);
            }
            esp_http_client_cleanup(client);
            if (s_firebase_http_mutex != NULL) xSemaphoreGive(s_firebase_http_mutex);
            return false;
        }

        // Safety check: Is data too big for our static buffer?
        // Only check if content_length is positive/known.
        if (content_length >= BUFFER_SIZE) {
            ESP_LOGE(TAG, "Error: JSON too large (%d bytes) for buffer (%d bytes)\n", content_length, BUFFER_SIZE);
            esp_http_client_cleanup(client);
            if (s_firebase_http_mutex != NULL) xSemaphoreGive(s_firebase_http_mutex);
            return false;
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
                esp_http_client_cleanup(client);
                if (s_firebase_http_mutex != NULL) xSemaphoreGive(s_firebase_http_mutex);
                return false;
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

        // Release the mutex now — we have the data, JSON parsing is CPU-only
        esp_http_client_cleanup(client);
        if (s_firebase_http_mutex != NULL) xSemaphoreGive(s_firebase_http_mutex);

        // 6. Parse JSON with cJSON
        cJSON *root = cJSON_Parse(rx_buffer);
        if (root == NULL) {
            printf("Error parsing JSON syntax\n");
            return false;
        }

        cJSON *alarms_array = cJSON_IsArray(root) ? root : cJSON_GetObjectItemCaseSensitive(root, "alarms");
        if (!cJSON_IsArray(alarms_array)) {
            ESP_LOGE(TAG, "Firebase response is not an alarms array");
            cJSON_Delete(root);
            return false;
        }

        // 7. Extract Data into Struct
        int array_size = cJSON_GetArraySize(alarms_array);
        
        // Cap at MAX_ALARMS
        if (array_size > MAX_ALARMS) array_size = MAX_ALARMS;

        int valid_count = 0;
        for (int i = 0; i < array_size; i++) {
            cJSON *item = cJSON_GetArrayItem(alarms_array, i);
            cJSON *timeStr = cJSON_GetObjectItem(item, "time"); // "08:30"
            cJSON *days = cJSON_GetObjectItem(item, "days");    // 65 (int)

            if (cJSON_IsString(timeStr) && cJSON_IsNumber(days)) {
                int h, m;
                // Parse "HH:MM" string
                if (sscanf(timeStr->valuestring, "%d:%d", &h, &m) != 2 ||
                    h < 0 || h > 23 || m < 0 || m > 59) {
                    ESP_LOGW(TAG, "Skipping invalid alarm time: %s", timeStr->valuestring);
                    continue;
                }
                
                result.alarms[valid_count].hour = (uint8_t)h;
                result.alarms[valid_count].minute = (uint8_t)m;
                result.alarms[valid_count].days = (uint8_t)days->valueint;
                
                printf("Parsed Alarm %d: %02d:%02d Days:%d\n", valid_count, h, m, result.alarms[valid_count].days);
                valid_count++;
            }
        }
        result.count = valid_count;

        // 8. Cleanup
        cJSON_Delete(root); // Free cJSON internal memory

        current_alarm_config = result;
        return result.count > 0;
    }else{
        ESP_LOGE(TAG, "Couldn't connect to wifi. Timeout 20 seconds");
        return false;
    }
}

// ---------------------------------------------------------------------------
// Firebase Logging
// ---------------------------------------------------------------------------

// Struct for background logging queue
typedef struct {
    char event[32];
    char message[128];
} firebase_log_msg_t;

static QueueHandle_t firebase_log_queue = NULL;

static void firebase_send_log_perform(const char *event, const char *message)
{
    // ---- 1. Build JSON payload ----
    time_t now;
    time(&now);

    char timestamp_str[32];
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    strftime(timestamp_str, sizeof(timestamp_str), "%Y-%m-%dT%H:%M:%S+05:30", &timeinfo);

    // Escape any double-quotes in the message to keep JSON valid
    // (Simple approach – replace with escaped version in a temp buffer)
    static char safe_msg[256];
    int j = 0;
    for (int i = 0; message[i] != '\0' && j < (int)sizeof(safe_msg) - 2; i++) {
        if (message[i] == '"') safe_msg[j++] = '\\';
        safe_msg[j++] = message[i];
    }
    safe_msg[j] = '\0';

    // Firebase POST body: auto-key entry with event + message + timestamp
    static char post_body[512];
    snprintf(post_body, sizeof(post_body),
             "{\"event\":\"%s\",\"message\":\"%s\",\"timestamp\":\"%s\",\"unix\":%lld}",
             event, safe_msg, timestamp_str, (long long)now);

    static char firebase_url[FIREBASE_URL_BUFFER_SIZE];
    if (!firebase_build_rtdb_url("/logs", firebase_url, sizeof(firebase_url))) {
        return;
    }

    // ---- 2. Send HTTP POST (serialized with mutex) ----
    if (s_firebase_http_mutex != NULL) {
        xSemaphoreTake(s_firebase_http_mutex, portMAX_DELAY);
    }

    esp_http_client_config_t config = {
        .url            = firebase_url,
        .method         = HTTP_METHOD_POST,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .cert_pem       = firebase_roots_pem_start,
        .buffer_size = FIREBASE_HTTP_RX_BUFFER_SIZE,
        .buffer_size_tx = FIREBASE_HTTP_TX_BUFFER_SIZE,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "firebase_send_log: failed to init HTTP client");
        if (s_firebase_http_mutex != NULL) xSemaphoreGive(s_firebase_http_mutex);
        return;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");

    esp_err_t err = esp_http_client_open(client, strlen(post_body));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "firebase_send_log: open failed (%s)", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        if (s_firebase_http_mutex != NULL) xSemaphoreGive(s_firebase_http_mutex);
        return;
    }

    int written = esp_http_client_write(client, post_body, strlen(post_body));
    if (written < 0) {
        ESP_LOGE(TAG, "firebase_send_log: write failed");
    } else {
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "firebase_send_log: [%s] %s -> HTTP %d", event, message, status);
        if (status == 401 || status == 403) {
            ESP_LOGE(TAG, "firebase_send_log: permission denied by Firebase rules/auth");
        }
    }

    esp_http_client_cleanup(client);
    if (s_firebase_http_mutex != NULL) xSemaphoreGive(s_firebase_http_mutex);
}

static void firebase_log_task(void *pvParameters)
{
    firebase_log_msg_t msg;
    ESP_LOGI(TAG, "Firebase background logging task started");
    while (1) {
        if (xQueueReceive(firebase_log_queue, &msg, portMAX_DELAY) == pdTRUE) {
            if (s_wifi_event_group == NULL) {
                ESP_LOGW(TAG, "firebase_log_task: WiFi not initialized, skipping %s", msg.event);
                continue;
            }

            EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
            if ((bits & WIFI_CONNECTED_BIT) == 0) {
                ESP_LOGW(TAG, "firebase_log_task: WiFi not connected, skipping %s", msg.event);
                continue;
            }

            // Wait for time sync before sending — TLS cert validation requires valid clock
            if ((bits & TIME_SYNCED_BIT) == 0) {
                ESP_LOGW(TAG, "firebase_log_task: Time not synced yet, skipping log %s", msg.event);
                continue;
            }

            firebase_send_log_perform(msg.event, msg.message);
        }
    }
}

void firebase_send_log(const char *event, const char *message)
{
    // Remote logging is disabled while the alarm data path is validated.
    // USB logging remains active through app_log_vprintf().
    (void)event;
    (void)message;
}
