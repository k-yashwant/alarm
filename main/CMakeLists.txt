execute_process(
    COMMAND python3 ${CMAKE_CURRENT_LIST_DIR}/../scripts/generate_firebase_secrets.py
            ${CMAKE_CURRENT_LIST_DIR}/../.env
            ${CMAKE_CURRENT_SOURCE_DIR}/firebase_secrets.h
    RESULT_VARIABLE FIREBASE_SECRETS_RESULT
)

if(NOT FIREBASE_SECRETS_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to generate Firebase secrets header. Create ${CMAKE_CURRENT_LIST_DIR}/../.env from .env.example.")
endif()

idf_component_register(SRCS "oled_display.c" "usb_led_functions.c" "main.c" "nvs_manager.c" "wifi_functions.c" "alarm_trigger_event.c" "alarm_storage_structures.c"
                    INCLUDE_DIRS ".")
