/**
 * @file test_hw_i2c_timing.c
 * @brief The bounds that used to be asserts in the I²C driver.
 *
 * Asserting the exact counters for 100 kHz would restate the arithmetic, so what is
 * held here is what the header promises: the frequencies it accepts produce counters
 * the hardware's own ordering allows, and the ones it cannot express are refused
 * rather than answered. The refusals are the point — each was an `assert` before,
 * which is to say each was nothing at all in a build with NDEBUG set.
 *
 * Run under UBSan, which is what makes the zero case worth writing down: with the
 * refusal removed it reports the division, rather than leaving a test to notice that
 * the counters it got back were nonsense. It does not reach the second half of that
 * zero — __builtin_clz of it is undefined too, and gcc's sanitiser does not
 * instrument the builtins — so that one is refused on the language's word rather
 * than on a demonstration.
 */

#include "check.h"

#include "hw_i2c_timing.h"

/** @brief The ESP32-C3's crystal, which is what the driver derives from. */
#define XTAL_HZ 40000000u

void test_hw_i2c_timing(void);

/** @brief Every invariant that has to hold for a frequency the driver accepts. */
static void check_usable(uint32_t bus_hz, const hw_i2c_timing_t *timing)
{
    /* The floor the whole derivation stands on. Below it two of the counters
     * underflow and the timeout exponent is taken of zero. */
    CHECK(timing->half >= HW_I2C_MIN_HALF_PERIOD);

    /* An ordering the hardware assumes: stop looking for clock stretching before
     * sampling SDA, and sample before the pulse ends. */
    CHECK(timing->wait_high < timing->sda_sample);
    CHECK(timing->sda_sample < timing->high);

    /* Written to their registers one less than they are, so zero is not a value
     * either may hold. */
    CHECK(timing->sda_hold >= 1u);
    CHECK(timing->sda_sample >= 1u);
    CHECK(timing->clkm_div >= 1u);

    CHECK(timing->timeout <= HW_I2C_TIMEOUT_VALUE_MAX);

    /*
     * The counters are what the bus speed is, so they have to describe the frequency
     * that was asked for. Stated exactly rather than within a tolerance: the
     * half-period is whole source-clock ticks, so it is the largest count that still
     * fits inside one, which pins it between two bounds with nothing to argue about.
     *
     * The consequence is worth naming, since it is the direction the rounding goes:
     * a bus runs at or slightly above the requested frequency, never below it. At
     * the top of the range, where the half-period is eight ticks, "slightly" is an
     * eighth.
     */
    const uint64_t sclk_hz = XTAL_HZ / timing->clkm_div;
    CHECK(2u * (uint64_t)timing->half * bus_hz <= sclk_hz);
    CHECK(sclk_hz < 2u * ((uint64_t)timing->half + 1u) * bus_hz);
}

void test_hw_i2c_timing(void)
{
    check_begin("hw_i2c_timing");

    hw_i2c_timing_t timing;

    /* The frequency this board actually runs, and the two other speeds the bus
     * standard names. Each has to be expressible, or the driver has regressed into
     * refusing something it has been driving for months. */
    static const uint32_t usable[] = { 100000u, 400000u, 1000000u };
    for (unsigned i = 0; i < sizeof(usable) / sizeof(usable[0]); i++) {
        CHECK(hw_i2c_timing_derive(XTAL_HZ, usable[i], &timing));
        check_usable(usable[i], &timing);
    }

    /* Zero. The divisor twice over, and the reason this is a check rather than an
     * assert: with NDEBUG the driver divided by it and then took __builtin_clz of
     * zero, which is undefined — so a release build's timing came from whatever the
     * instruction happened to leave behind. */
    CHECK(!hw_i2c_timing_derive(XTAL_HZ, 0u, &timing));

    /* A source clock of zero is the same division, reached the other way. */
    CHECK(!hw_i2c_timing_derive(0u, 100000u, &timing));

    /* Faster than the source can be divided into. 40 MHz over eight ticks a half
     * period is 2.5 MHz, so this is the first frequency past the floor. */
    CHECK(!hw_i2c_timing_derive(XTAL_HZ, XTAL_HZ / (2u * HW_I2C_MIN_HALF_PERIOD) + 1u, &timing));
    CHECK(hw_i2c_timing_derive(XTAL_HZ, XTAL_HZ / (2u * HW_I2C_MIN_HALF_PERIOD), &timing));

    /* Absurdly fast. The intermediate here is bus_hz × 1024, which wraps a uint32_t
     * above 4.19 MHz — and a wrapped divisor yields a divider that looks plausible
     * rather than one that is obviously wrong, so the refusal must not depend on it. */
    CHECK(!hw_i2c_timing_derive(XTAL_HZ, 0xFFFFFFFFu, &timing));
    CHECK(!hw_i2c_timing_derive(XTAL_HZ, 5000000u, &timing));

    /* A refusal leaves the caller's struct alone rather than half filled in. */
    hw_i2c_timing_t untouched = { 0 };
    untouched.half = 0xA5A5A5A5u;
    CHECK(!hw_i2c_timing_derive(XTAL_HZ, 0u, &untouched));
    CHECK_EQ(untouched.half, 0xA5A5A5A5u);

    /* A null destination is refused rather than written through. */
    CHECK(!hw_i2c_timing_derive(XTAL_HZ, 100000u, NULL));

    /* Every frequency between the floor and the bottom of the range, at a step small
     * enough to land on the awkward ones. A derivation that succeeds has to be
     * usable: the two orderings above are not free properties, they are the ones the
     * assert pair used to claim without ever being compiled. */
    for (uint32_t bus_hz = 1000u; bus_hz <= 2500000u; bus_hz += 1000u) {
        if (hw_i2c_timing_derive(XTAL_HZ, bus_hz, &timing)) {
            check_usable(bus_hz, &timing);
        }
    }
}
