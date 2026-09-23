/**
 * @file hub_link.h
 * @brief Tells the hub where this device is, and keeps telling it until it lands.
 *
 * The half of announcing that needs a network and a clock. What to send and when to
 * send it is ::hub_announce, which has neither and is therefore the half that can be
 * tested; this opens the socket, reads the status back, and turns the answer into one
 * of three outcomes for that schedule to act on.
 *
 * Nothing here is on the path that switches the PC. A hub that is down, misconfigured
 * or absent entirely leaves this device doing its own job over its own API, which is
 * the arrangement that existed before the hub did and the one it falls back to.
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "hub_announce.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Load the stored settings and start announcing.
 *
 * Starts the task whether or not anything is stored: settings can arrive later over
 * the provisioning page, and a task that only exists when they were present at boot
 * would mean a device that has to be rebooted after being told where its hub is.
 */
esp_err_t hub_link_start(void);

/**
 * @brief Copy the stored settings into @p out.
 *
 * The key comes back as stored. The only caller that wants it is the one that sends
 * it; the page that shows the others deliberately does not ask.
 */
void hub_link_settings(hub_settings_t *out);

/** @brief Whether a complete, valid set of settings is stored. */
bool hub_link_configured(void);

/**
 * @brief Store @p settings and announce to them.
 *
 * The schedule is reset rather than carried over, because a hub that has just been
 * named has never heard of this device — whatever the previous one had recorded says
 * nothing about what this one knows.
 *
 * @return ESP_ERR_INVALID_ARG if the settings are not ones this device will use.
 */
esp_err_t hub_link_save(const hub_settings_t *settings);

/** @brief Forget the stored settings and stop announcing. */
esp_err_t hub_link_clear(void);

/**
 * @brief One short line on how announcing is going, for a person to read.
 *
 * Never the key, and never the body. This ends up in a log and on a panel, both of
 * which get photographed and pasted into issues.
 */
const char *hub_link_status(void);

#ifdef __cplusplus
}
#endif
