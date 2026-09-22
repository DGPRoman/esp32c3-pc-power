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

/**
 * @brief I²C bus carrying the on-board OLED.
 *
 * These pins are not silkscreened on the board and its seller publishes no
 * schematic. They are what this family of modules uses, and the bus scan at startup
 * is what turns that from an assumption into a fact — a scan finding nothing here is
 * a reason to try other pairs before suspecting the display.
 */
#define BOARD_I2C_SDA_GPIO 5u
#define BOARD_I2C_SCL_GPIO 6u

/**
 * @brief Bus frequency.
 *
 * 100 kHz, not the 400 kHz an SSD1306 will accept. Rise time on an I²C line is set
 * by its pull-up resistors and the capacitance they have to charge, and the
 * resistors fitted to this module are an unknown; a slower bus tolerates a weak
 * pull-up that a faster one would not. Worth revisiting once the display is known
 * to answer, since a display redraw is the one thing here that moves real data.
 */
#define BOARD_I2C_HZ 100000u

/**
 * @brief Optocoupler driving the front-panel PWR_BTN header.
 *
 * @warning Provisional. These two pins have not been confirmed against a board
 * with the header wired to it. They are chosen rather than measured: GPIO3 and
 * GPIO4 are brought out on this module, adjacent on the pad row, and clear of
 * everything already spoken for — GPIO8 is the status LED and a strapping pin,
 * GPIO2 and GPIO9 are the other two strapping pins, GPIO5 and GPIO6 carry the
 * display's I²C, GPIO18 and GPIO19 are the native USB pair that the console and
 * the flasher both arrive on, and GPIO11 through GPIO17 are the module's SPI
 * flash. Bring-up confirms or replaces them, and this file is the only place that
 * has to change.
 *
 * Active high, so that the pin's reset state is the safe one. A pad leaves reset
 * as a high-impedance input, which with the opto's LED pulled down is the button
 * not pressed. Active low would make every reset a keypress.
 */
#define BOARD_POWER_BUTTON_GPIO 3u

/** @brief The level that closes the optocoupler, and so presses the button. */
#define BOARD_POWER_BUTTON_PRESSED_LEVEL true

/**
 * @brief Divider sensing the front-panel PWR_LED header.
 *
 * @warning Provisional in the same way as ::BOARD_POWER_BUTTON_GPIO.
 *
 * No internal pull. The divider defines the level at both ends — the LED anode
 * sits near the supply rail when the machine is running and near ground when it is
 * not — and a 45 kΩ pull-up against a divider would shift both readings by an
 * amount that depends on resistors this file does not know.
 */
#define BOARD_POWER_SENSE_GPIO 4u

/** @brief The level the sense pin reads while the machine's power LED is lit.
 *
 * True for a divider straight off PWR_LED+. A board that inverts the signal —
 * through a transistor, or from PWR_LED− — is one constant away from working. */
#define BOARD_POWER_SENSE_LIT_LEVEL true
