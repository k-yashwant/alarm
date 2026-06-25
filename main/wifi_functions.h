//wifi_functions.h
#ifndef WIFI_FUNCTIONS_H
#define WIFI_FUNCTIONS_H

#include "alarm_storage_structures.h"

void connect_campus_wifi();
void connect_home_wifi();
void connect_wifi(int option);

void sync_time();

void parse_alarm_json();


#endif


