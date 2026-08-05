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

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

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
