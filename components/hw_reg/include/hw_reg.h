/**
 * @file hw_reg.h
 * @brief Memory-mapped peripheral register access.
 *
 * Peripherals on this chip are memory-mapped, and the only thing separating a
 * working driver from code the compiler is entitled to delete is `volatile`.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Read the 32-bit register at @p addr.
 *
 * `volatile` declares the access an observable side effect at that exact address,
 * so it may not be kept in a register, reordered against another volatile access,
 * merged with one, or discarded. Without it, a sequence that clears a bit and then
 * sets it again is legitimately optimised away to nothing: the compiler has no
 * other way to know that the store itself is the point.
 */
static inline uint32_t hw_reg_read(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

/** @brief Write @p value to the 32-bit register at @p addr. */
static inline void hw_reg_write(uint32_t addr, uint32_t value)
{
    *(volatile uint32_t *)addr = value;
}

/**
 * @brief Set the bits of @p bits in the register at @p addr, leaving the rest.
 *
 * Read-modify-write, and therefore not safe against concurrent access to the same
 * register. Where the hardware offers a write-1-to-set register, use that instead:
 * it turns the same intent into a single store that cannot lose an update.
 *
 * In this firmware that warning has exactly one place it bites, and it is worth
 * naming so the rest is not read as luck. Every register these drivers touch is
 * owned by one peripheral and driven by one component — except PERIP_CLK_EN0 and
 * PERIP_RST_EN0, which hold a bit per peripheral and are therefore shared with every
 * other bring-up on the chip, ESP-IDF's included. Those two are reached through
 * periph_module_enable() and periph_module_reset() in hw_i2c.c, because a lock of our
 * own would not be the lock the other writer takes.
 */
static inline void hw_reg_set_bits(uint32_t addr, uint32_t bits)
{
    hw_reg_write(addr, hw_reg_read(addr) | bits);
}

/** @brief Clear the bits of @p bits. Read-modify-write; see ::hw_reg_set_bits. */
static inline void hw_reg_clear_bits(uint32_t addr, uint32_t bits)
{
    hw_reg_write(addr, hw_reg_read(addr) & ~bits);
}

/**
 * @brief Replace one field of the register at @p addr.
 *
 * @param addr  Register address.
 * @param mask  Field width as an unshifted mask, e.g. 0x7 for three bits.
 * @param shift Position of the field's least significant bit.
 * @param value New field value; bits above @p mask are ignored.
 */
static inline void hw_reg_set_field(uint32_t addr, uint32_t mask, uint32_t shift,
                                    uint32_t value)
{
    uint32_t reg = hw_reg_read(addr);
    reg &= ~(mask << shift);
    reg |= (value & mask) << shift;
    hw_reg_write(addr, reg);
}

#ifdef __cplusplus
}
#endif
