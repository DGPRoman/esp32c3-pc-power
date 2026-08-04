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

/**
 * @brief SCL pulses used to free a bus left mid-transfer.
 *
 * Nine, because a byte and its acknowledgement are nine bits: a peripheral
 * interrupted at any point inside one needs at most that many clocks to finish it and
 * let go of SDA.
 */
#define SCL_RESET_PULSES 9u

/** @brief Budget for the hardware to emit those pulses. They take 90 µs at 100 kHz. */
#define CLEAR_BUS_TIMEOUT_US 10000u

/** @brief Budget for a lone stop condition, which is a fraction of one bit time. */
#define STOP_TIMEOUT_US 1000u

/**
 * @brief How long to wait for the bus to fall idle before starting a transaction.
 *
 * A stop condition takes half a bit time — five microseconds at 100 kHz — so this is
 * two orders of magnitude more than the normal case needs. Anything approaching it
 * means the bus is held rather than merely finishing.
 */
#define BUS_IDLE_TIMEOUT_US 1000u

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

/**
 * @brief Release a bus that something is still holding, and report whether it worked.
 *
 * A controller reset mid-transaction leaves the peripheral it was talking to halfway
 * through a byte, still driving SDA low and waiting for clocks that are never coming.
 * Nothing about that resolves on its own, and it is not a rare corner: a watchdog
 * reboot, a brownout, or a flash cycle landing during a display redraw all produce
 * it. The symptom is worse than a dead bus, because a low SDA reads as an
 * acknowledgement — a scan reports devices that are not there, and only then does the
 * bus wedge.
 *
 * The C3 does this in silicon. Chips without the feature need the same nine pulses
 * bit-banged through GPIO with software delays; here it is two fields and a wait,
 * and the hardware generates the closing stop condition itself.
 *
 * @return True if the controller finished. False means SCL itself is held down, which
 *         is a wiring fault rather than a confused peripheral, and no amount of
 *         further waiting will change it.
 */
static bool clear_bus(void)
{
    hw_reg_write(I2C_SCL_SP_CONF_REG(I2C0), (SCL_RESET_PULSES << I2C_SCL_RST_SLV_NUM_S) |
                                                (1u << I2C_SCL_RST_SLV_EN_S));
    hw_reg_set_bits(I2C_CTR_REG(I2C0), 1u << I2C_CONF_UPGATE_S);

    /* The hardware clears the enable bit when it is done, so that is the completion
     * signal — there is no separate status flag to read. */
    for (uint32_t waited = 0; waited < CLEAR_BUS_TIMEOUT_US; waited++) {
        if ((hw_reg_read(I2C_SCL_SP_CONF_REG(I2C0)) & (1u << I2C_SCL_RST_SLV_EN_S)) == 0u) {
            /* A second synchronisation, after the hardware has finished rather than
             * before it starts. The controller cleared that enable bit itself, behind
             * the staged register copy the state machine reads from, and without
             * republishing it the next transaction runs against a stale view and times
             * out. Found the hard way: the first probe after a bus clear failed. */
            hw_reg_set_bits(I2C_CTR_REG(I2C0), 1u << I2C_CONF_UPGATE_S);
            return true;
        }
        esp_rom_delay_us(1);
    }

    /* Stop asking, rather than leaving a state machine running that cannot finish. */
    hw_reg_clear_bits(I2C_SCL_SP_CONF_REG(I2C0), 1u << I2C_SCL_RST_SLV_EN_S);
    hw_reg_set_bits(I2C_CTR_REG(I2C0), 1u << I2C_CONF_UPGATE_S);

    return false;
}

/**
 * @brief Put a stop condition on the bus, ending any transaction still in progress.
 *
 * Clocking the bus free only helps a peripheral that was *driving* SDA. One that was
 * interrupted while *receiving* holds nothing, and so has nothing to release — it
 * simply still believes it is inside a transaction. The next address byte then arrives
 * as data, and it acknowledges it, which is how a scan comes to report a device at an
 * address where nothing lives.
 *
 * A stop condition is what tells every peripheral on the bus that whatever it thought
 * was happening is over. ESP-IDF's software bus-clear ends with one for exactly this
 * reason, and whether the C3's hardware version also emits one is not something the
 * headers say, so it is issued unconditionally: one stop condition at startup against a
 * failure mode the clock pulses cannot reach.
 *
 * Worth being precise about what this did and did not do. It was added while chasing a
 * scan that reported devices which were not there, and it did not fix that — the cause
 * turned out to be elsewhere entirely. It is kept because the mode it covers is real
 * and untested, not because it was ever seen to help.
 */
static void emit_stop(void)
{
    hw_reg_write(I2C_FIFO_CONF_REG(I2C0),
                 (1u << I2C_TX_FIFO_RST_S) | (1u << I2C_RX_FIFO_RST_S));
    hw_reg_write(I2C_FIFO_CONF_REG(I2C0), 0u);
    hw_reg_write(I2C_INT_CLR_REG(I2C0), UINT32_MAX);

    /* A command list of one. There is no start and no payload — the point is the stop
     * condition itself, not a transaction to carry it. */
    hw_reg_write(CMD_REG(0), (uint32_t)OP_STOP << CMD_OP_CODE_S);
    hw_reg_set_bits(I2C_CTR_REG(I2C0), 1u << I2C_TRANS_START_S);

    for (uint32_t waited = 0; waited < STOP_TIMEOUT_US; waited++) {
        const uint32_t raw = hw_reg_read(I2C_INT_RAW_REG(I2C0));
        if ((raw & ((1u << I2C_TRANS_COMPLETE_INT_RAW_S) |
                    (1u << I2C_TIME_OUT_INT_RAW_S))) != 0u) {
            break;
        }
        esp_rom_delay_us(1);
    }

    /* Whatever happened, do not leave the verdict behind for the next transaction to
     * read as its own. */
    hw_reg_write(I2C_INT_CLR_REG(I2C0), UINT32_MAX);
}

hw_i2c_result_t hw_i2c_init(uint32_t sda_pin, uint32_t scl_pin, uint32_t bus_hz)
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

    /* Pins after the controller. Routing them any earlier would attach an
     * unconfigured controller to the bus, and the first thing the other devices on it
     * would see is a glitch. */
    hw_gpio_open_drain_init(sda_pin, I2CEXT0_SDA_OUT_IDX, I2CEXT0_SDA_IN_IDX, true);
    hw_gpio_open_drain_init(scl_pin, I2CEXT0_SCL_OUT_IDX, I2CEXT0_SCL_IN_IDX, true);

    /* And the bus recovery after the pins, since the pulses have to reach the wire.
     * Both halves run before anything is allowed to trust what it reads from the bus:
     * clocking frees a peripheral that is holding SDA, and the stop condition ends a
     * transaction one still thinks it is inside. Neither covers the other. */
    if (!clear_bus()) {
        return HW_I2C_BUS_BUSY;
    }
    emit_stop();

    return HW_I2C_OK;
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

/** @brief Wait for the bus to be idle, bounded by ::BUS_IDLE_TIMEOUT_US. */
static bool wait_for_idle(void)
{
    for (uint32_t waited = 0; waited < BUS_IDLE_TIMEOUT_US; waited++) {
        if ((hw_reg_read(I2C_SR_REG(I2C0)) & (1u << I2C_BUS_BUSY_S)) == 0u) {
            return true;
        }
        esp_rom_delay_us(1);
    }

    return false;
}

/**
 * @brief Run one write transaction, @p frame beginning with the address byte.
 */
static hw_i2c_result_t run_write(const uint8_t *frame, size_t count)
{
    /*
     * Wait for the bus rather than refusing it. A transaction is reported complete as
     * soon as its flag is raised, but the stop condition that ends it is still going out
     * on the wire for another half bit time after that — so BUS_BUSY immediately
     * afterwards is the previous transfer finishing normally, not a fault. Treating it
     * as one made every second probe of a bus scan fail. ESP-IDF spins on the same bit
     * for the same reason; the difference here is a bound, so a genuinely held bus is
     * reported instead of hanging.
     */
    if (!wait_for_idle()) {
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

    /*
     * Reset the state machine only when the controller is actually stuck. A timeout or a
     * lost arbitration leaves it partway through a transaction, still believing it holds
     * the bus, and every later transfer would be refused as busy — one unresponsive
     * device would take the bus down permanently.
     *
     * A NACK is not that. The hardware finishes the transaction itself, stop condition
     * included, and leaves the state machine idle; ESP-IDF's own driver resets nothing
     * on a NACK for that reason. Doing it anyway was actively harmful here: a bus scan
     * NACKs on every empty address, and each reset can raise a completion flag late
     * enough for the *next* transaction to clear the register, start, and immediately
     * read that flag as its own — reporting a device at an address where nothing is.
     */
    if (result == HW_I2C_TIMEOUT || result == HW_I2C_ARB_LOST) {
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
