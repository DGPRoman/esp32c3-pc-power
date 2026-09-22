/**
 * @file hw_i2c_timing.c
 * @brief Deriving the controller's timing counters. See hw_i2c_timing.h.
 */

#include "hw_i2c_timing.h"

#include <stddef.h>

bool hw_i2c_timing_derive(uint32_t source_hz, uint32_t bus_hz, hw_i2c_timing_t *out)
{
    if (out == NULL || source_hz == 0u || bus_hz == 0u) {
        /* bus_hz is the one that mattered: it is the divisor twice below, and the
         * only thing that had ever stood between a zero and that was an assert in
         * the driver's init. */
        return false;
    }

    /*
     * Pre-divider for the peripheral's own clock. Keeping the divided clock at least
     * 1024× the bus frequency leaves the counters enough resolution to land on the
     * requested speed rather than near it.
     *
     * In 64 bits because the multiplication comes first: bus_hz × 1024 wraps a
     * uint32_t above 4.19 MHz, and a wrapped divisor produces a divider that looks
     * reasonable rather than one that is obviously wrong.
     */
    const uint32_t clkm_div = (uint32_t)(source_hz / ((uint64_t)bus_hz * 1024u)) + 1u;
    const uint32_t sclk_hz = source_hz / clkm_div;
    const uint32_t half = sclk_hz / bus_hz / 2u;

    if (half < HW_I2C_MIN_HALF_PERIOD) {
        /* Asked for more than the source clock can be divided into. Everything below
         * depends on this: wait_high subtracts two from half of it, sda_hold and
         * sda_sample are written one less than they are, and the timeout exponent is
         * a __builtin_clz of five times it — which is undefined at zero, and was
         * reachable in any build with NDEBUG set. */
        return false;
    }

    /* SCL's high time is split in two: part of it is spent watching whether a
     * peripheral is holding the line down to ask for more time, and the rest is the
     * clock pulse proper. */
    const uint32_t wait_high = (bus_hz >= 80000u) ? (half / 2u - 2u) : (half / 4u);
    const uint32_t high = half - wait_high;
    const uint32_t sda_hold = half / 4u;
    const uint32_t sda_sample = half / 2u;

    /* An ordering the hardware assumes: finish looking for clock stretching before
     * sampling SDA, and sample before the pulse ends. It holds for every frequency
     * that clears the floor above, so this is the check that says so rather than one
     * that expects to fire — which is exactly why it must not be an assert, because
     * the build where it would matter is the one that compiles asserts out. */
    if (!(wait_high < sda_sample && sda_sample < high)) {
        return false;
    }

    /*
     * Hardware timeout, expressed as a power of two in source-clock ticks: how long
     * SCL may sit in one state before the controller abandons the transaction. Sized
     * at roughly five half-cycles, so a peripheral that dies mid-byte cannot hold the
     * bus indefinitely, and clamped to the width of the field.
     */
    uint32_t timeout = 32u - (uint32_t)__builtin_clz(5u * half) + 2u;
    if (timeout > HW_I2C_TIMEOUT_VALUE_MAX) {
        timeout = HW_I2C_TIMEOUT_VALUE_MAX;
    }

    out->clkm_div = clkm_div;
    out->half = half;
    out->high = high;
    out->wait_high = wait_high;
    out->sda_hold = sda_hold;
    out->sda_sample = sda_sample;
    out->timeout = timeout;

    return true;
}
