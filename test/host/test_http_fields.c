/**
 * @file test_http_fields.c
 * @brief Reading values out of a request body — including bodies nobody sends by hand.
 *
 * Linked rather than included: everything under test is public. This is the layer
 * where the network's bytes become this device's data, so most of what is below is
 * input a browser would never produce and an attacker would.
 */

#include "check.h"

#include "http_fields.h"

#include <string.h>

void test_http_fields(void);

/** @brief Decode one field into a fixed buffer, reporting what happened to it. */
static http_field_result_t decode(const char *body, const char *name, char *out, size_t size)
{
    memset(out, '!', size);
    return http_fields_form_value(body, name, out, size);
}

static void test_form_values(void)
{
    char value[64];

    CHECK_EQ(decode("ssid=home", "ssid", value, sizeof(value)), HTTP_FIELD_OK);
    CHECK_EQ_STR(value, "home");

    /* '+' is a space in this encoding, unlike in a URL's own query string. */
    CHECK_EQ(decode("ssid=my+network", "ssid", value, sizeof(value)), HTTP_FIELD_OK);
    CHECK_EQ_STR(value, "my network");

    CHECK_EQ(decode("ssid=caf%C3%A9", "ssid", value, sizeof(value)), HTTP_FIELD_OK);
    CHECK_EQ_STR(value, "caf\xC3\xA9");

    /* Lower-case hex, and a literal percent. */
    CHECK_EQ(decode("ssid=100%25+done", "ssid", value, sizeof(value)), HTTP_FIELD_OK);
    CHECK_EQ_STR(value, "100% done");
    CHECK_EQ(decode("ssid=%c3%a9", "ssid", value, sizeof(value)), HTTP_FIELD_OK);
    CHECK_EQ_STR(value, "\xC3\xA9");

    /* Later fields, and a value that ends at the separator rather than the body. */
    CHECK_EQ(decode("ssid=home&password=secret", "password", value, sizeof(value)),
             HTTP_FIELD_OK);
    CHECK_EQ_STR(value, "secret");
    CHECK_EQ(decode("ssid=home&password=secret", "ssid", value, sizeof(value)), HTTP_FIELD_OK);
    CHECK_EQ_STR(value, "home");

    /* An empty value is a value: the form submits one for an open network. */
    CHECK_EQ(decode("ssid=home&password=", "password", value, sizeof(value)), HTTP_FIELD_OK);
    CHECK_EQ_STR(value, "");
}

static void test_a_missing_field_is_not_a_broken_one(void)
{
    char value[64];

    CHECK_EQ(decode("ssid=home", "password", value, sizeof(value)), HTTP_FIELD_ABSENT);
    CHECK_EQ(decode("", "ssid", value, sizeof(value)), HTTP_FIELD_ABSENT);

    /* A name that is a prefix of a real field is not that field. */
    CHECK_EQ(decode("ssidx=home", "ssid", value, sizeof(value)), HTTP_FIELD_ABSENT);
}

static void test_an_encoded_nul_is_refused(void)
{
    char value[64];

    /* %00 decodes to a terminator. Everything downstream is a C string, so it does
     * not store a byte — it ends the value, and the submitter is told the network
     * was saved. Proven against the previous implementation: "secret%00rest" was
     * stored as "secret" with no error. */
    CHECK_EQ(decode("password=secret%00rest", "password", value, sizeof(value)),
             HTTP_FIELD_INVALID);
    CHECK_EQ_STR(value, "");

    /* Including when it is the whole value, where the truncation is total. */
    CHECK_EQ(decode("password=%00", "password", value, sizeof(value)), HTTP_FIELD_INVALID);

    /* And in upper case, since hex_digit accepts both. */
    CHECK_EQ(decode("ssid=a%00b", "ssid", value, sizeof(value)), HTTP_FIELD_INVALID);
}

static void test_a_broken_escape_is_refused(void)
{
    char value[64];

    CHECK_EQ(decode("ssid=%zz", "ssid", value, sizeof(value)), HTTP_FIELD_INVALID);
    CHECK_EQ(decode("ssid=%4", "ssid", value, sizeof(value)), HTTP_FIELD_INVALID);
    CHECK_EQ(decode("ssid=%", "ssid", value, sizeof(value)), HTTP_FIELD_INVALID);
    /* Cut off by the next field rather than by the end of the body. */
    CHECK_EQ(decode("ssid=%4&password=x", "ssid", value, sizeof(value)), HTTP_FIELD_INVALID);
}

static void test_a_value_too_long_is_refused_not_truncated(void)
{
    char value[8];

    /* Seven bytes and a terminator fit. */
    CHECK_EQ(decode("ssid=1234567", "ssid", value, sizeof(value)), HTTP_FIELD_OK);
    CHECK_EQ_STR(value, "1234567");

    CHECK_EQ(decode("ssid=12345678", "ssid", value, sizeof(value)), HTTP_FIELD_INVALID);
    /* Not a prefix of what was sent: a caller that ignored the result would
     * otherwise be handed a credential that is quietly the wrong one. */
    CHECK_EQ_STR(value, "");

    /* The same, counted after decoding rather than before it. */
    char four[4];
    CHECK_EQ(decode("ssid=%41%42%43", "ssid", four, sizeof(four)), HTTP_FIELD_OK);
    CHECK_EQ_STR(four, "ABC");
    CHECK_EQ(decode("ssid=%41%42%43%44", "ssid", four, sizeof(four)), HTTP_FIELD_INVALID);
}

static void test_json_booleans(void)
{
    bool on = false;

    CHECK(http_fields_json_bool("{\"on\":true}", "on", &on));
    CHECK(on);

    CHECK(http_fields_json_bool("{\"on\":false}", "on", &on));
    CHECK(!on);

    /* Whitespace anywhere JSON allows it. */
    on = false;
    CHECK(http_fields_json_bool("{ \"on\" : \r\n\ttrue }", "on", &on));
    CHECK(on);

    /* The named member, not the first one. */
    CHECK(http_fields_json_bool("{\"force\":true,\"on\":false}", "on", &on));
    CHECK(!on);
    CHECK(http_fields_json_bool("{\"force\":true,\"on\":false}", "force", &on));
    CHECK(on);
}

static void test_a_truncated_or_trailing_literal_is_refused(void)
{
    bool on = false;

    /* "truthy" and "true_nonsense" both begin with "true" and were read as it: the
     * comparison was four bytes long and nothing checked what came next. */
    CHECK(!http_fields_json_bool("{\"on\":truthy}", "on", &on));
    CHECK(!http_fields_json_bool("{\"on\":true_nonsense}", "on", &on));
    CHECK(!http_fields_json_bool("{\"on\":falsehood}", "on", &on));

    /* Cut short, which is what a body truncated in flight gives. */
    CHECK(!http_fields_json_bool("{\"on\":tru", "on", &on));
    CHECK(!http_fields_json_bool("{\"on\":fals", "on", &on));

    /* Neither token at all. */
    CHECK(!http_fields_json_bool("{\"on\":1}", "on", &on));
    CHECK(!http_fields_json_bool("{\"on\":\"true\"}", "on", &on));
    CHECK(!http_fields_json_bool("{\"on\":null}", "on", &on));
    CHECK(!http_fields_json_bool("{\"on\"}", "on", &on));
    CHECK(!http_fields_json_bool("{\"off\":true}", "on", &on));
    CHECK(!http_fields_json_bool("", "on", &on));

    /* A literal that ends where JSON says it may. */
    CHECK(http_fields_json_bool("{\"on\":true,\"force\":false}", "on", &on));
    CHECK(on);
    CHECK(http_fields_json_bool("[{\"on\":false}]", "on", &on));
    CHECK(!on);
}

static void test_a_key_inside_a_string_is_not_a_key(void)
{
    bool on = true;

    /* The member is false; the text of another member contains what looks like it
     * set to true. strstr found the decoy first and the device switched on. */
    CHECK(http_fields_json_bool("{\"note\":\"\\\"on\\\":true\",\"on\":false}", "on", &on));
    CHECK(!on);

    /* The other order, so this cannot pass by reading the last match instead. */
    on = false;
    CHECK(http_fields_json_bool("{\"on\":true,\"note\":\"\\\"on\\\":false\"}", "on", &on));
    CHECK(on);

    /* A decoy with no real member behind it is not a member. */
    CHECK(!http_fields_json_bool("{\"note\":\"\\\"on\\\":true\"}", "on", &on));

    /* A name that is a suffix or prefix of a real key is not that key. */
    CHECK(!http_fields_json_bool("{\"onward\":true}", "on", &on));
    CHECK(!http_fields_json_bool("{\"button_on\":true}", "on", &on));

    /* An escaped quote must not end the string. Get this wrong and every quote
     * after it is off by one, so the real member is read as being inside a string
     * and never found at all — the device answers "no such field" to a request
     * that plainly carries it.
     *
     * The earlier decoys above survive a parser that ignores escapes, by accident
     * of where their quotes fall. This one does not. */
    on = false;
    CHECK(http_fields_json_bool("{\"note\":\"\\\"\",\"on\":true}", "on", &on));
    CHECK(on);

    /* A string equal to the name, in a position where a key cannot be. Without the
     * colon check the array element is taken for the key and the token after the
     * comma is read as its value — so this answers true while the member says
     * false. */
    on = true;
    CHECK(http_fields_json_bool("{\"list\":[\"on\",true],\"on\":false}", "on", &on));
    CHECK(!on);

    /* An unterminated string runs to the end of the body without reading past it;
     * ASan is what proves the second half of that. */
    CHECK(!http_fields_json_bool("{\"on\":true,\"note\":\"unclosed", "note", &on));
    CHECK(!http_fields_json_bool("{\"note\":\"ends with a backslash\\", "on", &on));
}

void test_http_fields(void)
{
    check_begin("http_fields");
    test_form_values();
    test_a_missing_field_is_not_a_broken_one();
    test_an_encoded_nul_is_refused();
    test_a_broken_escape_is_refused();
    test_a_value_too_long_is_refused_not_truncated();
    test_json_booleans();
    test_a_truncated_or_trailing_literal_is_refused();
    test_a_key_inside_a_string_is_not_a_key();
}
