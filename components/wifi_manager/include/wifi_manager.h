/**
 * @file wifi_manager.h
 * @brief Brings up Wi-Fi, and puts the device on a network without being told one at
 *        build time.
 *
 * Setup mode is how credentials arrive: the device raises its own access point,
 * protected by a password it generated and prints on its panel, and waits to be given
 * a network to join. Nothing is compiled in, so one firmware image serves every unit
 * and a change of network needs no toolchain.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Longest SSID 802.11 allows, in bytes. Matches ::WIFI_STORE_SSID_MAX. */
#define WIFI_MANAGER_SSID_MAX 32u

/** @brief Longest WPA2 passphrase. Matches ::WIFI_STORE_PASSWORD_MAX. */
#define WIFI_MANAGER_PASSWORD_MAX 63u

/**
 * @brief Networks a scan will report.
 *
 * Scan results come back sorted by signal strength, so this caps the list to the
 * strongest this many rather than an arbitrary one: the networks it drops are exactly
 * the ones least likely to hold a connection anyway.
 */
#define WIFI_MANAGER_SCAN_MAX 16u

/** @brief One network in range, as far as choosing one to join needs to know. */
typedef struct {
    char ssid[WIFI_MANAGER_SSID_MAX + 1u];
    /** Whether joining it needs a password. */
    bool secured;
} wifi_manager_network_t;

/**
 * @brief Bring up Wi-Fi.
 *
 * The setup access point always comes up. If a network is already stored, joining it
 * is attempted too, in parallel — not instead: the access point stays up until that
 * attempt succeeds, and comes back if a later one fails, so this device is never only
 * reachable through a connection that might not exist.
 */
esp_err_t wifi_manager_start(void);

/**
 * @brief Scan for nearby networks, blocking until the scan completes.
 *
 * About a second and a half: the radio visits every 2.4 GHz channel in turn, and while
 * it is on a channel other than the setup access point's own, this device's beacon
 * goes out late. The one station allowed to associate — the phone doing the
 * provisioning — tolerates a gap that short without deciding the access point is gone;
 * nothing shorter would reach every channel.
 *
 * A network answering on more than one access point — a mesh, a repeater — is reported
 * once. Which one of those access points supplied the entry is not meaningful here:
 * joining is by name, and the strongest of them was already the one kept, because the
 * driver returns results in descending signal order.
 *
 * @param out   Array of at least ::WIFI_MANAGER_SCAN_MAX entries.
 * @param count In: capacity of @p out. Out: networks written.
 */
esp_err_t wifi_manager_scan(wifi_manager_network_t *out, uint16_t *count);

/**
 * @brief Save a network, and start trying to join it.
 *
 * The attempt itself is delayed a few seconds: this call is answered over the setup
 * access point it might succeed fast enough to tear down, and a connection quicker
 * than that response would turn a successful save into a page the phone never gets to
 * see. A network already stored is replaced, not merged with — one device, one target.
 *
 * @param ssid     Network name. Must not be empty.
 * @param password Passphrase, or an empty string for an open network.
 */
esp_err_t wifi_manager_join(const char *ssid, const char *password);

/**
 * @brief SSID of the setup access point, or an empty string before it is up.
 *
 * Ends in part of the device's MAC address, so two of these on one desk are still
 * distinguishable.
 */
const char *wifi_manager_setup_ssid(void);

/**
 * @brief Password for the setup access point, or an empty string before it is up.
 *
 * For drawing on the panel. It is never logged: a console log ends up in scrollback
 * and in pasted output, which is the wrong place for the only secret that stands
 * between a stranger in radio range and this device's configuration.
 */
const char *wifi_manager_setup_password(void);

/**
 * @brief SSID this device is trying to join, or an empty string if none is stored.
 */
const char *wifi_manager_station_ssid(void);

/** @brief Whether the station role currently holds a connection and an address. */
bool wifi_manager_station_connected(void);

/**
 * @brief This device's address on the joined network, or an empty string if it is not
 *        currently connected.
 */
const char *wifi_manager_station_ip(void);

#ifdef __cplusplus
}
#endif
