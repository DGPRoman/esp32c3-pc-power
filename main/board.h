/**
 * @file board.h
 * @brief Physical connections of the board this firmware runs on.
 *
 * Every pin number lives here. Drivers take pin numbers as arguments and know
 * nothing about the board, so a revision or a different module variant is a change
 * to this file and nowhere else.
 */

#pragma once

#include <stdbool.h>

/**
 * @brief On-board status LED.
 *
 * Wired active low: the LED sits between 3V3 and the pin, so the pin sinks its
 * current and a low level lights it.
 *
 * GPIO8 is also one of the C3's three strapping pins, with GPIO2 and GPIO9. Their
 * levels are sampled once as the chip leaves reset to select a boot mode, and
 * entering download mode — how the flasher gets in — requires GPIO8 to read high.
 * The LED wiring is compatible with that, because a pad configured as an input
 * offers no path to ground through the LED. Anything added to this pin has to
 * preserve it: a pull-down strong enough to hold GPIO8 low at reset leaves the
 * board unable to enter download mode without the BOOT button.
 */
#define BOARD_STATUS_LED_GPIO 8u

/** @brief The level that lights ::BOARD_STATUS_LED_GPIO. */
#define BOARD_STATUS_LED_LIT_LEVEL false
