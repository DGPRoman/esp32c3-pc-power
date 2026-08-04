/**
 * @file ssd1306.h
 * @brief Driver for the 72×40 OLED on this board, an SSD1306 behind an I²C bus.
 *
 * Drawing goes into a framebuffer held here and reaches the glass only on
 * ::ssd1306_flush. The panel has no way to read its own memory back, so a
 * framebuffer is not an optimisation — it is the only way to change one pixel
 * without knowing what the other 2879 are.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "font5x7.h"
#include "hw_i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Visible width of the panel, in pixels. */
#define SSD1306_WIDTH 72u

/** @brief Visible height of the panel, in pixels. */
#define SSD1306_HEIGHT 40u

/**
 * @brief Bus address of the display.
 *
 * 0x3C is what the startup scan finds. The alternative an SSD1306 can be strapped
 * to is 0x3D; this module is not.
 */
#define SSD1306_ADDRESS 0x3Cu

/**
 * @brief Configure the panel and turn it on, leaving every pixel dark.
 *
 * The framebuffer is cleared and flushed before the display is enabled, so the
 * first thing it shows is deliberate rather than whatever the RAM held at power-on.
 *
 * @return ::HW_I2C_OK, or the first transfer failure. ::HW_I2C_NACK means nothing
 *         answered at ::SSD1306_ADDRESS.
 */
hw_i2c_result_t ssd1306_init(void);

/** @brief Set every pixel in the framebuffer dark. Does not touch the panel. */
void ssd1306_clear(void);

/**
 * @brief Set or clear one framebuffer pixel.
 *
 * Coordinates outside the panel are ignored rather than rejected: callers drawing
 * shapes and text routinely run past an edge, and clipping is this function's job
 * to absorb, not theirs to prevent.
 *
 * @param x  Column, 0 at the left.
 * @param y  Row, 0 at the top.
 * @param on True to light the pixel.
 */
void ssd1306_set_pixel(uint32_t x, uint32_t y, bool on);

/** @brief Vertical pitch of a text line at scale 1: a seven-row glyph plus a blank row. */
#define SSD1306_TEXT_LINE_HEIGHT 8u

/** @brief Vertical pitch of a text line drawn at @p scale. */
#define SSD1306_TEXT_LINE_HEIGHT_AT(scale) (SSD1306_TEXT_LINE_HEIGHT * (scale))

/** @brief Characters that fit across the panel at @p scale. */
#define SSD1306_TEXT_COLUMNS_AT(scale) (SSD1306_WIDTH / (FONT5X7_ADVANCE * (scale)))

/** @brief Text lines that fit down the panel at @p scale. */
#define SSD1306_TEXT_ROWS_AT(scale) (SSD1306_HEIGHT / SSD1306_TEXT_LINE_HEIGHT_AT(scale))

/**
 * @brief Characters that fit across the panel at scale 1.
 *
 * Derived from the font rather than written down, so changing the font cannot leave a
 * stale layout constant behind. Twelve is a coincidence worth keeping: an IPv4
 * address in dotted-quad form runs to fifteen characters at worst, and the ones a
 * home network actually hands out — 192.168.1.42 — are exactly twelve.
 */
#define SSD1306_TEXT_COLUMNS SSD1306_TEXT_COLUMNS_AT(1u)

/** @brief Text lines that fit down the panel at scale 1. */
#define SSD1306_TEXT_ROWS SSD1306_TEXT_ROWS_AT(1u)

/**
 * @brief Draw @p text into the framebuffer with its top-left corner at (@p x, @p y).
 *
 * Blank pixels within each glyph are written as dark rather than skipped, so text
 * covers whatever it is drawn over instead of merging with it. Characters running
 * past an edge are clipped away a pixel at a time; nothing wraps, because a status
 * line that silently reflows is harder to read than one that is visibly cut off.
 *
 * @param scale Integer magnification: every glyph pixel becomes a @p scale square
 *              block. There is no second font, because a device this size needs the
 *              same characters at two sizes — a state word legible across a room and
 *              an address legible up close — and scaling the one font keeps both
 *              answerable by the same 475 bytes of verified data. Values above 1 are
 *              blocky by construction; that is the trade, and at these sizes it reads
 *              better than a smoothed glyph would.
 *
 * @return The x coordinate immediately after the last glyph, so consecutive pieces of
 *         a line can be drawn without the caller recomputing widths.
 */
uint32_t ssd1306_draw_text(uint32_t x, uint32_t y, const char *text, uint32_t scale);

/**
 * @brief Send the framebuffer to the panel.
 *
 * @return ::HW_I2C_OK, or the first transfer failure.
 */
hw_i2c_result_t ssd1306_flush(void);

#ifdef __cplusplus
}
#endif
