#ifndef ALARM_STORAGE_STRUCTURES_H
#define ALARM_STORAGE_STRUCTURES_H

#include <stdint.h>
#define MAX_ALARMS 10
#define MINUTES_IN_A_DAY 1440

typedef struct {
    uint8_t hour;     // 0-23
    uint8_t minute;   // 0-59
    uint8_t days;     // Bitmask (1=Mon, 2=Tue, etc)
} alarm_entry_t;

typedef struct {
    alarm_entry_t alarms[MAX_ALARMS];
    uint8_t count;    // How many alarms are actually set
} alarm_storage_t;

typedef struct {
    uint16_t minutes_since_day;
    uint8_t day_bitmask;
} current_time_t;


typedef struct {
    uint32_t time_remaining;
    uint8_t day_offset;
} alarm_epoch_t;


extern alarm_storage_t current_alarm_config;

#endif