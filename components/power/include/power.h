/**
 * @file power.h
 * @brief The pins, the timer and the lock around ::power_machine.
 *
 * Everything in ::power_machine_t is decided by ::power_machine, which knows
 * nothing about hardware. This is the other half: it claims the two pins, samples
 * the sense line on a timer, drives the button line to whatever the machine says,
 * and serialises access so that an HTTP task and the timer can both reach it.
 *
 * Deliberately not an object. There is one front-panel header on one PC, and the
 * caller that would own a handle is the same one that would pass it back.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "power_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What ::power_read answers: the whole of the observable state. */
typedef struct {
    power_state_t state;
    /** The request being served, or ::POWER_REQUEST_NONE. */
    power_request_t pending;
    /** Device uptime, in milliseconds, when @c state was entered. */
    uint64_t observed_at_ms;
    /** Device uptime now, so a reader can work out how old @c state is without
     * sharing a clock with this device. */
    uint64_t uptime_ms;
} power_status_t;

/**
 * @brief How this board is wired to the front-panel header.
 *
 * Pins and polarities only. The durations — how long a press is, how long to wait
 * for an operating system to shut down — are properties of an ATX front panel
 * rather than of this board, so they live in the component and not here.
 */
typedef struct {
    /** Pin driving the optocoupler across the PWR_BTN pins. */
    uint32_t button_gpio;
    /** Level on @c button_gpio that closes the optocoupler. */
    bool button_pressed_level;
    /** Pin reading the divider off the PWR_LED header. */
    uint32_t sense_gpio;
    /** Level on @c sense_gpio while the machine's power LED is lit. */
    bool sense_lit_level;
} power_wiring_t;

/**
 * @brief Claim both pins as @p wiring describes them, and start sampling.
 *
 * The button line is released before the sense line is claimed, so the first thing
 * this does to the header on boot is nothing.
 *
 * @return ESP_OK, or the error from creating or starting the sampling timer.
 */
esp_err_t power_start(const power_wiring_t *wiring);

/** @brief The state as of the last sample. */
power_status_t power_read(void);

/**
 * @brief Ask for @p what, and say whether it was taken up.
 *
 * False means the request was refused with nothing changed — the button line is
 * still down from a previous one, a transition is already running, or the sense
 * line has not settled yet, so nothing here knows what a press would do. See
 * ::power_machine_request, which decides all of it.
 *
 * Safe to call before ::power_start, where it refuses everything.
 */
bool power_request(power_request_t what);

#ifdef __cplusplus
}
#endif
