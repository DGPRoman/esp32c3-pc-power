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

/**
 * @brief Send the framebuffer to the panel.
 *
 * @return ::HW_I2C_OK, or the first transfer failure.
 */
hw_i2c_result_t ssd1306_flush(void);

#ifdef __cplusplus
}
#endif
