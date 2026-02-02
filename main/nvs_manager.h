#ifndef NVS_MANAGER_H
#define NVS_MANAGER_H
#include "alarm_storage_structures.h"
#include <time.h>


void init_nvs(void);
void save_alarms_to_nvs(void);
void print_alarms(void);
bool fetch_nearest_alarm_timestamp(time_t *nearest_alarm_timestamp);
alarm_epoch_t trigger_time_remaining(alarm_entry_t *candidate, current_time_t *current_time);
#endif