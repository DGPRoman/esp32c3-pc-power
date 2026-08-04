/**
 * @file font5x7.h
 * @brief A 5×7 bitmap font covering printable ASCII.
 *
 * Column-major, one byte per column, bit 0 the topmost row. That is the same
 * orientation an SSD1306 consumes pixels in, so a renderer copying glyphs onto such
 * a panel never has to transpose anything.
 *
 * Nothing here knows about a display. The font is data.
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Glyph width in pixels, and so the number of bytes per glyph. */
#define FONT5X7_WIDTH 5u

/** @brief Glyph height in pixels. Bits 0 to 6 of each column byte are used. */
#define FONT5X7_HEIGHT 7u

/**
 * @brief Horizontal distance from one glyph's origin to the next.
 *
 * One pixel wider than a glyph, so the gap between characters is built into the
 * advance rather than left for callers to remember. Seven rows plus one blank row
 * likewise makes a text line exactly one page of display memory tall.
 */
#define FONT5X7_ADVANCE (FONT5X7_WIDTH + 1u)

/**
 * @brief The ::FONT5X7_WIDTH column bytes of @p c.
 *
 * Characters outside printable ASCII return a solid block rather than blank space.
 * A visible marker where an unexpected byte arrived is worth more than a gap that
 * looks like deliberate spacing — the failure should be legible on the panel.
 */
const uint8_t *font5x7_glyph(char c);

#ifdef __cplusplus
}
#endif
