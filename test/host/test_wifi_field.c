/**
 * @file test_wifi_field.c
 * @brief Filling a driver field that carries no length of its own.
 *
 * Linked rather than included: the one function under test is public. Every field
 * here is allocated at exactly the size being claimed for it, so a byte written past
 * the end is an ASan report rather than a check that happened to be looking in the
 * right place — which is the whole point, since the field this stands in for is a
 * struct member with a password on the other side of it.
 */

#include "check.h"

#include "wifi_field.h"

#include <stdlib.h>
#include <string.h>

void test_wifi_field(void);

/** @brief Size of wifi_sta_config_t::ssid, the field with no ssid_len beside it. */
#define SSID_SIZE 32u

/** @brief 802.11's longest SSID: the case that fills the field and leaves no NUL. */
#define LONGEST_SSID "0123456789abcdef0123456789abcdef"

/** @brief Exactly @p size bytes, pre-filled so that an untouched byte is visible. */
static uint8_t *field_of(size_t size)
{
    uint8_t *field = malloc(size);
    CHECK(field != NULL);
    if (field != NULL) {
        memset(field, 0xAA, size);
    }
    return field;
}

static void test_a_value_shorter_than_the_field(void)
{
    uint8_t *field = field_of(SSID_SIZE);

    CHECK(wifi_field_set(field, SSID_SIZE, "home"));
    CHECK(memcmp(field, "home", 5u) == 0); /* Terminator included. */

    for (size_t i = 4u; i < SSID_SIZE; i++) {
        CHECK_EQ(field[i], 0);
    }

    free(field);
}

static void test_a_value_that_fills_the_field(void)
{
    uint8_t *field = field_of(SSID_SIZE);

    CHECK_EQ(strlen(LONGEST_SSID), SSID_SIZE);
    CHECK(wifi_field_set(field, SSID_SIZE, LONGEST_SSID));
    CHECK(memcmp(field, LONGEST_SSID, SSID_SIZE) == 0);

    free(field);
}

static void test_a_value_one_octet_too_long(void)
{
    uint8_t *field = field_of(SSID_SIZE);

    CHECK(!wifi_field_set(field, SSID_SIZE, LONGEST_SSID "!"));

    /* Refused whole. A field holding the front of a name that was rejected is a
     * network nobody asked to join. */
    for (size_t i = 0; i < SSID_SIZE; i++) {
        CHECK_EQ(field[i], 0xAA);
    }

    free(field);
}

static void test_a_value_far_longer_than_the_field(void)
{
    uint8_t *field = field_of(SSID_SIZE);

    char huge[512];
    memset(huge, 'x', sizeof(huge) - 1u);
    huge[sizeof(huge) - 1u] = '\0';

    CHECK(!wifi_field_set(field, SSID_SIZE, huge));
    CHECK_EQ(field[SSID_SIZE - 1u], 0xAA);

    free(field);
}

static void test_the_empty_and_the_absent(void)
{
    uint8_t *field = field_of(SSID_SIZE);

    /* An open network's password: nothing to send, and the field wants zeroing rather
     * than leaving as it was. */
    CHECK(wifi_field_set(field, SSID_SIZE, ""));
    for (size_t i = 0; i < SSID_SIZE; i++) {
        CHECK_EQ(field[i], 0);
    }

    CHECK(!wifi_field_set(field, SSID_SIZE, NULL));
    CHECK(!wifi_field_set(NULL, SSID_SIZE, "home"));

    free(field);
}

static void test_a_field_with_no_room_at_all(void)
{
    /* Degenerate, and here because it is where an off-by-one in the length loop shows
     * up as a write into a field that has nowhere to write. */
    uint8_t *field = field_of(1u);

    CHECK(wifi_field_set(field, 0, ""));
    CHECK(!wifi_field_set(field, 0, "a"));
    CHECK_EQ(field[0], 0xAA);

    free(field);
}

static void test_every_length_around_the_field(void)
{
    for (size_t length = 0; length <= SSID_SIZE + 4u; length++) {
        char value[SSID_SIZE + 8u];
        memset(value, 'a', length);
        value[length] = '\0';

        uint8_t *field = field_of(SSID_SIZE);
        CHECK_EQ(wifi_field_set(field, SSID_SIZE, value), length <= SSID_SIZE);
        free(field);
    }
}

void test_wifi_field(void)
{
    check_begin("wifi_field");
    test_a_value_shorter_than_the_field();
    test_a_value_that_fills_the_field();
    test_a_value_one_octet_too_long();
    test_a_value_far_longer_than_the_field();
    test_the_empty_and_the_absent();
    test_a_field_with_no_room_at_all();
    test_every_length_around_the_field();
}
