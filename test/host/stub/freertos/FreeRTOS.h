/**
 * @file FreeRTOS.h
 * @brief Host stand-in: the few scheduler names this firmware uses.
 */

#pragma once

#include <stdint.h>

typedef uint32_t TickType_t;

/** One tick per millisecond, which is the default this firmware is built with. */
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

#define pdPASS 1
#define pdFAIL 0
