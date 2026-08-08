/**
 * @file device_auth.h
 * @brief The secret that tells this device's HTTP command endpoints apart from an
 *        arbitrary request on the same network.
 *
 * The setup access point's password answers "is this the right device" — WPA2 keeps
 * everyone else off the network it protects. Once this device joins a home network,
 * anything else on that network can reach its HTTP server too, and the question
 * becomes "is this request from the hub" instead. A single generated key, checked on
 * every command, is the answer to that question.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Length of the generated API key, in characters. */
#define DEVICE_AUTH_API_KEY_LEN 32u

/**
 * @brief Open the credential store and make sure a key exists, generating one if not.
 *
 * A partition left unusable by a firmware update that changed its layout is erased and
 * recreated rather than treated as fatal, for the same reason wifi_store does the same:
 * losing a generated key costs one round of copying a new one into the hub's
 * configuration, while refusing to boot costs the device.
 */
esp_err_t device_auth_init(void);

/**
 * @brief This device's API key.
 *
 * Generated once, on the device, the first time it is needed, and persisted — not
 * supplied by whoever configures the hub, the way the hub's own relay API key is.
 * Nothing about setting up a hub gives it a trustworthy way to hand this device a
 * secret before there is a channel between them to hand it over on; generating the key
 * here and reading it back off the setup page, the same way the Wi-Fi password is
 * read, needs nothing to already be secure.
 */
const char *device_auth_api_key(void);

/**
 * @brief Whether @p presented is this device's API key.
 *
 * Compared in constant time and only after confirming the lengths match, matching the
 * hub's own choice of secrets.compare_digest for the same job: a comparison that
 * returns faster for a wrong first byte than for a wrong last byte is a channel a
 * network attacker can measure, one byte at a time.
 *
 * @param presented        Value to check. Not expected to be NUL-terminated — it comes
 *                          straight out of an HTTP header — so @p presented_length is
 *                          required rather than inferred with strlen().
 * @param presented_length Length of @p presented, in bytes.
 */
bool device_auth_verify(const char *presented, size_t presented_length);

#ifdef __cplusplus
}
#endif
