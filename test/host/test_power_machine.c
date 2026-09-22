/**
 * @file test_power_machine.c
 * @brief The power state machine, driven a millisecond at a time.
 *
 * Every case here is a scenario played out against a fake clock and a fake sense
 * line: advance time, say what the LED is doing, and check what the machine
 * concluded and what it did to the button. The real thing has a PC on the other
 * end of it, which is why the interesting cases — a shutdown the OS ignores, a
 * client retrying a request it never saw answered — are only ever going to be
 * exercised here.
 */

#include "check.h"
#include "power_machine.h"

/* Round numbers, so that a failure message reads as a time rather than as a sum. */
#define DEBOUNCE_MS 50u
#define PRESS_MS 200u
#define HOLD_MS 6000u
#define TURN_ON_MS 10000u
#define TURN_OFF_MS 60000u

static const power_config_t CONFIG = {
    .debounce_ms = DEBOUNCE_MS,
    .press_ms = PRESS_MS,
    .hold_ms = HOLD_MS,
    .turn_on_ms = TURN_ON_MS,
    .turn_off_ms = TURN_OFF_MS,
};

/** @brief A clock and a sense line, so a test reads as a sequence of moments. */
typedef struct {
    power_machine_t machine;
    uint64_t now_ms;
    bool led;
} rig_t;

static void rig_init(rig_t *rig, uint64_t start_ms)
{
    rig->now_ms = start_ms;
    rig->led = false;
    power_machine_init(&rig->machine, &CONFIG, rig->now_ms);
}

/**
 * @brief Run @p duration_ms of time past the machine, one millisecond per step.
 *
 * A step per millisecond rather than one jump, because the difference between a
 * machine that compares deadlines and one that counts calls only shows up when it
 * is called more than once.
 */
static void run(rig_t *rig, uint64_t duration_ms)
{
    for (uint64_t elapsed = 0; elapsed < duration_ms; elapsed++) {
        rig->now_ms++;
        power_machine_advance(&rig->machine, rig->led, rig->now_ms);
    }
}

/** @brief Settle the machine into a known resting state before a test's own steps. */
static void rig_start_at(rig_t *rig, bool led_lit, uint64_t start_ms)
{
    rig_init(rig, start_ms);
    rig->led = led_lit;
    run(rig, DEBOUNCE_MS + 1u);
}

static bool state_is(const rig_t *rig, power_state_t state)
{
    return power_machine_state(&rig->machine) == state;
}

static void test_the_state_before_anything_has_been_read(void)
{
    rig_t rig;
    rig_init(&rig, 0);

    CHECK(state_is(&rig, POWER_UNKNOWN));

    /* One sample is not an answer. The level has to hold for the debounce. */
    rig.led = true;
    run(&rig, DEBOUNCE_MS - 1u);
    CHECK(state_is(&rig, POWER_UNKNOWN));

    run(&rig, 2);
    CHECK(state_is(&rig, POWER_ON));
}

static void test_a_line_that_will_not_settle_is_never_believed(void)
{
    rig_t rig;
    rig_init(&rig, 0);

    /* Flipping faster than the debounce: a real possibility, because some machines
     * drive the power LED with PWM and some pulse it in standby. */
    for (unsigned cycle = 0; cycle < 40u; cycle++) {
        rig.led = !rig.led;
        run(&rig, DEBOUNCE_MS - 1u);
    }

    CHECK(state_is(&rig, POWER_UNKNOWN));
}

static void test_the_machine_reads_off_and_on_from_the_led(void)
{
    rig_t rig;
    rig_start_at(&rig, false, 1000);
    CHECK(state_is(&rig, POWER_OFF));

    /* Somebody pressed the case button. Nothing asked this device for anything. */
    rig.led = true;
    run(&rig, DEBOUNCE_MS + 1u);
    CHECK(state_is(&rig, POWER_ON));
    CHECK_EQ(power_machine_pending(&rig.machine), POWER_REQUEST_NONE);

    rig.led = false;
    run(&rig, DEBOUNCE_MS + 1u);
    CHECK(state_is(&rig, POWER_OFF));
}

static void test_a_press_while_off_turns_the_machine_on(void)
{
    rig_t rig;
    rig_start_at(&rig, false, 0);

    CHECK(power_machine_request(&rig.machine, POWER_REQUEST_PRESS, rig.now_ms));
    CHECK(state_is(&rig, POWER_TURNING_ON));
    CHECK_EQ(power_machine_pending(&rig.machine), POWER_REQUEST_PRESS);
    CHECK(power_machine_button(&rig.machine));

    /* The line goes back up after exactly the press width and not before. */
    run(&rig, PRESS_MS - 1u);
    CHECK(power_machine_button(&rig.machine));
    run(&rig, 1);
    CHECK(!power_machine_button(&rig.machine));

    /* Still turning on: the LED has not come up yet. */
    CHECK(state_is(&rig, POWER_TURNING_ON));

    rig.led = true;
    run(&rig, DEBOUNCE_MS + 1u);
    CHECK(state_is(&rig, POWER_ON));
    CHECK_EQ(power_machine_pending(&rig.machine), POWER_REQUEST_NONE);
}

static void test_a_press_that_does_not_take_reverts_to_off(void)
{
    rig_t rig;
    rig_start_at(&rig, false, 0);

    CHECK(power_machine_request(&rig.machine, POWER_REQUEST_PRESS, rig.now_ms));

    run(&rig, PRESS_MS + TURN_ON_MS - 1u);
    CHECK(state_is(&rig, POWER_TURNING_ON));

    run(&rig, 2);
    CHECK(state_is(&rig, POWER_OFF));
    CHECK_EQ(power_machine_pending(&rig.machine), POWER_REQUEST_NONE);

    /* And the machine is usable again rather than stuck. */
    CHECK(power_machine_request(&rig.machine, POWER_REQUEST_PRESS, rig.now_ms));
}

static void test_a_press_while_on_asks_the_os_and_waits(void)
{
    rig_t rig;
    rig_start_at(&rig, true, 0);
    CHECK(state_is(&rig, POWER_ON));

    CHECK(power_machine_request(&rig.machine, POWER_REQUEST_PRESS, rig.now_ms));
    CHECK(state_is(&rig, POWER_TURNING_OFF));

    /* An operating system takes its time over this, and the state says so for as
     * long as it does. */
    run(&rig, 30000);
    CHECK(state_is(&rig, POWER_TURNING_OFF));

    rig.led = false;
    run(&rig, DEBOUNCE_MS + 1u);
    CHECK(state_is(&rig, POWER_OFF));
}

static void test_a_shutdown_the_os_ignores_goes_back_to_on(void)
{
    rig_t rig;
    rig_start_at(&rig, true, 0);

    CHECK(power_machine_request(&rig.machine, POWER_REQUEST_PRESS, rig.now_ms));

    run(&rig, PRESS_MS + TURN_OFF_MS - 1u);
    CHECK(state_is(&rig, POWER_TURNING_OFF));

    run(&rig, 2);
    CHECK(state_is(&rig, POWER_ON));
    CHECK_EQ(power_machine_pending(&rig.machine), POWER_REQUEST_NONE);
}

static void test_a_hold_is_a_longer_pulse(void)
{
    rig_t rig;
    rig_start_at(&rig, true, 0);

    CHECK(power_machine_request(&rig.machine, POWER_REQUEST_HOLD, rig.now_ms));
    CHECK_EQ(power_machine_pending(&rig.machine), POWER_REQUEST_HOLD);

    /* Past the width of a press, and still holding — this is the whole difference
     * between the two requests as far as the motherboard is concerned. */
    run(&rig, PRESS_MS + 1u);
    CHECK(power_machine_button(&rig.machine));

    run(&rig, HOLD_MS - PRESS_MS - 2u);
    CHECK(power_machine_button(&rig.machine));
    run(&rig, 2);
    CHECK(!power_machine_button(&rig.machine));
}

static void test_a_hold_keeps_holding_after_the_power_is_cut(void)
{
    rig_t rig;
    rig_start_at(&rig, true, 0);

    CHECK(power_machine_request(&rig.machine, POWER_REQUEST_HOLD, rig.now_ms));

    /* The supply is cut partway through the hold, which is what a cut-off is. */
    run(&rig, 4200);
    rig.led = false;
    run(&rig, DEBOUNCE_MS + 1u);
    CHECK(state_is(&rig, POWER_OFF));

    /* Releasing the line here would have been a four-second press, not a hold.
     * The button runs on its own clock for exactly this reason. */
    CHECK(power_machine_button(&rig.machine));

    run(&rig, HOLD_MS);
    CHECK(!power_machine_button(&rig.machine));
}

static void test_no_request_is_taken_while_the_line_is_still_down(void)
{
    rig_t rig;
    rig_start_at(&rig, true, 0);

    CHECK(power_machine_request(&rig.machine, POWER_REQUEST_HOLD, rig.now_ms));

    /* The supply is cut partway through the hold, so the state settles to off with
     * the button still held. This is the one moment where the state on its own
     * would say a request is fine and it is not: taken up, it would start a second
     * pulse from here and run the two into one far longer hold. */
    run(&rig, 4200);
    rig.led = false;
    run(&rig, DEBOUNCE_MS + 1u);
    CHECK(state_is(&rig, POWER_OFF));
    CHECK(power_machine_button(&rig.machine));

    CHECK(!power_machine_request(&rig.machine, POWER_REQUEST_PRESS, rig.now_ms));
    CHECK(!power_machine_request(&rig.machine, POWER_REQUEST_HOLD, rig.now_ms));

    /* Once the line is back up it is an ordinary machine that happens to be off. */
    run(&rig, HOLD_MS);
    CHECK(!power_machine_button(&rig.machine));
    CHECK(power_machine_request(&rig.machine, POWER_REQUEST_PRESS, rig.now_ms));
}

static void test_a_retried_press_does_not_press_twice(void)
{
    rig_t rig;
    rig_start_at(&rig, false, 0);

    CHECK(power_machine_request(&rig.machine, POWER_REQUEST_PRESS, rig.now_ms));

    /* A client that never saw the first answer, asking again. Every one of these
     * has to be refused: taken up, they would run into one long pulse. */
    for (unsigned attempt = 0; attempt < 20u; attempt++) {
        run(&rig, 5);
        CHECK(!power_machine_request(&rig.machine, POWER_REQUEST_PRESS, rig.now_ms));
    }

    /* Which means the line still goes up on the original schedule. */
    CHECK(power_machine_button(&rig.machine));
    run(&rig, PRESS_MS - 100u);
    CHECK(!power_machine_button(&rig.machine));
}

static void test_a_retried_press_never_becomes_a_hold(void)
{
    rig_t rig;
    rig_start_at(&rig, true, 0);

    CHECK(power_machine_request(&rig.machine, POWER_REQUEST_PRESS, rig.now_ms));

    /* Retried past the four seconds a motherboard reads as a cut-off. The line has
     * to be up long before then, whatever the client does. */
    uint64_t held_ms = 0;
    for (unsigned attempt = 0; attempt < 100u; attempt++) {
        run(&rig, 100);
        (void)power_machine_request(&rig.machine, POWER_REQUEST_PRESS, rig.now_ms);
        if (power_machine_button(&rig.machine)) {
            held_ms += 100u;
        }
    }

    CHECK(held_ms <= PRESS_MS);
    CHECK(!power_machine_button(&rig.machine));
}

static void test_nothing_is_asked_of_a_machine_that_has_not_been_read(void)
{
    rig_t rig;
    rig_init(&rig, 0);

    /* A press means opposite things on and off, so there is no answer to give
     * before the sense line has settled. */
    CHECK(!power_machine_request(&rig.machine, POWER_REQUEST_PRESS, rig.now_ms));
    CHECK(!power_machine_request(&rig.machine, POWER_REQUEST_HOLD, rig.now_ms));
    CHECK(!power_machine_button(&rig.machine));

    rig.led = true;
    run(&rig, DEBOUNCE_MS + 1u);
    CHECK(power_machine_request(&rig.machine, POWER_REQUEST_PRESS, rig.now_ms));
}

static void test_a_request_during_a_transition_is_refused(void)
{
    rig_t rig;
    rig_start_at(&rig, true, 0);

    CHECK(power_machine_request(&rig.machine, POWER_REQUEST_PRESS, rig.now_ms));

    /* Past the pulse, so the line is up — but the OS is still being waited on, and
     * a second press would be a second shutdown request to an OS already handling
     * one. */
    run(&rig, PRESS_MS + 1000u);
    CHECK(!power_machine_button(&rig.machine));
    CHECK(state_is(&rig, POWER_TURNING_OFF));
    CHECK(!power_machine_request(&rig.machine, POWER_REQUEST_PRESS, rig.now_ms));
    CHECK(!power_machine_request(&rig.machine, POWER_REQUEST_HOLD, rig.now_ms));
}

static void test_a_hold_while_off_is_an_ordinary_power_on(void)
{
    rig_t rig;
    rig_start_at(&rig, false, 0);

    /* Nothing to cut off. A motherboard reads the pulse as a press either way, so
     * the state has to say turning_on rather than turning_off. */
    CHECK(power_machine_request(&rig.machine, POWER_REQUEST_HOLD, rig.now_ms));
    CHECK(state_is(&rig, POWER_TURNING_ON));
}

static void test_the_state_clock_measures_the_machine_not_the_caller(void)
{
    rig_t rig;
    rig_start_at(&rig, true, 7000);

    const uint64_t entered = power_machine_state_since(&rig.machine);
    CHECK(state_is(&rig, POWER_ON));

    /* Thousands of advances that change nothing must not restart it. */
    run(&rig, 5000);
    CHECK_EQ(power_machine_state_since(&rig.machine), entered);

    rig.led = false;
    run(&rig, DEBOUNCE_MS + 1u);
    CHECK(power_machine_state_since(&rig.machine) > entered);
}

static void test_an_irregular_caller_measures_the_same_intervals(void)
{
    /* The sampling task shares a core with the radio, so the calls do not arrive
     * evenly. Deadlines are compared against the clock rather than counted for
     * exactly this reason, and this is the assertion that says so. */
    rig_t rig;
    rig_start_at(&rig, false, 0);

    CHECK(power_machine_request(&rig.machine, POWER_REQUEST_PRESS, rig.now_ms));

    /* One call, a whole press-width later. */
    rig.now_ms += PRESS_MS;
    power_machine_advance(&rig.machine, rig.led, rig.now_ms);
    CHECK(!power_machine_button(&rig.machine));

    /* And one more, long enough that the transition has to have given up. */
    rig.now_ms += TURN_ON_MS;
    power_machine_advance(&rig.machine, rig.led, rig.now_ms);
    CHECK(state_is(&rig, POWER_OFF));
}

static void test_the_names_are_the_ones_the_api_publishes(void)
{
    CHECK_EQ_STR(power_state_name(POWER_UNKNOWN), "unknown");
    CHECK_EQ_STR(power_state_name(POWER_OFF), "off");
    CHECK_EQ_STR(power_state_name(POWER_ON), "on");
    CHECK_EQ_STR(power_state_name(POWER_TURNING_ON), "turning_on");
    CHECK_EQ_STR(power_state_name(POWER_TURNING_OFF), "turning_off");

    CHECK_EQ_STR(power_request_name(POWER_REQUEST_NONE), "none");
    CHECK_EQ_STR(power_request_name(POWER_REQUEST_PRESS), "press");
    CHECK_EQ_STR(power_request_name(POWER_REQUEST_HOLD), "hold");

    /* A value from outside the enumeration, which C permits and a future field
     * could carry: named rather than left to read whatever is past the table. */
    CHECK_EQ_STR(power_state_name((power_state_t)99), "unknown");
    CHECK_EQ_STR(power_request_name((power_request_t)99), "none");
}

void test_power_machine(void);

void test_power_machine(void)
{
    check_begin("power state machine");

    test_the_state_before_anything_has_been_read();
    test_a_line_that_will_not_settle_is_never_believed();
    test_the_machine_reads_off_and_on_from_the_led();
    test_a_press_while_off_turns_the_machine_on();
    test_a_press_that_does_not_take_reverts_to_off();
    test_a_press_while_on_asks_the_os_and_waits();
    test_a_shutdown_the_os_ignores_goes_back_to_on();
    test_a_hold_is_a_longer_pulse();
    test_a_hold_keeps_holding_after_the_power_is_cut();
    test_no_request_is_taken_while_the_line_is_still_down();
    test_a_retried_press_does_not_press_twice();
    test_a_retried_press_never_becomes_a_hold();
    test_nothing_is_asked_of_a_machine_that_has_not_been_read();
    test_a_request_during_a_transition_is_refused();
    test_a_hold_while_off_is_an_ordinary_power_on();
    test_the_state_clock_measures_the_machine_not_the_caller();
    test_an_irregular_caller_measures_the_same_intervals();
    test_the_names_are_the_ones_the_api_publishes();
}
