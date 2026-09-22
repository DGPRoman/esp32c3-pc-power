/**
 * @file power_machine.h
 * @brief What the PC's power state is, and what the button line should be doing.
 *
 * The whole of the decision-making, with no hardware in it. It is handed the level
 * on the power-LED sense line and the current time, it is asked for actions, and it
 * answers two questions: what state the machine is in, and whether the button line
 * should be asserted right now. Driving a pin and reading one is ::power's job.
 *
 * Split that way because this is the part worth testing, and it is the part a host
 * can run. The alternative — a state machine tangled into GPIO reads and timer
 * callbacks — can only be exercised on a desk with a PC attached to it, which in
 * practice means it is exercised once.
 *
 * ### The physical situation this models
 *
 * A front-panel power button is a momentary switch across two header pins, and the
 * motherboard reads its duration:
 *
 * - a short press is a power-on when the machine is off, and a request to the
 *   running OS to shut down when it is on. The OS may take a minute over it, or
 *   ignore it entirely and put a dialog on a screen nobody is looking at.
 * - a hold of more than about four seconds is handled below the OS: the supply is
 *   cut, with whatever was in flight lost. It is the remedy for a machine that has
 *   stopped answering, and it is never what anyone wants by accident.
 *
 * The power LED is the only honest report of the result, which is why this reads it
 * rather than remembering what it asked for.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What the machine is doing, as far as the LED and the clock can say. */
typedef enum {
    /** Nothing has been read from the sense line yet — the state at boot. */
    POWER_UNKNOWN = 0,
    POWER_OFF,
    POWER_ON,
    /** A press was issued while off; the LED has not come on yet. */
    POWER_TURNING_ON,
    /** A press or a hold was issued while on; the LED has not gone out yet. */
    POWER_TURNING_OFF,
} power_state_t;

/** @brief The two things that can be asked of the button line. */
typedef enum {
    POWER_REQUEST_NONE = 0,
    /** A tap. Power-on when off, a shutdown request to the OS when on. */
    POWER_REQUEST_PRESS,
    /** The hardware cut-off. Never issued to satisfy a press. */
    POWER_REQUEST_HOLD,
} power_request_t;

/** @brief Durations, all in milliseconds. */
typedef struct {
    /**
     * @brief How long the sense line must hold a level before it is believed.
     *
     * The line is a resistive divider off a LED that some machines drive with PWM
     * and some pulse in standby, so a single sample says very little.
     */
    uint32_t debounce_ms;
    /** @brief Width of a ::POWER_REQUEST_PRESS pulse. Well under the four seconds
     * a motherboard reads as a hold. */
    uint32_t press_ms;
    /** @brief Width of a ::POWER_REQUEST_HOLD pulse. Comfortably over four seconds. */
    uint32_t hold_ms;
    /** @brief How long after the pulse ends to keep expecting the LED to come on. */
    uint32_t turn_on_ms;
    /** @brief The same going the other way, which an OS shutdown makes far longer. */
    uint32_t turn_off_ms;
} power_config_t;

/** @brief All of the state. Owned by the caller; nothing here allocates. */
typedef struct {
    power_config_t config;

    power_state_t state;
    uint64_t state_since_ms;

    /** Last raw level sampled, and when it last differed from the one before. */
    bool sense_raw;
    uint64_t sense_changed_ms;
    /** The debounced level, valid only once @c sense_settled is set. */
    bool sense_level;
    bool sense_settled;

    /** The request being served, cleared when the state settles. */
    power_request_t pending;

    /** Whether the button line is asserted, and until when. */
    bool pulsing;
    uint64_t pulse_until_ms;

    /** When a transition gives up and reverts to what the LED says. */
    uint64_t deadline_ms;
} power_machine_t;

/**
 * @brief Start in ::POWER_UNKNOWN with @p config, believing nothing.
 *
 * @p config is copied. @p now_ms is the origin for every duration that follows; it
 * need only be monotonic, not absolute.
 */
void power_machine_init(power_machine_t *machine, const power_config_t *config,
                        uint64_t now_ms);

/**
 * @brief Advance to @p now_ms, having read @p sense_level on the sense line.
 *
 * Expected to be called steadily — the debounce and both timeouts are measured in
 * calls to this. A gap in the calls delays a transition; it does not corrupt one,
 * because every deadline is compared against @p now_ms rather than counted.
 *
 * @param sense_level Raw level of the sense line, true meaning the LED is lit.
 */
void power_machine_advance(power_machine_t *machine, bool sense_level, uint64_t now_ms);

/**
 * @brief Ask for @p what, and say whether it was taken up.
 *
 * Refused, with nothing changed, when the button line is already asserted, when a
 * transition is already running, or while the state is still ::POWER_UNKNOWN. That
 * refusal is the point rather than a limitation: a client that retries a request it
 * never saw answered must not press the button a second time, and a caller that
 * cannot see the machine's state cannot know what a press would do to it.
 *
 * ::POWER_REQUEST_HOLD is never reached by retrying ::POWER_REQUEST_PRESS. The two
 * are separate requests because cutting the power is a separate decision.
 */
bool power_machine_request(power_machine_t *machine, power_request_t what,
                           uint64_t now_ms);

/**
 * @brief Whether the button line should be asserted at the moment last advanced to.
 *
 * Independent of the state, deliberately. A hold that has already cut the power
 * still has a second or two left to run, and releasing the line early because the
 * LED went out would be a shorter press than was asked for.
 */
bool power_machine_button(const power_machine_t *machine);

/** @brief The state as last advanced to. */
power_state_t power_machine_state(const power_machine_t *machine);

/** @brief When the current state was entered, on the caller's clock. */
uint64_t power_machine_state_since(const power_machine_t *machine);

/** @brief The request being served, or ::POWER_REQUEST_NONE. */
power_request_t power_machine_pending(const power_machine_t *machine);

/** @brief A stable lowercase name for @p state, for logs and for JSON. */
const char *power_state_name(power_state_t state);

/** @brief A stable lowercase name for @p request; ::POWER_REQUEST_NONE is "none". */
const char *power_request_name(power_request_t request);

#ifdef __cplusplus
}
#endif
