/**
 * @file hw_gpio.h
 * @brief GPIO output driver for the ESP32-C3, written against its register map.
 *
 * Scope is deliberately narrow — the pins this firmware drives and reads, and
 * nothing else. Interrupts still arrive with the code that needs them rather than
 * being written speculatively; the power-LED sense line is sampled on a timer, so
 * it wanted a level rather than an edge.
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

/**
 * @brief Claim @p pin as a plain input, optionally with the pad's pull-up.
 *
 * The output driver is disabled first, so a pin previously driven is released
 * before anything downstream is asked what level it is at.
 *
 * @param pin     GPIO number, 0 to ::HW_GPIO_MAX.
 * @param pull_up Enable the pad's internal pull-up, roughly 45 kΩ. Enough to give
 *                a floating pin a defined level; not enough to be a bias network.
 *                A line driven by something on the other end wants this off.
 */
void hw_gpio_input_init(uint32_t pin, bool pull_up);

/**
 * @brief Read @p pin, previously claimed by ::hw_gpio_input_init.
 *
 * One sample of a level, with no filtering of any kind. Anything connected to the
 * world outside the board needs that filtering, and it belongs to whoever knows
 * what the signal is supposed to look like.
 */
bool hw_gpio_read(uint32_t pin);

/**
 * @brief Claim @p pin as an open-drain bus line owned by a peripheral.
 *
 * For a shared bus — I²C is the reason this exists — where any device may pull the
 * line low and none may drive it high. The peripheral drives @p out_signal onto the
 * pin, and the pin is routed back into @p in_signal at the same time, because a bus
 * controller has to read the line it drives: that is how it sees an acknowledgement,
 * and how it notices a peripheral holding the clock low to ask for time.
 *
 * The line is released before it is claimed, so bringing a bus up cannot be mistaken
 * by other devices for a start condition.
 *
 * @param pin        GPIO number, 0 to ::HW_GPIO_MAX.
 * @param out_signal Peripheral output signal index that drives the pin.
 * @param in_signal  Peripheral input signal index fed from the pin.
 * @param pull_up    Enable the pad's internal pull-up. At roughly 45 kΩ it is far
 *                   too weak to run a bus on: a board without external pull-ups
 *                   needs resistors fitted, not this flag set.
 */
void hw_gpio_open_drain_init(uint32_t pin, uint32_t out_signal, uint32_t in_signal,
                             bool pull_up);

#ifdef __cplusplus
}
#endif
