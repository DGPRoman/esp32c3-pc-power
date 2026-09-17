/**
 * @file esp_log.h
 * @brief Host stand-in for ESP-IDF's logging macros.
 *
 * Arguments are still evaluated by the compiler's format checking — passing the
 * wrong type to a log line is a real defect and one a host build can catch — but
 * nothing is printed, because a test that passes should say nothing.
 */

#pragma once

#include <stdio.h>

#define ESP_LOG_SINK(tag, fmt, ...)                        \
    do {                                                   \
        if (0) {                                           \
            (void)fprintf(stderr, "%s" fmt, tag, ##__VA_ARGS__); \
        }                                                  \
    } while (0)

#define ESP_LOGE(tag, fmt, ...) ESP_LOG_SINK(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) ESP_LOG_SINK(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) ESP_LOG_SINK(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) ESP_LOG_SINK(tag, fmt, ##__VA_ARGS__)
#define ESP_LOGV(tag, fmt, ...) ESP_LOG_SINK(tag, fmt, ##__VA_ARGS__)
