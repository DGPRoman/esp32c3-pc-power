/**
 * @file test_ssd1306.c
 * @brief Framebuffer geometry and text rendering, against the actual pixels.
 *
 * The driver's translation unit is included rather than linked, so the tests can
 * read s_framebuffer directly. Reconstructing it from the bytes ssd1306_flush
 * sends would work, but it would test the flush at the same time and report both
 * as one failure.
 */

#include "check.h"

#include "../../components/ssd1306/ssd1306.c"

void test_ssd1306(void);

/** @brief The framebuffer byte holding (x, y), and the bit within it. */
static uint8_t pixel(uint32_t x, uint32_t y)
{
    return (uint8_t)((s_framebuffer[(y / 8u) * SSD1306_WIDTH + x] >> (y % 8u)) & 1u);
}

static unsigned lit_pixels(void)
{
    unsigned count = 0;
    for (size_t i = 0; i < sizeof(s_framebuffer); i++) {
        for (unsigned bit = 0; bit < 8u; bit++) {
            count += (s_framebuffer[i] >> bit) & 1u;
        }
    }
    return count;
}

/** @brief Commands that would not fit one transaction, for the bound below. */
static uint8_t s_oversized[CHUNK_MAX + 8u];

static void test_the_command_frame_is_bounded(void)
{
    /* The bound used to be an assert, which is compiled out wherever NDEBUG is
     * set — and what is left in a release build is a memcpy of caller-chosen
     * length into a 31-byte stack array. ASan is what would notice; a panel in a
     * case would not.
     *
     * Asserted through a public path would be better and is not available: every
     * caller in this file passes a fixed array, which is exactly why the two
     * static_asserts are there. This reaches the static function directly.
     */
    CHECK_EQ(send_commands(s_oversized, CHUNK_MAX), HW_I2C_OK);
    CHECK_EQ(send_commands(s_oversized, CHUNK_MAX + 1u), HW_I2C_TOO_LONG);
    CHECK_EQ(send_commands(s_oversized, sizeof(s_oversized)), HW_I2C_TOO_LONG);

    /* Nothing is a special case at zero: an empty sequence is a control byte and
     * no commands, which the controller accepts and which costs a transaction. */
    CHECK_EQ(send_commands(s_oversized, 0u), HW_I2C_OK);
}

void test_ssd1306(void)
{
    check_begin("ssd1306");

    test_the_command_frame_is_bounded();

    /* -- Geometry ---------------------------------------------------------- */

    ssd1306_clear();
    CHECK_EQ(lit_pixels(), 0);

    /* A page is eight vertically stacked pixels, so y = 9 is bit 1 of page 1.
     * Getting this wrong puts text one row off and only on some lines. */
    ssd1306_set_pixel(3, 9, true);
    CHECK_EQ(pixel(3, 9), 1);
    CHECK_EQ(s_framebuffer[SSD1306_WIDTH + 3u], 0x02);
    CHECK_EQ(lit_pixels(), 1);

    ssd1306_set_pixel(3, 9, false);
    CHECK_EQ(lit_pixels(), 0);

    /* The corners, which is where an off-by-one lands. */
    ssd1306_set_pixel(0, 0, true);
    ssd1306_set_pixel(SSD1306_WIDTH - 1u, SSD1306_HEIGHT - 1u, true);
    CHECK_EQ(pixel(0, 0), 1);
    CHECK_EQ(pixel(SSD1306_WIDTH - 1u, SSD1306_HEIGHT - 1u), 1);
    CHECK_EQ(lit_pixels(), 2);

    /* Off the panel is dropped, not wrapped and not written past the end. The
     * whole of ssd1306_draw_text's clipping rests on this. */
    ssd1306_set_pixel(SSD1306_WIDTH, 0, true);
    ssd1306_set_pixel(0, SSD1306_HEIGHT, true);
    ssd1306_set_pixel(UINT32_MAX, UINT32_MAX, true);
    CHECK_EQ(lit_pixels(), 2);

    /* -- Text -------------------------------------------------------------- */

    ssd1306_clear();
    const uint32_t after = ssd1306_draw_text(0, 0, "A", 1u);

    /* The cursor lands one advance on, ready for the next character. */
    CHECK_EQ(after, FONT5X7_ADVANCE);

    /* Every pixel of the glyph, compared against the font rather than against a
     * copy of it: the renderer's job is to place what the table holds. */
    const uint8_t *glyph = font5x7_glyph('A');
    unsigned expected = 0;
    for (uint32_t column = 0; column < FONT5X7_WIDTH; column++) {
        for (uint32_t row = 0; row < FONT5X7_HEIGHT; row++) {
            const uint8_t on = (glyph[column] >> row) & 1u;
            CHECK_EQ(pixel(column, row), on);
            expected += on;
        }
    }
    /* Nothing outside the glyph, including the advance's blank column. */
    CHECK_EQ(lit_pixels(), expected);

    /* Scaling turns each glyph pixel into a scale×scale block, so the lit count
     * goes up by exactly that factor. */
    ssd1306_clear();
    (void)ssd1306_draw_text(0, 0, "A", 2u);
    CHECK_EQ(lit_pixels(), expected * 4u);
    CHECK_EQ(ssd1306_draw_text(0, 20u, "A", 2u), FONT5X7_ADVANCE * 2u);

    /* Text that runs off the right edge is clipped, not wrapped onto the next
     * page and not written out of bounds. */
    ssd1306_clear();
    const uint32_t past = ssd1306_draw_text(SSD1306_WIDTH - 2u, 0, "AAAA", 1u);
    CHECK(past > SSD1306_WIDTH);
    for (uint32_t y = 0; y < SSD1306_HEIGHT; y++) {
        for (uint32_t x = 0; x < SSD1306_WIDTH - 2u; x++) {
            CHECK_EQ(pixel(x, y), 0);
        }
    }

    /* An empty string moves nothing and draws nothing. */
    ssd1306_clear();
    CHECK_EQ(ssd1306_draw_text(7u, 3u, "", 1u), 7u);
    CHECK_EQ(lit_pixels(), 0);
}
