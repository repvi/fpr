#pragma once

#include "freertos/FreeRTOS.h"
#include "esp_timer.h"

#define CPU_CYCLE_ONE_SECOND ((uint32_t)(240000000))
#define CPU_CYCLE_ONE_SECOND_HALF ((uint32_t)(CPU_CYCLE_ONE_SECOND / 2))

#define SECONDS_TO_US(s) ((s) * 1000000LL)
#define MS_TO_US(ms) ((ms) * 1000LL)

// Convert microseconds to other units
#define US_TO_MS(us) ((us) / 1000LL)
#define US_TO_SECONDS(us) ((us) / 1000000LL)

#define MS_TO_SECONDS(ms) ((ms) / 1000LL)
#define SECONDS_TO_MS(s) ((s) * 1000LL)

#define esp_get_current_time_ms() ((uint64_t)(esp_timer_get_time() / 1000LL))
#define esp_get_current_time_s() ((uint64_t)(esp_get_current_time_ms() / 1000LL))

#define esp_get_fps_delay_ms(delay, time_passed) ((TickType_t)(((delay) > (TickType_t)(time_passed)) ? pdMS_TO_TICKS((TickType_t)((delay) - (time_passed))) : 0))

// use for default press duration
#define PRESS_DURATION_STANDARD_TIME ((uint32_t)MS_TO_US(500))