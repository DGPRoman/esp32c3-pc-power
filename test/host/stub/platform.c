/**
 * @file platform.c
 * @brief Definitions for the stubbed platform declarations.
 *
 * Deliberately inert. The host tests call parsers and renderers directly; nothing
 * here should ever run, and a stub that did something would make these tests
 * depend on a scheduler that is not present.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hw_i2c.h"

void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
}

int xTaskCreate(TaskFunction_t entry, const char *name, uint32_t stack_depth, void *argument,
                uint32_t priority, TaskHandle_t *created)
{
    (void)entry;
    (void)name;
    (void)stack_depth;
    (void)argument;
    (void)priority;
    if (created != NULL) {
        *created = NULL;
    }
    return pdPASS;
}

/*
 * A bus with nothing on it. The display tests assert on the framebuffer rather
 * than on what reaches the wire, so these only need to exist and to answer
 * plausibly — ssd1306_init checks the result of every transaction.
 */

hw_i2c_result_t hw_i2c_init(uint32_t sda_pin, uint32_t scl_pin, uint32_t bus_hz)
{
    (void)sda_pin;
    (void)scl_pin;
    (void)bus_hz;
    return HW_I2C_OK;
}

hw_i2c_result_t hw_i2c_write(uint8_t address, const uint8_t *data, size_t len)
{
    (void)address;
    (void)data;
    (void)len;
    return HW_I2C_OK;
}

hw_i2c_result_t hw_i2c_probe(uint8_t address)
{
    (void)address;
    return HW_I2C_OK;
}

const char *hw_i2c_result_name(hw_i2c_result_t result)
{
    (void)result;
    return "stub";
}
