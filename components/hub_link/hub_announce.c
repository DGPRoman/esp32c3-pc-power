#include "hub_announce.h"

#include <stdio.h>
#include <string.h>

/** @brief Scheme this device will announce to. See ::hub_origin_valid. */
static const char SCHEME[] = "http://";

/** @brief Path the hub serves announcements on, under the device's id. */
#define PATH_PREFIX "/v1/devices/"
#define PATH_SUFFIX "/announcements"

/** @brief Longest firmware string the hub records. Beyond it, the field is dropped. */
#define FIRMWARE_MAX 64u

/**
 * @brief Whether @p now has reached @p deadline, across the clock's wrap.
 *
 * The millisecond clock this runs on is 32 bits and wraps after 49.7 days, which is
 * an ordinary uptime for a device that lives inside a case. A plain `now >= deadline`
 * would, for the one wrap in every seven weeks, either fire every deadline at once or
 * stop firing any of them for the rest of the epoch.
 *
 * Unsigned subtraction wraps by definition, so the difference is correct across the
 * boundary; half the range is then the dividing line between "just passed" and "still
 * a long way off". That holds for any wait shorter than about 24 days, which every
 * one here is by three orders of magnitude.
 */
static bool reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (now_ms - deadline_ms) < 0x80000000u;
}

/**
 * @brief Length of @p value, stopping at @p limit rather than at its terminator.
 *
 * Not strlen(), and not strnlen() either: the latter is POSIX, and these sources are
 * compiled as strict C11 on the host, where asking for it would be an implicit
 * declaration rather than the function. Every caller here is bounding a value that
 * arrived from outside, so stopping is the point.
 *
 * @return The length, or @p limit + 1 if the terminator is not within @p limit.
 */
static size_t bounded_length(const char *value, size_t limit)
{
    size_t length = 0;
    while (length <= limit && value[length] != '\0') {
        length++;
    }
    return length;
}

/** @brief Copy @p value into @p field, or empty it if it does not fit. */
static void copy_bounded(char *field, size_t size, const char *value)
{
    if (value == NULL) {
        field[0] = '\0';
        return;
    }

    const size_t length = bounded_length(value, size - 1u);
    if (length >= size) {
        field[0] = '\0';
        return;
    }

    memcpy(field, value, length + 1u);
}

/**
 * @brief Read one dotted-quad octet, advancing @p cursor past it.
 *
 * Leading zeros are refused rather than skipped. "010" is ten here and eight to
 * anything that reads it as octal, and an address that means two things is one this
 * device should not be building a request from.
 */
static bool take_octet(const char **cursor)
{
    const char *at = *cursor;
    unsigned value = 0;
    unsigned digits = 0;

    while (at[digits] >= '0' && at[digits] <= '9') {
        value = (value * 10u) + (unsigned)(at[digits] - '0');
        digits++;
        if (digits > 3u) {
            return false;
        }
    }

    if (digits == 0u || value > 255u) {
        return false;
    }
    if (digits > 1u && at[0] == '0') {
        return false;
    }

    *cursor = at + digits;
    return true;
}

/** @brief Whether @p text is exactly a dotted-quad IPv4 address. */
static bool is_dotted_quad(const char *text)
{
    if (text == NULL) {
        return false;
    }

    const char *cursor = text;
    for (unsigned octet = 0; octet < 4u; octet++) {
        if (octet > 0u) {
            if (*cursor != '.') {
                return false;
            }
            cursor++;
        }
        if (!take_octet(&cursor)) {
            return false;
        }
    }

    return *cursor == '\0';
}

/** @brief Whether @p text is a port the TCP header can carry: 1 to 65535. */
static bool is_port(const char *text)
{
    unsigned value = 0;
    unsigned digits = 0;

    while (text[digits] >= '0' && text[digits] <= '9') {
        value = (value * 10u) + (unsigned)(text[digits] - '0');
        digits++;
        if (digits > 5u) {
            return false;
        }
    }

    return digits > 0u && text[digits] == '\0' && value > 0u && value <= 65535u &&
           !(digits > 1u && text[0] == '0');
}

bool hub_origin_valid(const char *origin)
{
    if (origin == NULL) {
        return false;
    }

    const size_t length = bounded_length(origin, HUB_ORIGIN_MAX + 1u);
    if (length == 0u || length > HUB_ORIGIN_MAX) {
        return false;
    }
    if (strncmp(origin, SCHEME, sizeof(SCHEME) - 1u) != 0) {
        return false;
    }

    /* Copied so the host and the port can be terminated separately. Bounded by the
     * length check above, which is what makes this array enough. */
    char rest[HUB_ORIGIN_MAX + 1u];
    copy_bounded(rest, sizeof(rest), origin + (sizeof(SCHEME) - 1u));

    char *const colon = strchr(rest, ':');
    if (colon != NULL) {
        *colon = '\0';
        if (!is_port(colon + 1)) {
            return false;
        }
    }

    return is_dotted_quad(rest);
}

bool hub_device_id_valid(const char *id)
{
    if (id == NULL) {
        return false;
    }

    const size_t length = bounded_length(id, HUB_DEVICE_ID_MAX + 1u);
    if (length == 0u || length > HUB_DEVICE_ID_MAX) {
        return false;
    }

    /* The hub's own pattern: lower-case alphanumeric runs joined by single hyphens,
     * with no hyphen at either end. Written out rather than approximated, because an
     * id this accepts and the hub does not is a 422 nobody can see the cause of. */
    bool previous_was_hyphen = true; /* Nothing before the first character. */
    for (size_t at = 0; at < length; at++) {
        const char character = id[at];
        const bool alphanumeric = (character >= 'a' && character <= 'z') ||
                                  (character >= '0' && character <= '9');

        if (character == '-') {
            if (previous_was_hyphen) {
                return false;
            }
            previous_was_hyphen = true;
        } else if (alphanumeric) {
            previous_was_hyphen = false;
        } else {
            return false;
        }
    }

    return !previous_was_hyphen;
}

bool hub_key_valid(const char *key)
{
    if (key == NULL) {
        return false;
    }

    const size_t length = bounded_length(key, HUB_KEY_MAX + 1u);
    if (length < HUB_KEY_MIN || length > HUB_KEY_MAX) {
        return false;
    }

    for (size_t at = 0; at < length; at++) {
        const unsigned char character = (unsigned char)key[at];
        /* Printable ASCII, space excluded. See the header: this ends up in a header
         * value, where a CR or LF is not a strange character but a second header. */
        if (character <= 0x20u || character >= 0x7Fu) {
            return false;
        }
    }

    return true;
}

bool hub_settings_complete(const hub_settings_t *settings)
{
    return settings != NULL && hub_origin_valid(settings->origin) &&
           hub_device_id_valid(settings->device_id) && hub_key_valid(settings->key);
}

int hub_announce_address(char *out, size_t size, const char *ip)
{
    if (out == NULL || size == 0u) {
        return -1;
    }
    out[0] = '\0';

    if (!is_dotted_quad(ip)) {
        return -1;
    }

    const int written = snprintf(out, size, "%s%s", SCHEME, ip);
    if (written < 0 || (size_t)written >= size) {
        out[0] = '\0';
        return -1;
    }

    return written;
}

int hub_announce_url(char *out, size_t size, const hub_settings_t *settings)
{
    if (out == NULL || size == 0u) {
        return -1;
    }
    out[0] = '\0';

    if (!hub_settings_complete(settings)) {
        return -1;
    }

    const int written = snprintf(out, size, "%s" PATH_PREFIX "%s" PATH_SUFFIX, settings->origin,
                                 settings->device_id);
    if (written < 0 || (size_t)written >= size) {
        out[0] = '\0';
        return -1;
    }

    return written;
}

/**
 * @brief Whether @p text can go inside a JSON string with no escaping at all.
 *
 * Not an escaper. Everything this device puts in a body is either something it built
 * itself or something it validated on the way in, so a value needing an escape is a
 * value that should not have got this far — and a hand-written escaper is a thing to
 * get wrong for no benefit.
 */
static bool json_safe(const char *text, size_t limit)
{
    if (text == NULL) {
        return false;
    }

    const size_t length = bounded_length(text, limit + 1u);
    if (length == 0u || length > limit) {
        return false;
    }

    for (size_t at = 0; at < length; at++) {
        const unsigned char character = (unsigned char)text[at];
        if (character < 0x20u || character >= 0x7Fu || character == '"' || character == '\\') {
            return false;
        }
    }

    return true;
}

int hub_announce_body(char *out, size_t size, const char *address, const char *api_key,
                      const char *firmware)
{
    if (out == NULL || size == 0u) {
        return -1;
    }
    out[0] = '\0';

    if (!json_safe(address, HUB_ADDRESS_MAX) || !json_safe(api_key, HUB_KEY_MAX)) {
        return -1;
    }

    /* Optional to the hub, so optional here. A version string this cannot put in a
     * body unescaped is dropped rather than allowed to fail the announcement: the
     * address and the key are what the hub needs, and the version is something it
     * records without ever interpreting. */
    const bool with_firmware = json_safe(firmware, FIRMWARE_MAX);

    const int written =
        with_firmware
            ? snprintf(out, size, "{\"address\":\"%s\",\"api_key\":\"%s\",\"firmware\":\"%s\"}",
                       address, api_key, firmware)
            : snprintf(out, size, "{\"address\":\"%s\",\"api_key\":\"%s\"}", address, api_key);

    if (written < 0 || (size_t)written >= size) {
        out[0] = '\0';
        return -1;
    }

    return written;
}

bool hub_origin_split(const char *origin, char *host, size_t host_size, uint16_t *port)
{
    if (host == NULL || host_size == 0u || port == NULL) {
        return false;
    }
    host[0] = '\0';

    if (!hub_origin_valid(origin)) {
        return false;
    }

    const char *const authority = origin + (sizeof(SCHEME) - 1u);
    const char *const colon = strchr(authority, ':');

    if (colon == NULL) {
        copy_bounded(host, host_size, authority);
        *port = 80u;
        return host[0] != '\0';
    }

    const size_t length = (size_t)(colon - authority);
    if (length >= host_size) {
        return false;
    }
    memcpy(host, authority, length);
    host[length] = '\0';

    unsigned value = 0;
    for (const char *digit = colon + 1; *digit != '\0'; digit++) {
        value = (value * 10u) + (unsigned)(*digit - '0');
    }
    *port = (uint16_t)value;
    return true;
}

int hub_announce_request(char *out, size_t size, const hub_settings_t *settings,
                         const char *address, const char *api_key, const char *firmware)
{
    if (out == NULL || size == 0u) {
        return -1;
    }
    out[0] = '\0';

    if (!hub_settings_complete(settings)) {
        return -1;
    }

    char body[HUB_ADDRESS_MAX + HUB_KEY_MAX + FIRMWARE_MAX + 64u];
    const int body_length = hub_announce_body(body, sizeof(body), address, api_key, firmware);
    if (body_length < 0) {
        return -1;
    }

    /* The authority exactly as it was given, port and all. The hub reads Host only to
     * route, and an origin that said :5000 is one whose Host says :5000. */
    const char *const authority = settings->origin + (sizeof(SCHEME) - 1u);

    const int written =
        snprintf(out, size,
                 "POST " PATH_PREFIX "%s" PATH_SUFFIX " HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "X-API-Key: %s\r\n"
                 "Content-Type: application/json\r\n"
                 "Content-Length: %d\r\n"
                 "Connection: close\r\n"
                 "\r\n"
                 "%s",
                 settings->device_id, authority, settings->key, body_length, body);

    /* The body held this device's key. It is about to be a dead stack frame. */
    memset(body, 0, sizeof(body));

    if (written < 0 || (size_t)written >= size) {
        memset(out, 0, size);
        return -1;
    }

    return written;
}

int hub_announce_status(const char *response, size_t length)
{
    /* "HTTP/1.1 204" is twelve characters, and nothing shorter can carry a code. */
    if (response == NULL || length < 12u) {
        return -1;
    }
    if (strncmp(response, "HTTP/1.", 7u) != 0) {
        return -1;
    }
    if (response[7] < '0' || response[7] > '9' || response[8] != ' ') {
        return -1;
    }

    int code = 0;
    for (size_t at = 9u; at < 12u; at++) {
        if (response[at] < '0' || response[at] > '9') {
            return -1;
        }
        code = (code * 10) + (response[at] - '0');
    }

    /* A status line ends the code with a space or with the line itself. Anything else
     * means the three digits were the start of something longer. */
    if (length > 12u && response[12] != ' ' && response[12] != '\r' && response[12] != '\n') {
        return -1;
    }

    return code;
}

void hub_announce_reset(hub_announce_state_t *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

bool hub_announce_due(const hub_announce_state_t *state, const char *address, uint32_t now_ms)
{
    if (state == NULL || address == NULL || address[0] == '\0') {
        return false;
    }

    /* The hub has this one. Nothing is owed — and sending anyway would clear the
     * record its poller has been keeping, which is the thing worth not doing. */
    if (strcmp(address, state->announced) == 0) {
        return false;
    }

    /* A different address from the one that last failed. The wait was earned by an
     * address nobody is asking about now, so it does not apply to this one. */
    if (strcmp(address, state->attempted) != 0) {
        return true;
    }

    return reached(now_ms, state->next_attempt_ms);
}

void hub_announce_record(hub_announce_state_t *state, const char *address, hub_result_t result,
                         uint32_t now_ms)
{
    if (state == NULL || address == NULL) {
        return;
    }

    copy_bounded(state->attempted, sizeof(state->attempted), address);

    switch (result) {
    case HUB_RESULT_ACCEPTED:
        copy_bounded(state->announced, sizeof(state->announced), address);
        state->backoff_ms = 0;
        state->next_attempt_ms = now_ms;
        return;

    case HUB_RESULT_REFUSED:
        state->backoff_ms = HUB_ANNOUNCE_REFUSED_MS;
        state->next_attempt_ms = now_ms + HUB_ANNOUNCE_REFUSED_MS;
        return;

    case HUB_RESULT_UNREACHABLE:
    default:
        state->backoff_ms = (state->backoff_ms == 0u)
                                ? HUB_ANNOUNCE_FIRST_RETRY_MS
                                : ((state->backoff_ms >= HUB_ANNOUNCE_MAX_RETRY_MS / 2u)
                                       ? HUB_ANNOUNCE_MAX_RETRY_MS
                                       : state->backoff_ms * 2u);
        state->next_attempt_ms = now_ms + state->backoff_ms;
        return;
    }
}
