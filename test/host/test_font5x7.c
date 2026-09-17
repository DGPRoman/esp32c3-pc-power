/**
 * @file test_font5x7.c
 * @brief The font table's invariants, not its artwork.
 *
 * Asserting the exact columns of a glyph would restate the table, and a test that
 * restates its subject cannot disagree with it. What is worth holding is the
 * contract the header states: the orientation, the advance, and what happens to a
 * byte that is not a printable character.
 */

#include "check.h"

#include "font5x7.h"

void test_font5x7(void);

void test_font5x7(void)
{
    check_begin("font5x7");

    /* Space is the one glyph whose appearance is specified: nothing. */
    const uint8_t *space = font5x7_glyph(' ');
    for (unsigned column = 0; column < FONT5X7_WIDTH; column++) {
        CHECK_EQ(space[column], 0x00);
    }

    /* Bit 7 is outside the 7-pixel glyph height. A set one would light a row the
     * panel places in the next text line, which reads as a rendering fault
     * somewhere else entirely. */
    for (char c = 0x20; c > 0; c++) {
        const uint8_t *glyph = font5x7_glyph(c);
        for (unsigned column = 0; column < FONT5X7_WIDTH; column++) {
            CHECK_EQ(glyph[column] & 0x80u, 0x00);
        }
    }

    /* Printable characters other than space must actually draw something. A hole
     * in the table would otherwise present as text with gaps in it. */
    for (char c = 0x21; c > 0; c++) {
        const uint8_t *glyph = font5x7_glyph(c);
        unsigned lit = 0;
        for (unsigned column = 0; column < FONT5X7_WIDTH; column++) {
            lit |= glyph[column];
        }
        CHECK(lit != 0u);
    }

    /* Outside printable ASCII: a solid block, as the header promises. Control
     * characters and DEL are below and above the table. */
    const uint8_t *control = font5x7_glyph('\n');
    const uint8_t *del = font5x7_glyph(0x7F);
    for (unsigned column = 0; column < FONT5X7_WIDTH; column++) {
        CHECK_EQ(control[column], 0x7F);
        CHECK_EQ(del[column], 0x7F);
    }

    /*
     * The byte above 0x7F is the one that matters. `char` is signed on this
     * architecture and on the ESP32-C3 both, so a byte from a UTF-8 sequence —
     * the first byte of "ї" is 0xD1 — arrives negative. Compared without care it
     * is below the table's first character and indexes backwards out of it.
     */
    const uint8_t *high = font5x7_glyph((char)0xD1);
    for (unsigned column = 0; column < FONT5X7_WIDTH; column++) {
        CHECK_EQ(high[column], 0x7F);
    }

    /* The advance builds the inter-character gap in, which is what lets a caller
     * lay out a line without knowing the glyph width. */
    CHECK_EQ(FONT5X7_ADVANCE, FONT5X7_WIDTH + 1u);
}
