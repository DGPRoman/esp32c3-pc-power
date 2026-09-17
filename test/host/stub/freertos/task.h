/**
 * @file task.h
 * @brief Host stand-in for task creation and delays.
 *
 * Neither does anything. The host tests call the parsers directly and never start
 * the server task, so a task that is created and never runs is the accurate
 * stand-in — one that actually ran would make these tests concurrent for no
 * reason.
 */

#pragma once

#include "freertos/FreeRTOS.h"

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

void vTaskDelay(TickType_t ticks);

int xTaskCreate(TaskFunction_t entry, const char *name, uint32_t stack_depth, void *argument,
                uint32_t priority, TaskHandle_t *created);
