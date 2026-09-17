/**
 * @file esp_err.h
 * @brief Host stand-in for ESP-IDF's error type.
 *
 * Only the values this firmware names. A component that starts returning some
 * other ESP_ERR_ will fail to compile here, which is the point: the stub is a
 * list of what the host build is allowed to depend on, not a blanket.
 */

#pragma once

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL (-1)

#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
