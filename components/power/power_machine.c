#include "power_machine.h"

#include <string.h>

/**
 * @brief Move to @p state, remembering when.
 *
 * Re-entering the state already held is not a change and does not restart the
 * clock on it, so a poller asking how long the machine has been on gets an answer
 * about the machine rather than about how often this is called.
 */
static void enter(power_machine_t *machine, power_state_t state, uint64_t now_ms)
{
    if (machine->state == state) {
        return;
    }
    machine->state = state;
    machine->state_since_ms = now_ms;
}

/**
 * @brief Arrive at a resting state: the request is finished, one way or the other.
 *
 * The pulse is not touched. It ends on its own clock — see ::power_machine_button.
 */
static void settle(power_machine_t *machine, power_state_t state, uint64_t now_ms)
{
    machine->pending = POWER_REQUEST_NONE;
    machine->deadline_ms = 0;
    enter(machine, state, now_ms);
}

/**
 * @brief Feed one raw sample into the debounce.
 *
 * A level has to survive @c debounce_ms unchanged before it is believed. Written as
 * "when did this level start" rather than as a counter, so that a caller which
 * samples irregularly — or misses a few while the radio is busy — measures the same
 * interval as one that does not.
 */
static void settle_sense(power_machine_t *machine, bool level, uint64_t now_ms)
{
    if (level != machine->sense_raw) {
        machine->sense_raw = level;
        machine->sense_changed_ms = now_ms;
        return;
    }

    if (now_ms - machine->sense_changed_ms >= machine->config.debounce_ms) {
        machine->sense_level = level;
        machine->sense_settled = true;
    }
}

void power_machine_init(power_machine_t *machine, const power_config_t *config,
                        uint64_t now_ms)
{
    memset(machine, 0, sizeof(*machine));
    machine->config = *config;
    machine->state = POWER_UNKNOWN;
    machine->state_since_ms = now_ms;
    /* The first sample will differ from this half the time, which only costs one
     * debounce interval; assuming either level would be a guess about a machine
     * that has not been looked at yet. */
    machine->sense_raw = false;
    machine->sense_changed_ms = now_ms;
}

void power_machine_advance(power_machine_t *machine, bool sense_level, uint64_t now_ms)
{
    settle_sense(machine, sense_level, now_ms);

    if (machine->pulsing && now_ms >= machine->pulse_until_ms) {
        machine->pulsing = false;
    }

    if (!machine->sense_settled) {
        return;
    }

    switch (machine->state) {
    case POWER_UNKNOWN:
        enter(machine, machine->sense_level ? POWER_ON : POWER_OFF, now_ms);
        break;

    case POWER_OFF:
        /* Not necessarily anything this device did. Somebody can press the case
         * button, and the LED is how that is found out. */
        if (machine->sense_level) {
            enter(machine, POWER_ON, now_ms);
        }
        break;

    case POWER_ON:
        if (!machine->sense_level) {
            enter(machine, POWER_OFF, now_ms);
        }
        break;

    case POWER_TURNING_ON:
        if (machine->sense_level) {
            settle(machine, POWER_ON, now_ms);
        } else if (now_ms >= machine->deadline_ms) {
            /* The press did not take. Reporting off is not a failure being hidden:
             * off is what the machine is, and the caller can ask again. */
            settle(machine, POWER_OFF, now_ms);
        }
        break;

    case POWER_TURNING_OFF:
        if (!machine->sense_level) {
            settle(machine, POWER_OFF, now_ms);
        } else if (now_ms >= machine->deadline_ms) {
            /* An OS that ignored the request, or asked somebody a question about
             * unsaved work. Still on, and a hold is the next step — but that is a
             * decision for whoever is asking, not for this. */
            settle(machine, POWER_ON, now_ms);
        }
        break;
    }
}

bool power_machine_request(power_machine_t *machine, power_request_t what,
                           uint64_t now_ms)
{
    if (what != POWER_REQUEST_PRESS && what != POWER_REQUEST_HOLD) {
        return false;
    }

    /* The line is still down from the last request. Asserting it again would run
     * the two pulses together into one longer one — which, for a press repeated
     * because a client never saw its answer, is how a retry becomes a cut-off. */
    if (machine->pulsing) {
        return false;
    }

    /* Only from a resting state. POWER_UNKNOWN is refused because a press means
     * opposite things on and off, and TURNING_* is refused because that request is
     * still being answered. */
    if (machine->state != POWER_OFF && machine->state != POWER_ON) {
        return false;
    }

    const uint32_t width =
        (what == POWER_REQUEST_HOLD) ? machine->config.hold_ms : machine->config.press_ms;

    machine->pending = what;
    machine->pulsing = true;
    machine->pulse_until_ms = now_ms + width;

    if (machine->state == POWER_OFF) {
        /* A hold on a machine that is already off is not a cut-off — there is
         * nothing to cut — and a motherboard reads it as an ordinary press. */
        enter(machine, POWER_TURNING_ON, now_ms);
        machine->deadline_ms = now_ms + width + machine->config.turn_on_ms;
    } else {
        enter(machine, POWER_TURNING_OFF, now_ms);
        machine->deadline_ms = now_ms + width + machine->config.turn_off_ms;
    }

    return true;
}

bool power_machine_button(const power_machine_t *machine)
{
    return machine->pulsing;
}

power_state_t power_machine_state(const power_machine_t *machine)
{
    return machine->state;
}

uint64_t power_machine_state_since(const power_machine_t *machine)
{
    return machine->state_since_ms;
}

power_request_t power_machine_pending(const power_machine_t *machine)
{
    return machine->pending;
}

const char *power_state_name(power_state_t state)
{
    switch (state) {
    case POWER_OFF:          return "off";
    case POWER_ON:           return "on";
    case POWER_TURNING_ON:   return "turning_on";
    case POWER_TURNING_OFF:  return "turning_off";
    case POWER_UNKNOWN:      break;
    }
    return "unknown";
}

const char *power_request_name(power_request_t request)
{
    switch (request) {
    case POWER_REQUEST_PRESS: return "press";
    case POWER_REQUEST_HOLD:  return "hold";
    case POWER_REQUEST_NONE:  break;
    }
    return "none";
}
