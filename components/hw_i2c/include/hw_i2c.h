/**
 * @file hw_i2c.h
 * @brief I²C master built on the ESP32-C3 controller's command sequencer.
 *
 * The peripheral is not a byte-at-a-time shift register polled through status
 * flags. It runs a short program: up to eight command words describing the shape of
 * a transaction, a 32-byte FIFO holding the payload, and one bit that starts it.
 * That shape is visible in this API — a transaction is handed over whole, and its
 * length is bounded by the FIFO rather than by anything in software.
 *
 * Transfers are blocking and poll for completion. Interrupt-driven transfers would
 * matter for a device streaming continuously; this bus carries short bursts to one
 * display, where the wait is measured in tens of microseconds.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Largest payload a single transaction can carry, in bytes.
 *
 * The hardware FIFO holds 32 bytes and the address byte takes one of them. Longer
 * transfers have to be split, or eventually issued with the sequencer's END
 * command, which pauses a transaction mid-flight so software can refill the FIFO
 * without releasing the bus.
 */
#define HW_I2C_MAX_PAYLOAD 31u

/** @brief Outcome of one transaction. */
typedef enum {
    HW_I2C_OK,       /**< Every byte was acknowledged. */
    HW_I2C_NACK,     /**< A byte was not acknowledged — usually nothing lives at that address. */
    HW_I2C_TIMEOUT,  /**< The transaction did not finish: a line stuck low, or no pull-up at all. */
    HW_I2C_ARB_LOST, /**< Another master won the bus mid-transaction. */
    HW_I2C_BUS_BUSY, /**< The bus was not idle when the transaction was submitted. */
    HW_I2C_TOO_LONG, /**< Payload exceeds ::HW_I2C_MAX_PAYLOAD. */
} hw_i2c_result_t;

/**
 * @brief Bring up the I²C controller as a master.
 *
 * @param sda_pin GPIO carrying SDA.
 * @param scl_pin GPIO carrying SCL.
 * @param bus_hz  Target SCL frequency. The eight timing counters the hardware
 *                actually wants are derived from it.
 */
void hw_i2c_init(uint32_t sda_pin, uint32_t scl_pin, uint32_t bus_hz);

/**
 * @brief Write @p len bytes to the device at 7-bit @p address.
 *
 * @param address 7-bit device address, without the read/write bit.
 * @param data    Bytes to send; may be NULL only when @p len is zero.
 * @param len     Byte count, at most ::HW_I2C_MAX_PAYLOAD.
 */
hw_i2c_result_t hw_i2c_write(uint8_t address, const uint8_t *data, size_t len);

/**
 * @brief Ask whether anything answers at 7-bit @p address.
 *
 * Sends the address by itself and reports whether it was acknowledged, which is the
 * entirety of what a bus scan needs. ::HW_I2C_NACK is the ordinary answer from an
 * address with nothing on it; any other failure describes the bus, not the address.
 */
hw_i2c_result_t hw_i2c_probe(uint8_t address);

/** @brief Name for @p result, for logs. */
const char *hw_i2c_result_name(hw_i2c_result_t result);

#ifdef __cplusplus
}
#endif
