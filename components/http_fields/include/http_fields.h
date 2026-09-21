/**
 * @file http_fields.h
 * @brief Reading one named value out of a request body.
 *
 * Form values and JSON booleans, for the two bodies this device accepts. Split out
 * of main.c because it is where the network's input first becomes this device's
 * data — and because in main.c nothing could reach it. There is no ESP-IDF here on
 * purpose: this builds and runs on a host, and everything below is covered by
 * test/host/test_http_fields.c.
 *
 * Neither reader guesses. A body that is nearly right is refused rather than
 * partly honoured: what a lenient parser lets pass is what ends up in NVS.
 */

#ifndef HTTP_FIELDS_H
#define HTTP_FIELDS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Outcome of looking for one field. */
typedef enum {
    /** The field was there and its value is in the caller's buffer. */
    HTTP_FIELD_OK = 0,
    /** No field of that name. */
    HTTP_FIELD_ABSENT,
    /**
     * The field was there and its value cannot be used: a broken percent escape,
     * an encoded NUL, or more bytes than the caller has room for.
     *
     * Separate from ::HTTP_FIELD_ABSENT because the two mean different things to a
     * caller — one is a request that omitted something optional, the other is a
     * request that is wrong.
     */
    HTTP_FIELD_INVALID,
} http_field_result_t;

/**
 * @brief Decode the value of form field @p name from an
 *        application/x-www-form-urlencoded @p body into @p out.
 *
 * @param body     NUL-terminated request body.
 * @param name     Field name to look for.
 * @param out      Receives the decoded, NUL-terminated value.
 * @param out_size Size of @p out, terminator included. Must be at least 1.
 */
http_field_result_t http_fields_form_value(const char *body, const char *name, char *out,
                                           size_t out_size);

/**
 * @brief Read boolean field @p name from a JSON @p body shaped like {"name":true}.
 *
 * Not a JSON parser. It finds a string that is used as a key — one followed by a
 * colon, and not itself inside another string — and accepts exactly the token true
 * or false where the value belongs. Anything else is for the caller to refuse: a
 * hub sending something else is sending the wrong request, not one this device
 * should honour part of.
 *
 * @return True when the field was found and carried a boolean.
 */
bool http_fields_json_bool(const char *body, const char *name, bool *out);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_FIELDS_H */
