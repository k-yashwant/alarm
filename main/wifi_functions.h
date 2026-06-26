//wifi_functions.h
#ifndef WIFI_FUNCTIONS_H
#define WIFI_FUNCTIONS_H

#include <stdbool.h>
#include "alarm_storage_structures.h"

void connect_campus_wifi();
void connect_home_wifi();
void connect_wifi(int option);

void sync_time();

bool parse_alarm_json();

/**
 * @brief Send a structured log entry to Firebase RTDB under /logs.
 *
 * Uses HTTP POST so Firebase auto-generates a unique key per entry.
 * Safe to call only after WiFi + time are available.
 *
 * @param event   Short event tag, e.g. "BOOT", "ALARM_TRIGGERED"
 * @param message Human-readable detail string
 */
void firebase_send_log(const char *event, const char *message);

#endif

