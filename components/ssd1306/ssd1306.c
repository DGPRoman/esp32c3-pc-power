#include "ssd1306.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

/*
 * The panel is smaller than the controller driving it. An SSD1306 addresses 128
 * columns and eight pages of GDDRAM — 128×40 would still be 128 wide — while this
 * glass is 72 pixels across, and those 72 columns are bonded to the middle of the
 * controller's 128. Writing from column 0 therefore puts the image 28 pixels left of
 * the visible area, and what reaches the screen is the right-hand fragment of it.
 *
 * This is the single most common reason a 0.42" panel comes up "almost working".
 */
#define COLUMN_OFFSET 28u

/** @brief Pages the visible rows occupy. A page is eight vertically stacked pixels. */
#define PAGE_COUNT (SSD1306_HEIGHT / 8u)

/*
 * Every transaction to an SSD1306 opens with a control byte. Bit 6 selects whether
 * the bytes after it are commands or pixel data, and bit 7 left clear means "all of
 * the remaining ones are", so a single control byte covers the whole transaction.
 */
#define CONTROL_COMMAND 0x00u
#define CONTROL_DATA 0x40u

/** @brief Pixel bytes per transaction: the payload limit, less the control byte. */
#define CHUNK_MAX (HW_I2C_MAX_PAYLOAD - 1u)

enum {
    CMD_SET_MEMORY_MODE = 0x20,
    CMD_SET_COLUMN_ADDRESS = 0x21,
    CMD_SET_PAGE_ADDRESS = 0x22,
    CMD_DEACTIVATE_SCROLL = 0x2E,
    CMD_SET_START_LINE = 0x40,
    CMD_SET_CONTRAST = 0x81,
    CMD_CHARGE_PUMP = 0x8D,
    CMD_SEGMENT_REMAP_ON = 0xA1,
    CMD_ENTIRE_DISPLAY_FOLLOW_RAM = 0xA4,
    CMD_NORMAL_DISPLAY = 0xA6,
    CMD_SET_MULTIPLEX = 0xA8,
    CMD_DISPLAY_OFF = 0xAE,
    CMD_DISPLAY_ON = 0xAF,
    CMD_COM_SCAN_DESCENDING = 0xC8,
    CMD_SET_DISPLAY_OFFSET = 0xD3,
    CMD_SET_CLOCK_DIVIDE = 0xD5,
    CMD_SET_PRECHARGE = 0xD9,
    CMD_SET_COM_PINS = 0xDA,
    CMD_SET_VCOMH = 0xDB,
};

/*
 * One display, so one framebuffer. The layout is page-major because that is the
 * order the controller consumes bytes in: each byte is a vertical run of eight
 * pixels with the topmost in bit 0. The shape is dictated by the hardware, not
 * chosen here — which is why ::ssd1306_set_pixel has to do arithmetic.
 */
static uint8_t s_framebuffer[SSD1306_WIDTH * PAGE_COUNT];

/** @brief Send @p count command bytes as one transaction. */
static hw_i2c_result_t send_commands(const uint8_t *commands, size_t count)
{
    assert(count <= CHUNK_MAX);

    uint8_t frame[HW_I2C_MAX_PAYLOAD];
    frame[0] = CONTROL_COMMAND;
    memcpy(&frame[1], commands, count);

    return hw_i2c_write(SSD1306_ADDRESS, frame, count + 1u);
}

hw_i2c_result_t ssd1306_init(void)
{
    static const uint8_t setup[] = {
        /* Dark while it is reconfigured. Otherwise the panel spends the next few
         * milliseconds showing whatever its RAM held at power-on, interpreted
         * through whatever settings it last had. */
        CMD_DISPLAY_OFF,

        /* Oscillator frequency and clock divide. This is the reset value, written
         * back explicitly so a warm restart cannot inherit something else. */
        CMD_SET_CLOCK_DIVIDE, 0x80,

        /* Multiplex ratio is the row count less one — 40 of the controller's 64. */
        CMD_SET_MULTIPLEX, SSD1306_HEIGHT - 1u,

        /* No vertical shift, and RAM line 0 at the top: unlike the columns, this
         * panel's 40 rows are bonded to the controller's first 40, so the vertical
         * mapping is direct and needs no offset. */
        CMD_SET_DISPLAY_OFFSET, 0x00,
        CMD_SET_START_LINE | 0x00,

        /* The charge pump makes the panel's ~7 V from 3V3. This module has no
         * external supply for it, so leaving it off yields a display that is
         * configured correctly, acknowledges every byte, and stays black — the
         * failure that looks least like its own cause. */
        CMD_CHARGE_PUMP, 0x14,

        /* Horizontal addressing: the write pointer walks along a page and wraps to
         * the start of the next, which is what lets the framebuffer go out as one
         * linear stream instead of page by page. */
        CMD_SET_MEMORY_MODE, 0x00,

        /* Column and row order. Which setting is correct is a property of how the
         * glass is bonded to the controller and is not derivable from the
         * datasheet; the geometry probe drawn at startup is what settles it. */
        CMD_SEGMENT_REMAP_ON,
        CMD_COM_SCAN_DESCENDING,

        /* Alternative COM pin layout, without the left/right swap. */
        CMD_SET_COM_PINS, 0x12,

        CMD_SET_CONTRAST, 0x7F,
        CMD_SET_PRECHARGE, 0x22,
        CMD_SET_VCOMH, 0x20,

        CMD_ENTIRE_DISPLAY_FOLLOW_RAM,
        CMD_NORMAL_DISPLAY,
        CMD_DEACTIVATE_SCROLL,
    };

    hw_i2c_result_t result = send_commands(setup, sizeof(setup));
    if (result != HW_I2C_OK) {
        return result;
    }

    /* Blank the RAM before the panel becomes visible, so the first thing it shows is
     * something this firmware decided to show. */
    ssd1306_clear();
    result = ssd1306_flush();
    if (result != HW_I2C_OK) {
        return result;
    }

    static const uint8_t enable[] = { CMD_DISPLAY_ON };
    return send_commands(enable, sizeof(enable));
}

void ssd1306_clear(void)
{
    memset(s_framebuffer, 0, sizeof(s_framebuffer));
}

void ssd1306_set_pixel(uint32_t x, uint32_t y, bool on)
{
    if (x >= SSD1306_WIDTH || y >= SSD1306_HEIGHT) {
        return;
    }

    const size_t index = (y / 8u) * SSD1306_WIDTH + x;
    const uint8_t bit = (uint8_t)(1u << (y % 8u));

    if (on) {
        s_framebuffer[index] |= bit;
    } else {
        s_framebuffer[index] = (uint8_t)(s_framebuffer[index] & ~bit);
    }
}

hw_i2c_result_t ssd1306_flush(void)
{
    /* Point the write pointer at the visible window. Both ranges are inclusive, and
     * the column range is where COLUMN_OFFSET earns its keep. */
    static const uint8_t window[] = {
        CMD_SET_COLUMN_ADDRESS, COLUMN_OFFSET, COLUMN_OFFSET + SSD1306_WIDTH - 1u,
        CMD_SET_PAGE_ADDRESS,   0x00,          PAGE_COUNT - 1u,
    };

    hw_i2c_result_t result = send_commands(window, sizeof(window));
    if (result != HW_I2C_OK) {
        return result;
    }

    /*
     * The framebuffer is an order of magnitude larger than one transaction can
     * carry, so it leaves in chunks. That works because the controller's write
     * pointer is internal state a stop condition does not disturb: it keeps
     * advancing across transactions, and the chunks reassemble into one stream.
     *
     * The alternative is the I²C sequencer's END command, which suspends a
     * transaction so the FIFO can be refilled without ever releasing the bus. It
     * matters when a second master could interleave; with one master it saves about
     * a millisecond in thirty and costs considerably more machinery.
     */
    uint8_t frame[HW_I2C_MAX_PAYLOAD];
    frame[0] = CONTROL_DATA;

    for (size_t sent = 0; sent < sizeof(s_framebuffer);) {
        size_t chunk = sizeof(s_framebuffer) - sent;
        if (chunk > CHUNK_MAX) {
            chunk = CHUNK_MAX;
        }

        memcpy(&frame[1], &s_framebuffer[sent], chunk);
        result = hw_i2c_write(SSD1306_ADDRESS, frame, chunk + 1u);
        if (result != HW_I2C_OK) {
            return result;
        }

        sent += chunk;
    }

    return HW_I2C_OK;
}
