/**
 * @file wifi_field.h
 * @brief Filling the Wi-Fi driver's fixed-size credential fields.
 *
 * An SSID and a passphrase reach the driver as bare byte arrays — 32 bytes and 64 —
 * with no length beside them. A value shorter than its field is terminated by the
 * zeroes it is padded with; one that fills a field exactly has no terminator at all,
 * and that is the representation the driver expects rather than an oversight. ESP-IDF
 * reads a station SSID back with strnlen() bounded by the field's own size — see
 * debug_print_wifi_credentials() in components/wifi_provisioning/src/manager.c — and
 * the other 32-byte string in the same struct states the rule outright: "Strings
 * null-terminated (length < SAE_H2E_IDENTIFIER_LEN) or non-null terminated
 * (length = SAE_H2E_IDENTIFIER_LEN) are accepted."
 *
 * What no care on the driver's side can catch is a value longer than the field it is
 * copied into, because there is no length for it to disagree with: the copy has
 * already happened by the time anything reads what it wrote. That bound belongs to
 * whoever fills the field, which is what this is for.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Write @p value into the @p size bytes at @p field, zeroing the rest.
 *
 * The tail is zeroed rather than left alone because one config struct is filled again
 * for the next network, and the leftovers of a longer name behind a shorter one would
 * be read as part of it.
 *
 * @param field Destination, exactly @p size bytes of it.
 * @param size  Size of the driver's field, not of the value.
 * @param value NUL-terminated source. A value of exactly @p size bytes fills the field
 *              and is not terminated — see above for why that is an answer and not a
 *              truncation.
 *
 * @return False, with @p field left exactly as it was, when @p value is longer than
 *         the field or when either pointer is NULL. Nothing is ever written past
 *         @p field + @p size.
 */
bool wifi_field_set(uint8_t *field, size_t size, const char *value);

#ifdef __cplusplus
}
#endif
