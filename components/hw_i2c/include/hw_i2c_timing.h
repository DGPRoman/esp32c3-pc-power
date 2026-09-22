/**
 * @file hw_i2c_timing.h
 * @brief Turning a target bus frequency into the counters the controller wants.
 *
 * Split out of hw_i2c.c because it is arithmetic and nothing else — no registers,
 * no ESP-IDF — so it builds and runs on a host and is covered by
 * test/host/test_hw_i2c_timing.c. The driver writes what this computes.
 *
 * It answers rather than asserts. The bounds below were `assert`s inside the
 * driver, which means a release build had no bounds at all: NDEBUG left a
 * frequency nothing can express to produce counter values nothing intended, and
 * one of them — an exponent taken with __builtin_clz — to be undefined behaviour.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief TIME_OUT_VALUE is a five-bit field. */
#define HW_I2C_TIMEOUT_VALUE_MAX 0x1Fu

/**
 * @brief Shortest half-period the counters below can be carved out of.
 *
 * Every counter is a fraction of the half-period: a quarter of it, a half of it,
 * that half less two. Under eight source-clock ticks those fractions collapse into
 * each other and two of them underflow, so this is the floor the whole derivation
 * stands on rather than a comfort margin.
 */
#define HW_I2C_MIN_HALF_PERIOD 8u

/**
 * @brief The counters one bus frequency works out to, in source-clock ticks.
 *
 * Raw, in the sense that the register writes are still the driver's business: the
 * hardware wants some of these one less than the interval they describe and some
 * exactly as they are, and which is which is a fact about the controller that
 * belongs next to the writes.
 */
typedef struct {
    /** @brief Pre-divider for the peripheral clock, as a count rather than a field. */
    uint32_t clkm_div;
    /** @brief Half a bit time. SCL's low period, and the several hold and setup times. */
    uint32_t half;
    /** @brief The clock pulse proper, after the stretch window. */
    uint32_t high;
    /** @brief The part of the high period spent watching for a peripheral stretching. */
    uint32_t wait_high;
    /** @brief How long SDA is held after SCL falls. */
    uint32_t sda_hold;
    /** @brief When SDA is sampled within the high period. */
    uint32_t sda_sample;
    /** @brief Hardware timeout as a power of two, clamped to ::HW_I2C_TIMEOUT_VALUE_MAX. */
    uint32_t timeout;
} hw_i2c_timing_t;

/**
 * @brief Work out the counters for @p bus_hz off a source clock of @p source_hz.
 *
 * @return True when the frequency can be expressed, with @p out filled in. False
 *         leaves @p out untouched: there is no partial answer here, because every
 *         counter is derived from the same half-period and a plausible-looking
 *         subset of them is worse than none.
 */
bool hw_i2c_timing_derive(uint32_t source_hz, uint32_t bus_hz, hw_i2c_timing_t *out);

#ifdef __cplusplus
}
#endif
