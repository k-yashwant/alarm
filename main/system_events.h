#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

extern EventGroupHandle_t s_wifi_event_group;

/* bits */
#define WIFI_CONNECTED_BIT   BIT0
#define TIME_SYNCED_BIT      BIT1