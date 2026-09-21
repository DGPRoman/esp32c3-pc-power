/**
 * @file http_fields.c
 * @brief Reading one named value out of a request body. See http_fields.h.
 */

#include "http_fields.h"

#include <string.h>

/** @brief Value of one hex digit, or -1 if @p c is not one. */
static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/** @brief True for the whitespace JSON allows between tokens. */
static bool is_json_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/**
 * @brief Find the raw, still-encoded value of field @p name in @p body.
 *
 * @param value_length Receives the raw value's length.
 * @return Pointer to the value within @p body, or NULL if the field is absent.
 */
static const char *find_field(const char *body, const char *name, size_t *value_length)
{
    const size_t name_length = strlen(name);
    const char *field = body;

    /* Every value this loop considers a candidate is the start of a field: the first
     * iteration by definition, and every later one because the previous iteration only
     * advances to just past an '&'. */
    while (field != NULL) {
        if (strncmp(field, name, name_length) == 0 && field[name_length] == '=') {
            const char *value = field + name_length + 1u;
            const char *end = strchr(value, '&');
            *value_length = end != NULL ? (size_t)(end - value) : strlen(value);
            return value;
        }

        field = strchr(field, '&');
        if (field != NULL) {
            field++;
        }
    }

    return NULL;
}

/**
 * @brief Decode @p value_length bytes of a form value into @p out.
 *
 * '+' stands for a literal space in this encoding — unlike a URL's own query string,
 * where it does not — and every other reserved or non-ASCII byte arrives as %XX. A
 * percent not followed by two hex digits is treated as a malformed request rather than
 * copied through.
 *
 * @return True if @p value decoded into @p out without truncation.
 */
static bool url_decode(const char *value, size_t value_length, char *out, size_t out_size)
{
    size_t in = 0;
    size_t pos = 0;

    while (in < value_length) {
        if (pos + 1u >= out_size) {
            return false;
        }

        if (value[in] == '+') {
            out[pos++] = ' ';
            in++;
        } else if (value[in] == '%') {
            if (in + 2u >= value_length) {
                return false;
            }
            const int high = hex_digit(value[in + 1u]);
            const int low = hex_digit(value[in + 2u]);
            if (high < 0 || low < 0) {
                return false;
            }
            const char decoded = (char)((high << 4) | low);
            if (decoded == '\0') {
                /* %00. Everything downstream of here is a C string, so this byte
                 * would not be stored — it would end the value, wherever it landed,
                 * and the submitter would be told the credential was saved. A
                 * password silently cut in half is worse than one refused. */
                return false;
            }
            out[pos++] = decoded;
            in += 3u;
        } else {
            out[pos++] = value[in];
            in++;
        }
    }

    out[pos] = '\0';
    return true;
}

http_field_result_t http_fields_form_value(const char *body, const char *name, char *out,
                                           size_t out_size)
{
    if (body == NULL || name == NULL || out == NULL || out_size == 0u) {
        return HTTP_FIELD_INVALID;
    }

    size_t raw_length = 0;
    const char *raw = find_field(body, name, &raw_length);
    if (raw == NULL) {
        return HTTP_FIELD_ABSENT;
    }

    if (!url_decode(raw, raw_length, out, out_size)) {
        out[0] = '\0';
        return HTTP_FIELD_INVALID;
    }
    return HTTP_FIELD_OK;
}

/**
 * @brief Find the colon of the member named @p name, or NULL.
 *
 * Walks the body tracking whether it is inside a string, so that a key is only
 * recognised where a key can be. The previous version searched for the quoted name
 * with strstr, which matched one written inside another member's string value: a
 * body carrying {"note":"\"on\":true","on":false} was read as true.
 */
static const char *find_key(const char *body, const char *name)
{
    const size_t name_length = strlen(name);
    const char *start = NULL;
    bool in_string = false;

    for (const char *at = body; *at != '\0'; at++) {
        if (!in_string) {
            if (*at == '"') {
                in_string = true;
                start = at + 1;
            }
            continue;
        }

        if (*at == '\\') {
            /* An escaped byte neither ends the string nor is examined. */
            if (at[1] == '\0') {
                return NULL;
            }
            at++;
            continue;
        }

        if (*at != '"') {
            continue;
        }

        in_string = false;
        const size_t length = (size_t)(at - start);

        /* A string is a key exactly when a colon follows it. */
        const char *after = at + 1;
        while (is_json_space(*after)) {
            after++;
        }
        if (*after != ':') {
            continue;
        }
        if (length == name_length && strncmp(start, name, name_length) == 0) {
            return after;
        }
    }

    return NULL;
}

/** @brief True where a bare JSON literal is allowed to end. */
static bool literal_ends(char c)
{
    return c == '\0' || c == ',' || c == '}' || c == ']' || is_json_space(c);
}

bool http_fields_json_bool(const char *body, const char *name, bool *out)
{
    if (body == NULL || name == NULL || out == NULL) {
        return false;
    }

    const char *colon = find_key(body, name);
    if (colon == NULL) {
        return false;
    }

    const char *value = colon + 1;
    while (is_json_space(*value)) {
        value++;
    }

    /* The terminator matters as much as the literal. Without it "truthy" and
     * "true_nonsense" both begin with "true" and were accepted as it. */
    if (strncmp(value, "true", 4) == 0 && literal_ends(value[4])) {
        *out = true;
        return true;
    }
    if (strncmp(value, "false", 5) == 0 && literal_ends(value[5])) {
        *out = false;
        return true;
    }

    return false;
}
