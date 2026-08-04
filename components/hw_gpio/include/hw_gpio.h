/**
 * @file hw_gpio.h
 * @brief GPIO output driver for the ESP32-C3, written against its register map.
 *
 * Scope is deliberately narrow — the pins this firmware drives and nothing else.
 * Inputs, pull configuration and interrupts arrive with the code that needs them
 * rather than being written speculatively.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Highest GPIO number the ESP32-C3 exposes.
 *
 * The chip has 22 of them, GPIO0 through GPIO21. GPIO11..GPIO17 are wired to the
 * module's SPI flash on any board that carries one, which makes them numerically
 * valid and physically unusable — a distinction this driver cannot see and the
 * board pin map is responsible for.
 */
#define HW_GPIO_MAX 21u

/**
 * @brief Claim @p pin as a push-pull output already driving @p level.
 *
 * The level is latched before the output driver is enabled, so the pin moves
 * straight from high-impedance to the intended state with no glitch in between.
 * That ordering matters for anything on the other end that reacts to edges — a
 * momentary wrong level on a power button line is a keypress.
 *
 * @param pin   GPIO number, 0 to ::HW_GPIO_MAX.
 * @param level Level to drive; true is high.
 */
void hw_gpio_output_init(uint32_t pin, bool level);

/**
 * @brief Drive @p pin, previously claimed by ::hw_gpio_output_init, to @p level.
 *
 * Atomic with respect to every other pin, so an interrupt may drive one pin while
 * task code drives another. There is deliberately no toggle counterpart: reading
 * a pin back to invert it cannot be atomic, so the caller owns the current state.
 */
void hw_gpio_write(uint32_t pin, bool level);

#ifdef __cplusplus
}
#endif
