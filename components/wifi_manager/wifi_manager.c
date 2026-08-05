#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "wifi_store.h"

/* Not "wifi": that is the tag the Wi-Fi driver itself logs under, and two
 * components sharing one tag makes a boot log impossible to attribute. */
static const char *TAG = "wifi_mgr";

/**
 * @brief Prefix of the setup access point's name.
 *
 * Short, because the MAC suffix and the whole SSID both have to fit on a panel twelve
 * characters wide, and an SSID the user cannot read is an SSID they cannot join.
 */
#define SETUP_SSID_PREFIX "PCPWR-"

/**
 * @brief Channel the setup access point uses.
 *
 * One of the three non-overlapping 2.4 GHz channels. Which of them hardly matters for
 * an interface that exists to carry one form submission, and picking a fixed one keeps
 * setup mode deterministic.
 */
#define SETUP_CHANNEL 1

/**
 * @brief Stations allowed to associate with the setup access point.
 *
 * One. Provisioning is a single person with a single phone, and a lower limit is one
 * less thing for someone else in radio range to do.
 */
#define SETUP_MAX_STATIONS 1

static char s_setup_ssid[WIFI_STORE_SSID_MAX + 1u];
static char s_setup_password[WIFI_STORE_SETUP_PASSWORD_LEN + 1u];

/**
 * @brief Log the driver's own view of setup mode.
 *
 * Association events are the only evidence available that the access point is real:
 * the beacon is invisible from this side of the radio, so without these a failure to
 * connect gives no way to tell a wrong password from an AP that never came up.
 */
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    switch (id) {
    case WIFI_EVENT_AP_START:
        ESP_LOGI(TAG, "setup: access point \"%s\" up on channel %d", s_setup_ssid,
                 SETUP_CHANNEL);
        break;

    case WIFI_EVENT_AP_STOP:
        ESP_LOGI(TAG, "setup: access point down");
        break;

    case WIFI_EVENT_AP_STACONNECTED: {
        const wifi_event_ap_staconnected_t *event = data;
        ESP_LOGI(TAG, "setup: " MACSTR " joined", MAC2STR(event->mac));
        break;
    }

    case WIFI_EVENT_AP_STADISCONNECTED: {
        const wifi_event_ap_stadisconnected_t *event = data;
        ESP_LOGI(TAG, "setup: " MACSTR " left", MAC2STR(event->mac));
        break;
    }

    default:
        /*
         * At debug level, not info. This existed to tell "no such event arrived" from
         * "the handler never ran", which it did — and the answer was that the handler
         * was fine and its log tag was being filtered. Past that it is one line of noise
         * per boot carrying a number that cannot be turned back into a name: wifi_event_t
         * is built with preprocessor conditionals, so which enumerator a given id refers
         * to depends on what the target supports. Raise the level when the question comes
         * up again rather than paying for the answer every boot.
         */
        ESP_LOGD(TAG, "setup: unhandled wifi event %ld", (long)id);
        break;
    }
}

/** @brief Build the setup SSID from the access point's own MAC address. */
static esp_err_t build_setup_ssid(void)
{
    uint8_t mac[6];
    const esp_err_t err = esp_wifi_get_mac(WIFI_IF_AP, mac);
    if (err != ESP_OK) {
        return err;
    }

    /* The last two bytes are enough to tell two of these apart, and short enough to
     * read off the panel. The full address is in the log for anything that needs it. */
    snprintf(s_setup_ssid, sizeof(s_setup_ssid), SETUP_SSID_PREFIX "%02X%02X", mac[4],
             mac[5]);

    return ESP_OK;
}

esp_err_t wifi_manager_start(void)
{
    esp_err_t err = wifi_store_init();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK) {
        return err;
    }

    /* Creates the interface and, with it, the DHCP server that hands the phone an
     * address on 192.168.4.0/24 with the device at 192.168.4.1. Without a DHCP server
     * the access point associates and then does nothing, which looks like a firmware
     * fault and is not one. */
    if (esp_netif_create_default_wifi_ap() == NULL) {
        return ESP_FAIL;
    }

    /* Nothing is joined through this interface yet — there is no stored network, and
     * won't be until the portal can write one — but esp_wifi_scan_start() only works in
     * WIFI_MODE_STA or WIFI_MODE_APSTA, and the station control block it scans through
     * is created from this netif when Wi-Fi starts below. */
    if (esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_FAIL;
    }

    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK) {
        return err;
    }

    /*
     * Keep the driver's own configuration in RAM. By default esp_wifi mirrors whatever
     * it is given into its own NVS namespace, which would leave two copies of the
     * network credentials on flash: ours, and one written behind our back that nothing
     * in this firmware reads, clears, or knows the lifetime of. One owner of a secret
     * is a requirement, not a preference.
     */
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             &on_wifi_event, NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }

    err = build_setup_ssid();
    if (err != ESP_OK) {
        return err;
    }

    err = wifi_store_setup_password(s_setup_password, sizeof(s_setup_password));
    if (err != ESP_OK) {
        return err;
    }

    wifi_config_t config = {
        .ap = {
            .ssid_len = (uint8_t)strlen(s_setup_ssid),
            .channel = SETUP_CHANNEL,
            .max_connection = SETUP_MAX_STATIONS,
            /* WPA2 rather than an open network. An open provisioning AP lets anyone in
             * radio range hand this device a network of their choosing, and the form it
             * serves is the device's entire configuration surface. */
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    memcpy(config.ap.ssid, s_setup_ssid, strlen(s_setup_ssid));
    memcpy(config.ap.password, s_setup_password, strlen(s_setup_password));

    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_set_config(WIFI_IF_AP, &config);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }

    /* No URL in this message yet: nothing is listening on 192.168.4.1 until the
     * provisioning server exists, and a log line that names an address which refuses
     * connections sends whoever reads it looking for the wrong fault. */
    ESP_LOGI(TAG, "setup: \"%s\" ready, password is on the panel", s_setup_ssid);

    return ESP_OK;
}

const char *wifi_manager_setup_ssid(void)
{
    return s_setup_ssid;
}

const char *wifi_manager_setup_password(void)
{
    return s_setup_password;
}

/**
 * @brief True if @p ssid already appears among the first @p count entries of @p out.
 *
 * A network reachable through more than one access point — a mesh, a repeater — is one
 * choice to the person provisioning, not several identical-looking rows.
 */
static bool already_listed(const wifi_manager_network_t *out, uint16_t count,
                           const char *ssid)
{
    for (uint16_t i = 0; i < count; i++) {
        if (strcmp(out[i].ssid, ssid) == 0) {
            return true;
        }
    }
    return false;
}

esp_err_t wifi_manager_scan(wifi_manager_network_t *out, uint16_t *count)
{
    const uint16_t capacity = *count;
    *count = 0;

    if (capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const wifi_scan_config_t config = {
        .show_hidden = false,
    };

    esp_err_t err = esp_wifi_scan_start(&config, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan: %s", esp_err_to_name(err));
        return err;
    }

    /*
     * Static, and zeroed before use, for the same reason as everywhere else a buffer
     * this size appears: it does not belong on a task's stack. The zeroing matters on
     * its own — an SSID is up to 32 arbitrary octets in a 33-byte field, and nothing in
     * the driver's contract promises the 33rd byte is a terminator. ESP-IDF's own scan
     * example zeroes this array before the scan and then reads every ssid field as a C
     * string, so this does the same rather than trust a guarantee that is not written
     * down anywhere.
     */
    static wifi_ap_record_t s_records[WIFI_MANAGER_SCAN_MAX];
    memset(s_records, 0, sizeof(s_records));

    uint16_t fetched = capacity < WIFI_MANAGER_SCAN_MAX ? capacity : WIFI_MANAGER_SCAN_MAX;
    err = esp_wifi_scan_get_ap_records(&fetched, s_records);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t written = 0;
    for (uint16_t i = 0; i < fetched && written < capacity; i++) {
        const char *ssid = (const char *)s_records[i].ssid;

        if (ssid[0] == '\0' || already_listed(out, written, ssid)) {
            continue;
        }

        snprintf(out[written].ssid, sizeof(out[written].ssid), "%s", ssid);
        out[written].secured = s_records[i].authmode != WIFI_AUTH_OPEN;
        written++;
    }

    *count = written;
    ESP_LOGI(TAG, "scan: %u network(s) in range, %u shown", (unsigned)fetched,
             (unsigned)written);
    return ESP_OK;
}
