#include "hw_i2c.h"

#include <assert.h>
#include <string.h>

#include "esp_rom_sys.h"
#include "hw_gpio.h"
#include "hw_reg.h"
#include "soc/gpio_sig_map.h"
#include "soc/i2c_reg.h"
#include "soc/soc.h"
#include "soc/system_reg.h"

/** @brief The C3 has exactly one I²C controller, and its register macros are indexed. */
#define I2C0 0u

/** @brief One of the sequencer's eight command slots. */
#define CMD_REG(n) (I2C_COMD0_REG(I2C0) + ((n) * 4u))

/*
 * Field positions inside a command word.
 *
 * The opcodes deserve a second look: they are neither sequential nor ordered the way
 * a transaction uses them, so the obvious guess — start at 0 and count up in
 * transaction order — gets three of the five wrong, and the failure is a bus that
 * does something almost right.
 */
enum {
    CMD_BYTE_NUM_S = 0,
    CMD_ACK_EN_S = 8,
    CMD_ACK_EXP_S = 9,
    CMD_ACK_VAL_S = 10,
    CMD_OP_CODE_S = 11,
};

enum {
    OP_WRITE = 1,
    OP_STOP = 2,
    OP_READ = 3,
    OP_END = 4,
    OP_RESTART = 6,
};

/** @brief TIME_OUT_VALUE is a five-bit field. */
#define TIMEOUT_VALUE_MAX 0x1Fu

/*
 * One controller means one set of settings. The bus frequency is kept because the
 * software completion deadline has to scale with it — the same transaction takes
 * four times as long at 100 kHz as at 400 kHz, and a fixed timeout would either be
 * uselessly loose or wrong.
 */
static uint32_t s_bus_hz;

/**
 * @brief Derive the controller's timing counters from a target bus frequency.
 *
 * The hardware has no concept of a frequency. It has counters measured in
 * source-clock ticks, and the bus speed is whatever they add up to.
 *
 * The arithmetic follows ESP-IDF's, including the part that contradicts the
 * reference manual. The manual says each counter holds one less than the interval it
 * describes; Espressif's HAL carries a comment reporting that doing this to the two
 * high-period counters measurably overshoots the target frequency, and writes those
 * two as-is. That is a measurement, not a guess, and it is not derivable from the
 * documentation.
 */
static void configure_timing(uint32_t bus_hz)
{
    const uint32_t source_hz = SOC_XTAL_FREQ_MHZ * 1000000u;

    /* Pre-divider for the peripheral's own clock. Keeping the divided clock at least
     * 1024× the bus frequency leaves the counters enough resolution to land on the
     * requested speed rather than near it. */
    const uint32_t clkm_div = source_hz / (bus_hz * 1024u) + 1u;
    const uint32_t sclk_hz = source_hz / clkm_div;
    const uint32_t half = sclk_hz / bus_hz / 2u;
    assert(half >= 8u);

    /* SCL's high time is split in two: part of it is spent watching whether a
     * peripheral is holding the line down to ask for more time, and the rest is the
     * clock pulse proper. */
    const uint32_t wait_high = (bus_hz >= 80000u) ? (half / 2u - 2u) : (half / 4u);
    const uint32_t high = half - wait_high;
    const uint32_t sda_hold = half / 4u;
    const uint32_t sda_sample = half / 2u;

    /* An ordering the hardware assumes: finish looking for clock stretching before
     * sampling SDA, and sample before the pulse ends. */
    assert(wait_high < sda_sample && sda_sample < high);

    /* XTAL rather than the internal RC oscillator. These counters are in source
     * clock ticks, so a source that drifts with temperature drifts the bus with it. */
    hw_reg_write(I2C_CLK_CONF_REG(I2C0),
                 ((clkm_div - 1u) << I2C_SCLK_DIV_NUM_S) | (1u << I2C_SCLK_ACTIVE_S));

    hw_reg_write(I2C_SCL_LOW_PERIOD_REG(I2C0), half - 1u);
    hw_reg_write(I2C_SCL_HIGH_PERIOD_REG(I2C0),
                 (high << I2C_SCL_HIGH_PERIOD_S) |
                     (wait_high << I2C_SCL_WAIT_HIGH_PERIOD_S));
    hw_reg_write(I2C_SDA_HOLD_REG(I2C0), sda_hold - 1u);
    hw_reg_write(I2C_SDA_SAMPLE_REG(I2C0), sda_sample - 1u);
    hw_reg_write(I2C_SCL_START_HOLD_REG(I2C0), half - 1u);
    hw_reg_write(I2C_SCL_RSTART_SETUP_REG(I2C0), half - 1u);
    hw_reg_write(I2C_SCL_STOP_HOLD_REG(I2C0), half - 1u);
    hw_reg_write(I2C_SCL_STOP_SETUP_REG(I2C0), half - 1u);

    /*
     * Hardware timeout, expressed as a power of two in source-clock ticks: how long
     * SCL may sit in one state before the controller abandons the transaction. Sized
     * at roughly five half-cycles, so a peripheral that dies mid-byte cannot hold
     * the bus indefinitely, and clamped to the width of the field.
     */
    uint32_t tout = 32u - (uint32_t)__builtin_clz(5u * half) + 2u;
    if (tout > TIMEOUT_VALUE_MAX) {
        tout = TIMEOUT_VALUE_MAX;
    }
    hw_reg_write(I2C_TO_REG(I2C0), tout | (1u << I2C_TIME_OUT_EN_S));
}

void hw_i2c_init(uint32_t sda_pin, uint32_t scl_pin, uint32_t bus_hz)
{
    assert(bus_hz > 0u);
    s_bus_hz = bus_hz;

    /*
     * The peripheral comes out of boot clock-gated. Its reset is pulsed rather than
     * merely released, because a software restart can catch the controller
     * mid-transaction, and a half-finished state machine that still believes it owns
     * the bus reports the bus as busy forever.
     */
    hw_reg_set_bits(SYSTEM_PERIP_CLK_EN0_REG, 1u << SYSTEM_I2C_EXT0_CLK_EN_S);
    hw_reg_set_bits(SYSTEM_PERIP_RST_EN0_REG, 1u << SYSTEM_I2C_EXT0_RST_S);
    hw_reg_clear_bits(SYSTEM_PERIP_RST_EN0_REG, 1u << SYSTEM_I2C_EXT0_RST_S);

    /*
     * Master mode, arbitration enabled, both lines open-drain.
     *
     * SDA_FORCE_OUT and SCL_FORCE_OUT read like the opposite of open drain, and the
     * reference manual describes them that way. Espressif's own HAL sets both to 1
     * from a function named after enabling open drain, and 1 is the reset value.
     * Where the prose and the shipping driver disagree, the driver is the one that
     * has been on hardware.
     */
    hw_reg_write(I2C_CTR_REG(I2C0),
                 (1u << I2C_MS_MODE_S) | (1u << I2C_ARBITRATION_EN_S) |
                     (1u << I2C_SDA_FORCE_OUT_S) | (1u << I2C_SCL_FORCE_OUT_S));

    configure_timing(bus_hz);

    /* FIFO mode with both FIFOs emptied. */
    hw_reg_write(I2C_FIFO_CONF_REG(I2C0),
                 (1u << I2C_TX_FIFO_RST_S) | (1u << I2C_RX_FIFO_RST_S));
    hw_reg_write(I2C_FIFO_CONF_REG(I2C0), 0u);

    /*
     * A short glitch filter on both lines. The threshold is in source-clock ticks:
     * seven of them is 175 ns at 40 MHz, far shorter than any bit time this bus will
     * use, and long enough to swallow the fast edges that coupling produces on a
     * line running past a switching supply.
     */
    hw_reg_write(I2C_FILTER_CFG_REG(I2C0),
                 (1u << I2C_SCL_FILTER_EN_S) | (1u << I2C_SDA_FILTER_EN_S) |
                     (7u << I2C_SCL_FILTER_THRES_S) | (7u << I2C_SDA_FILTER_THRES_S));

    /*
     * Configuration is staged. None of the timing above reaches the state machine
     * until this bit is set, which is what stops a transaction already in flight from
     * being reconfigured underneath itself.
     */
    hw_reg_set_bits(I2C_CTR_REG(I2C0), 1u << I2C_CONF_UPGATE_S);

    /* Pins last. Routing them any earlier would attach an unconfigured controller to
     * the bus, and the first thing the other devices on it would see is a glitch. */
    hw_gpio_open_drain_init(sda_pin, I2CEXT0_SDA_OUT_IDX, I2CEXT0_SDA_IN_IDX, true);
    hw_gpio_open_drain_init(scl_pin, I2CEXT0_SCL_OUT_IDX, I2CEXT0_SCL_IN_IDX, true);
}

/** @brief Assemble one command word. */
static uint32_t command(uint32_t op_code, uint32_t byte_num, bool check_ack)
{
    return (op_code << CMD_OP_CODE_S) | (check_ack ? (1u << CMD_ACK_EN_S) : 0u) |
           (byte_num << CMD_BYTE_NUM_S);
}

/**
 * @brief How long to wait for a transaction of @p bytes bytes before giving up.
 *
 * Nine bit times per byte — eight of data and one of acknowledgement — plus the
 * start and stop conditions. Ten times that is loose enough to survive a peripheral
 * stretching the clock, and the fixed millisecond keeps a single-byte probe from
 * being handed an unreasonably tight deadline.
 *
 * This backs up the controller's own timeout rather than replacing it. The hardware
 * watches SCL; this watches the hardware.
 */
static uint32_t completion_timeout_us(size_t bytes)
{
    const uint64_t bit_times = 90u * (uint64_t)(bytes + 2u);
    return 1000u + (uint32_t)(bit_times * 1000000u / s_bus_hz);
}

/**
 * @brief Run one write transaction, @p frame beginning with the address byte.
 */
static hw_i2c_result_t run_write(const uint8_t *frame, size_t count)
{
    if (hw_reg_read(I2C_SR_REG(I2C0)) & (1u << I2C_BUS_BUSY_S)) {
        return HW_I2C_BUS_BUSY;
    }

    /* The interrupt raw register latches, so a NACK left behind by the previous
     * transaction would otherwise be read as this one's verdict. */
    hw_reg_write(I2C_FIFO_CONF_REG(I2C0),
                 (1u << I2C_TX_FIFO_RST_S) | (1u << I2C_RX_FIFO_RST_S));
    hw_reg_write(I2C_FIFO_CONF_REG(I2C0), 0u);
    hw_reg_write(I2C_INT_CLR_REG(I2C0), UINT32_MAX);

    for (size_t i = 0; i < count; i++) {
        hw_reg_write(I2C_DATA_REG(I2C0), frame[i]);
    }

    /*
     * The entire transaction as three command words. One WRITE covers the address
     * byte and the payload together, because to the hardware they are the same
     * thing: bytes clocked out of the FIFO with each acknowledgement checked. The
     * address is a protocol convention, not a hardware feature.
     */
    hw_reg_write(CMD_REG(0), command(OP_RESTART, 0u, false));
    hw_reg_write(CMD_REG(1), command(OP_WRITE, count, true));
    hw_reg_write(CMD_REG(2), command(OP_STOP, 0u, false));

    hw_reg_set_bits(I2C_CTR_REG(I2C0), 1u << I2C_TRANS_START_S);

    hw_i2c_result_t result = HW_I2C_TIMEOUT;
    const uint32_t budget_us = completion_timeout_us(count);
    for (uint32_t waited = 0; waited < budget_us; waited++) {
        const uint32_t raw = hw_reg_read(I2C_INT_RAW_REG(I2C0));

        /* Failure flags are tested before completion. An unacknowledged byte ends
         * the transaction, so both are raised together, and the useful half of that
         * pair is the reason it ended rather than the fact that it did. */
        if (raw & (1u << I2C_NACK_INT_RAW_S)) {
            result = HW_I2C_NACK;
            break;
        }
        if (raw & (1u << I2C_ARBITRATION_LOST_INT_RAW_S)) {
            result = HW_I2C_ARB_LOST;
            break;
        }
        if (raw & (1u << I2C_TIME_OUT_INT_RAW_S)) {
            result = HW_I2C_TIMEOUT;
            break;
        }
        if (raw & (1u << I2C_TRANS_COMPLETE_INT_RAW_S)) {
            result = HW_I2C_OK;
            break;
        }

        esp_rom_delay_us(1);
    }

    if (result != HW_I2C_OK) {
        /* Reset the state machine after any failure. A controller stopped partway
         * through still believes it holds the bus, and every later transaction would
         * be refused as busy — one bad device would take the bus down permanently. */
        hw_reg_set_bits(I2C_CTR_REG(I2C0), 1u << I2C_FSM_RST_S);
        hw_reg_set_bits(I2C_CTR_REG(I2C0), 1u << I2C_CONF_UPGATE_S);
    }

    return result;
}

hw_i2c_result_t hw_i2c_write(uint8_t address, const uint8_t *data, size_t len)
{
    assert(address <= 0x7Fu);
    assert(data != NULL || len == 0u);

    if (len > HW_I2C_MAX_PAYLOAD) {
        return HW_I2C_TOO_LONG;
    }

    /* Address and payload are gathered into one buffer because the hardware makes no
     * distinction between them: a single WRITE command clocks out both. */
    uint8_t frame[1u + HW_I2C_MAX_PAYLOAD];
    frame[0] = (uint8_t)(address << 1); /* bit 0 clear — this is a write */
    if (len > 0u) {
        memcpy(&frame[1], data, len);
    }

    return run_write(frame, len + 1u);
}

hw_i2c_result_t hw_i2c_probe(uint8_t address)
{
    assert(address <= 0x7Fu);

    const uint8_t frame = (uint8_t)(address << 1);
    return run_write(&frame, 1u);
}

const char *hw_i2c_result_name(hw_i2c_result_t result)
{
    /* No `default`, so that adding an outcome without a name here is a compile
     * error: ESP-IDF builds with -Werror=all, which makes an unhandled enumerator
     * -Werror=switch. The fallback below covers only a value that is not a valid
     * enumerator at all. */
    switch (result) {
    case HW_I2C_OK:       return "ok";
    case HW_I2C_NACK:     return "no acknowledgement";
    case HW_I2C_TIMEOUT:  return "timed out";
    case HW_I2C_ARB_LOST: return "arbitration lost";
    case HW_I2C_BUS_BUSY: return "bus busy";
    case HW_I2C_TOO_LONG: return "payload too long";
    }
    return "unrecognised";
}
