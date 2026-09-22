#include "hw_gpio.h"

#include <assert.h>

#include "hw_reg.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_sig_map.h"
#include "soc/io_mux_reg.h"

/*
 * Register addresses come from the `soc` component instead of being copied out of
 * the Technical Reference Manual by hand. Those headers are the same register map
 * in machine-readable form, and a mistyped address would not fail to compile — it
 * would write into some unrelated peripheral. The logic below, which is the part
 * worth owning, is this project's own.
 *
 * Asserts guard the arguments. Every caller passes compile-time constants from the
 * board pin map, so an out-of-range value is a programming error rather than a
 * runtime condition, and compiling the checks out in a release build loses nothing.
 */

/**
 * @brief IO_MUX configuration register for @p pin.
 *
 * The C3's pad registers begin one word into the IO_MUX block and run in GPIO
 * order, so the address is computed rather than looked up in a table.
 */
#define PAD_REG(pin) (IO_MUX_GPIO0_REG + ((pin) * 4u))

/** @brief Per-pin GPIO configuration register: open-drain, interrupt type, wake-up. */
#define PIN_REG(pin) (GPIO_PIN0_REG + ((pin) * 4u))

/** @brief GPIO matrix output-routing register for @p pin: which signal drives it. */
#define OUT_SEL_REG(pin) (GPIO_FUNC0_OUT_SEL_CFG_REG + ((pin) * 4u))

/**
 * @brief GPIO matrix input-routing register for peripheral signal @p sig.
 *
 * Note the asymmetry with ::OUT_SEL_REG, which is indexed by pin while this is
 * indexed by signal. Reaching for the wrong one is the standard way to spend an
 * afternoon on a bus that is electrically perfect and answers nothing.
 */
#define IN_SEL_REG(sig) (GPIO_FUNC0_IN_SEL_CFG_REG + ((sig) * 4u))

/** @brief Peripheral signals the GPIO matrix can route, and so the limit on @p sig. */
#define MATRIX_SIGNAL_COUNT 128u

/*
 * The matrix-routing enable bit in an input-routing register. The generated header
 * names it after signal 0 (GPIO_SIG0_IN_SEL) rather than following the GPIO_FUNCn_
 * convention of every other field in the same register, which is why it is easy to
 * miss — and missing it is expensive: the peripheral then reads a constant instead
 * of the pin. Every input-routing register has an identical layout, so the
 * signal-0 constant is the right one to use for all of them.
 */
#define MATRIX_IN_ENABLE (1u << GPIO_SIG0_IN_SEL_S)

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
    hw_reg_write(level ? GPIO_OUT_W1TS_REG : GPIO_OUT_W1TC_REG, 1u << pin);
}

/**
 * @brief Point @p pin's function multiplexer at the GPIO matrix.
 *
 * Every pad can be driven either by a peripheral hardwired to it or by the matrix,
 * and until MCU_SEL selects the GPIO function, none of the GPIO registers reach the
 * physical pin at all. The pulls and the input path are written explicitly rather
 * than left alone, because the bootloader may have set them to anything and an
 * initialisation that depends on what ran before it is not one.
 */
static void configure_pad(uint32_t pin, bool input_enable, bool pull_up)
{
    uint32_t pad = hw_reg_read(PAD_REG(pin));

    pad &= ~((uint32_t)MCU_SEL_V << MCU_SEL_S);
    pad |= (uint32_t)PIN_FUNC_GPIO << MCU_SEL_S;

    pad &= ~((1u << FUN_IE_S) | (1u << FUN_PU_S) | (1u << FUN_PD_S));
    if (input_enable) {
        pad |= 1u << FUN_IE_S;
    }
    if (pull_up) {
        pad |= 1u << FUN_PU_S;
    }

    hw_reg_write(PAD_REG(pin), pad);
}

void hw_gpio_output_init(uint32_t pin, bool level)
{
    assert(pin <= HW_GPIO_MAX);

    /* Latch the level while the pad is still high-impedance; the output driver is
     * enabled last. */
    hw_gpio_write(pin, level);

    /* A push-pull output needs neither the input path nor a pull resistor. */
    configure_pad(pin, false, false);

    /* Push-pull rather than open-drain, in case the pin was left otherwise. */
    hw_reg_clear_bits(PIN_REG(pin), 1u << GPIO_PIN0_PAD_DRIVER_S);

    /*
     * Choose what the matrix feeds the pad. The register holds a peripheral output
     * signal index; SIG_GPIO_OUT_IDX is the reserved value meaning "take the level
     * from GPIO_OUT_REG" rather than from a peripheral. Writing the whole register
     * also zeroes the inversion and output-enable override bits. Leaving OEN_SEL
     * clear is correct even though it reads as "the peripheral controls the output
     * enable": for this signal index that peripheral enable is GPIO_ENABLE, set
     * immediately below.
     */
    hw_reg_write(OUT_SEL_REG(pin), SIG_GPIO_OUT_IDX);

    hw_reg_write(GPIO_ENABLE_W1TS_REG, 1u << pin);
}

void hw_gpio_input_init(uint32_t pin, bool pull_up)
{
    assert(pin <= HW_GPIO_MAX);

    /*
     * Stop driving before enabling the input path, not after. A pin left as an
     * output from an earlier configuration would otherwise be read back as its own
     * output level for the instant in between — a reading that looks perfectly
     * plausible and says nothing about what is on the other end of the wire.
     */
    hw_reg_write(GPIO_ENABLE_W1TC_REG, 1u << pin);

    configure_pad(pin, true, pull_up);

    /* Push-pull rather than open-drain, in case the pin was left otherwise. With
     * the output disabled this changes nothing electrically; it leaves the pin in
     * the state hw_gpio_output_init would find rather than one it would have to
     * undo. */
    hw_reg_clear_bits(PIN_REG(pin), 1u << GPIO_PIN0_PAD_DRIVER_S);
}

bool hw_gpio_read(uint32_t pin)
{
    assert(pin <= HW_GPIO_MAX);

    /*
     * GPIO_IN_REG carries what the pads are actually at, which is not the same as
     * GPIO_OUT_REG: a pin held low by something stronger than this chip reads low
     * here while the output latch still says high. On a sense line that difference
     * is the entire measurement.
     */
    return (hw_reg_read(GPIO_IN_REG) & (1u << pin)) != 0u;
}

void hw_gpio_open_drain_init(uint32_t pin, uint32_t out_signal, uint32_t in_signal,
                             bool pull_up)
{
    assert(pin <= HW_GPIO_MAX);
    assert(out_signal <= GPIO_FUNC0_OUT_SEL_V);
    assert(in_signal < MATRIX_SIGNAL_COUNT);

    /* Release the line before anything can drive it. With the output latch high and
     * the pad open-drain, the pin is passive as far as the bus is concerned, so
     * bringing it up cannot look like a device asserting a start condition. */
    hw_gpio_write(pin, true);

    /*
     * Open drain. With PAD_DRIVER set, writing a 1 stops driving instead of sourcing
     * current, so a device pulling the line low is not fighting a driver pushing it
     * high — on a shared bus, that fight is a short circuit through both devices.
     */
    hw_reg_set_bits(PIN_REG(pin), 1u << GPIO_PIN0_PAD_DRIVER_S);

    /* The input path stays on. A bus controller has to read back the line it is
     * driving: that is how it sees an acknowledgement, and how it notices a
     * peripheral holding the clock low to ask for time. */
    configure_pad(pin, true, pull_up);

    /*
     * Output: which signal reaches this pin. OEN_SEL is left clear, which hands the
     * output enable to the peripheral rather than to GPIO_ENABLE — the peripheral is
     * what decides, bit by bit, whether the line is pulled down or released.
     */
    hw_reg_write(OUT_SEL_REG(pin), out_signal);

    /* Input: which pin reaches this signal, plus the enable that makes the matrix
     * carry it at all. */
    hw_reg_write(IN_SEL_REG(in_signal), pin | MATRIX_IN_ENABLE);

    hw_reg_write(GPIO_ENABLE_W1TS_REG, 1u << pin);
}
