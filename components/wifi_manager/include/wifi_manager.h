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
 * @brief Bring up Wi-Fi in setup mode.
 *
 * There is deliberately no second path yet. Joining a stored network needs a stored
 * network, and until the provisioning portal exists nothing can write one — so a
 * branch on the store's contents would be a branch that cannot be taken, and a state
 * machine with one reachable state is a state machine written too early.
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

#ifdef __cplusplus
}
#endif
