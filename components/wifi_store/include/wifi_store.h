/**
 * @file wifi_store.h
 * @brief Wi-Fi credentials, kept in NVS and never in the firmware image.
 *
 * Nothing here is compiled in. That is what lets one build artefact be flashed to
 * any number of units, keeps a password out of `build/` and out of this repository,
 * and means moving the device to a different network needs no toolchain.
 *
 * None of these values are ever logged. A console log is the least private thing on
 * an embedded device — it goes to a terminal, into scrollback, into pasted output —
 * so the setup password reaches the user by being drawn on the panel instead.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Longest SSID 802.11 allows, in bytes. */
#define WIFI_STORE_SSID_MAX 32u

/** @brief Longest WPA2 passphrase, in characters. */
#define WIFI_STORE_PASSWORD_MAX 63u

/**
 * @brief Length of the generated setup-mode password.
 *
 * Ten characters from a 32-symbol alphabet is fifty bits, which is far past what
 * matters for an access point that exists for a few minutes at a time. The length is
 * chosen at the other end of the problem: it has to be readable off a 72-pixel-wide
 * panel and typable on a phone without a mistake.
 */
#define WIFI_STORE_SETUP_PASSWORD_LEN 10u

/** @brief One network this device can join. */
typedef struct {
    char ssid[WIFI_STORE_SSID_MAX + 1u];
    char password[WIFI_STORE_PASSWORD_MAX + 1u];
} wifi_store_credentials_t;

/**
 * @brief Open the credential store, initialising the NVS partition if needed.
 *
 * A partition left unusable by a firmware update that changed its layout is erased
 * and recreated rather than treated as fatal: losing stored credentials costs one
 * trip through setup mode, while refusing to boot costs the device.
 */
esp_err_t wifi_store_init(void);

/**
 * @brief Read the stored network, if there is one.
 *
 * @param out Filled in only when this returns true.
 * @return False when the device has never been provisioned, which is a normal state
 *         and not an error.
 */
bool wifi_store_load_network(wifi_store_credentials_t *out);

/** @brief Persist @p credentials as the network to join from now on. */
esp_err_t wifi_store_save_network(const wifi_store_credentials_t *credentials);

/** @brief Forget the stored network, sending the device back to setup mode. */
esp_err_t wifi_store_clear_network(void);

/**
 * @brief The device's setup-mode access point password, generating it if absent.
 *
 * Generated once from the hardware random number generator and kept, rather than
 * derived from the MAC address: a MAC is broadcast in the clear, so anything derived
 * from one is public. Persisting it means the password on the panel survives a reboot
 * and the user is not re-reading it every time.
 *
 * @param out  Receives a NUL-terminated password.
 * @param size Size of @p out; must exceed ::WIFI_STORE_SETUP_PASSWORD_LEN.
 */
esp_err_t wifi_store_setup_password(char *out, size_t size);

#ifdef __cplusplus
}
#endif
