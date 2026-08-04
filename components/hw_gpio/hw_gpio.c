#include "hw_gpio.h"

#include <assert.h>

#include "soc/gpio_reg.h"
#include "soc/gpio_sig_map.h"
#include "soc/io_mux_reg.h"

/*
 * Register addresses come from the `soc` component instead of being copied out of
 * the Technical Reference Manual by hand. Those headers are the same register map
 * in machine-readable form, and a mistyped constant here would not fail to
 * compile — it would write into some unrelated peripheral. The logic below, which
 * is the part worth owning, is this project's own.
 *
 * Asserts guard the pin argument. Every caller passes a compile-time constant from
 * the board pin map, so an out-of-range pin is a programming error rather than a
 * runtime condition, and compiling the checks out in a release build loses nothing.
 */

/**
 * @brief IO_MUX configuration register for @p pin.
 *
 * The C3's pad registers begin one word into the IO_MUX block and run in GPIO
 * order, so the address is computed rather than looked up in a table.
 */
#define PAD_REG(pin) (IO_MUX_GPIO0_REG + ((pin) * 4u))

/** @brief GPIO matrix output-routing register for @p pin. */
#define OUT_SEL_REG(pin) (GPIO_FUNC0_OUT_SEL_CFG_REG + ((pin) * 4u))

/*
 * `volatile` is the entire contract with the compiler here. It declares each
 * access an observable side effect at that exact address, so none of these reads
 * and writes may be held in a register, reordered against each other, merged, or
 * discarded. Without it, code that clears a bit and then sets it again is
 * legitimately optimised away to nothing — the compiler has no other way to know
 * that the store itself is the point.
 */
static inline uint32_t reg_read(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

static inline void reg_write(uint32_t addr, uint32_t value)
{
    *(volatile uint32_t *)addr = value;
}

void hw_gpio_write(uint32_t pin, bool level)
{
    assert(pin <= HW_GPIO_MAX);

    /*
     * W1TS and W1TC — write 1 to set, write 1 to clear — act only on the bits
     * written as 1 and ignore the rest, which makes changing one pin a single
     * store. Read-modify-write on GPIO_OUT_REG would be three instructions with
     * nothing holding them together: an interrupt landing between the read and the
     * write drives its own pin, then this write puts the stale value back and
     * silently undoes it. That lost update is why the hardware provides these.
     */
    reg_write(level ? GPIO_OUT_W1TS_REG : GPIO_OUT_W1TC_REG, 1u << pin);
}

void hw_gpio_output_init(uint32_t pin, bool level)
{
    assert(pin <= HW_GPIO_MAX);

    /* Latch the level while the pad is still high-impedance; the output driver is
     * enabled last, in step 3. */
    hw_gpio_write(pin, level);

    /*
     * Step 1 — aim the pad's function multiplexer at the GPIO matrix. Every pad
     * can be driven either by a peripheral hardwired to it or by the matrix, and
     * until MCU_SEL selects the GPIO function, none of the registers below reach
     * the physical pin at all.
     *
     * The input path and both pull resistors are cleared in the same write. A
     * push-pull output needs none of them, and the bootloader may have left them
     * in any state — writing an explicit value makes this initialisation
     * independent of whatever ran before it.
     */
    uint32_t pad = reg_read(PAD_REG(pin));
    pad &= ~((uint32_t)MCU_SEL_V << MCU_SEL_S);
    pad |= (uint32_t)PIN_FUNC_GPIO << MCU_SEL_S;
    pad &= ~((1u << FUN_IE_S) | (1u << FUN_PU_S) | (1u << FUN_PD_S));
    reg_write(PAD_REG(pin), pad);

    /*
     * Step 2 — choose what the matrix feeds that pad. The register holds a
     * peripheral output signal index; SIG_GPIO_OUT_IDX is the reserved value
     * meaning "take the level from GPIO_OUT_REG" rather than from a peripheral.
     * Writing the whole register also zeroes the inversion and output-enable
     * override bits, which is the behaviour we want.
     */
    reg_write(OUT_SEL_REG(pin), SIG_GPIO_OUT_IDX);

    /* Step 3 — enable the pin's output driver. */
    reg_write(GPIO_ENABLE_W1TS_REG, 1u << pin);
}
