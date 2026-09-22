#include "wifi_manager.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "wifi_field.h"
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

/**
 * @brief Delay between a lost connection and the next attempt at it, and how that
 *        delay grows.
 *
 * Doubling from two seconds up to a minute is short enough that a router's own reboot
 * is usually still within it by the second or third try, and long enough past that not
 * to be spending airtime on a network that is not coming back soon. It never stops:
 * the alternative — giving up — would mean a stored network surviving a router outage
 * longer than this backoff does becomes a network this device has to be told about
 * again, and forgetting is not something a failed connection attempt should decide.
 */
#define RECONNECT_BACKOFF_MIN_MS 2000u
#define RECONNECT_BACKOFF_MAX_MS 60000u

/**
 * @brief Delay before the first connection attempt after a network is submitted.
 *
 * That submission is answered over the setup access point this device is about to try
 * leaving. Connecting fast enough to succeed — and tear that access point down — before
 * the response confirming the save has finished sending would turn a successful save
 * into a page the phone never gets to see.
 */
#define INITIAL_CONNECT_DELAY_MS 3000u

static char s_setup_ssid[WIFI_STORE_SSID_MAX + 1u];
static char s_setup_password[WIFI_STORE_SETUP_PASSWORD_LEN + 1u];

/** @brief Network the station role is trying to join, or empty if none is stored. */
static char s_station_ssid[WIFI_STORE_SSID_MAX + 1u];
static bool s_station_connected = false;
static char s_station_ip[16]; /* "255.255.255.255" and a terminator. */
static uint32_t s_backoff_ms = RECONNECT_BACKOFF_MIN_MS;

/**
 * @brief Whether the setup access point is currently up.
 *
 * Tracked here rather than read back from the driver because the two places that
 * change it — a connection succeeding, one failing — already know which way they are
 * changing it, and a flag they agree on is simpler than asking esp_wifi_get_mode() to
 * settle a question this code already knows the answer to.
 */
static bool s_ap_visible = true;

/** @brief Fires when a connection attempt is due — the first one, or a retry. */
static esp_timer_handle_t s_reconnect_timer;

static void reconnect_timer_callback(void *arg)
{
    (void)arg;

    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "station: %s", esp_err_to_name(err));
    }
}

/**
 * @brief Stage @p ssid and @p password as the station's target. Does not connect.
 *
 * Kept apart from actually connecting because the two callers need different timing:
 * a network read from flash at boot can be tried immediately, while one just submitted
 * through the portal cannot — see ::INITIAL_CONNECT_DELAY_MS.
 *
 * @return The driver's own answer to the new target, or ESP_ERR_INVALID_SIZE — with
 *         nothing changed at all — if either value is too long for the field the
 *         driver keeps it in. Nothing that fits wifi_store_credentials_t can fail that
 *         way, since every driver field is at least as large as the stored one feeding
 *         it; the check is for the day one of those two sizes moves and the other
 *         does not.
 */
static esp_err_t configure_station(const char *ssid, const char *password)
{
    /*
     * Filled and checked before anything else is touched. Half-applying a network —
     * disconnected from the one that worked, pointed at one the driver was never given
     * — is a worse place to refuse from than not having started.
     */
    wifi_config_t config = {0};
    if (!wifi_field_set(config.sta.ssid, sizeof(config.sta.ssid), ssid) ||
        !wifi_field_set(config.sta.password, sizeof(config.sta.password), password)) {
        /* Neither value is named here. The name would be most of the way to the
         * password on a network whose password is its name, and a log is the least
         * private thing on this device. */
        ESP_LOGE(TAG, "station: credentials do not fit the driver's configuration");
        return ESP_ERR_INVALID_SIZE;
    }

    snprintf(s_station_ssid, sizeof(s_station_ssid), "%s", ssid);
    s_station_connected = false;
    s_station_ip[0] = '\0';
    s_backoff_ms = RECONNECT_BACKOFF_MIN_MS;

    /* Harmless if the station role was not joined to anything — this only clears a
     * previous target before the new one below replaces it. */
    esp_wifi_disconnect();
    /* Drop whatever attempt — an initial one, a retry — was already waiting. */
    esp_timer_stop(s_reconnect_timer);

    const esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "station: %s", esp_err_to_name(err));
    }
    return err;
}

/**
 * @brief Arm the one reconnect timer, replacing whatever was pending.
 *
 * Stopped before it is started. esp_timer_start_once refuses a timer that is
 * already running, so without the stop the return value would be
 * ESP_ERR_INVALID_STATE half the time for a perfectly ordinary reason — and a
 * check that cries wolf is a check nobody keeps. With it, a failure here means
 * something is actually wrong.
 *
 * Both callers race, and deliberately are not locked against each other. The
 * portal's HTTP task calls this after submitting a network; the disconnect that
 * same submission causes is delivered to the Wi-Fi event task, which calls it too.
 * Holding a mutex across the esp_wifi_* calls between them is how this driver
 * would come to wait on the event task from inside a handler the event task runs.
 *
 * The race is survivable and the outcome is stated rather than hoped for: whoever
 * arms last wins, and both delays — 3 s for the initial attempt, 2 s for the first
 * backoff — end in a connection attempt. Losing the race costs at most one second
 * of the window that exists so a setup page's response can finish sending. It
 * cannot leave the station with nothing scheduled.
 *
 * @param delay_ms How long to wait before trying to connect.
 * @param reason   What to call this in the log.
 */
static void arm_reconnect(uint32_t delay_ms, const char *reason)
{
    /* An unarmed timer answers ESP_ERR_INVALID_STATE here, which is the expected
     * case and not an error. */
    esp_timer_stop(s_reconnect_timer);

    const esp_err_t err =
        esp_timer_start_once(s_reconnect_timer, (uint64_t)delay_ms * 1000u);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "station: %s, connecting to \"%s\" in %u ms", reason, s_station_ssid,
                 (unsigned)delay_ms);
        return;
    }

    /*
     * Every reconnection in this driver goes through this one timer, so nothing
     * else will bring the station back. Connecting immediately is worse than the
     * delay it skips and far better than a device that silently never joins: the
     * delay exists to let an HTTP response drain, not to make the attempt correct.
     *
     * This used to be dropped without a word, which is the reason it is worth
     * handling at all — the symptom arrives much later, as a device that did not
     * join a network, with nothing in the log pointing here.
     */
    ESP_LOGE(TAG, "station: could not schedule the attempt (%s); connecting now",
             esp_err_to_name(err));
    const esp_err_t connect_err = esp_wifi_connect();
    if (connect_err != ESP_OK) {
        ESP_LOGE(TAG, "station: %s", esp_err_to_name(connect_err));
    }
}

/**
 * @brief React to the station role losing its connection.
 *
 * Every disconnect — the first attempt failing, a router rebooting, walking out of
 * range — arrives here the same way, and is answered the same way: bring the setup
 * access point back so this device stays reachable, and try again later.
 */
static void handle_station_disconnected(const wifi_event_sta_disconnected_t *event)
{
    if (s_station_ssid[0] == '\0') {
        /* Nothing is stored, so this is not a network this device is trying to hold —
         * the station role was never asked to join anything. */
        return;
    }

    ESP_LOGW(TAG, "station: disconnected from \"%s\" (reason %d)", s_station_ssid,
             (int)event->reason);
    s_station_connected = false;

    if (!s_ap_visible) {
        const esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err == ESP_OK) {
            s_ap_visible = true;
            ESP_LOGI(TAG, "setup: access point back up while offline");
        } else {
            ESP_LOGE(TAG, "setup: %s", esp_err_to_name(err));
        }
    }

    arm_reconnect(s_backoff_ms, "retrying");
    s_backoff_ms = s_backoff_ms * 2u < RECONNECT_BACKOFF_MAX_MS ? s_backoff_ms * 2u
                                                                : RECONNECT_BACKOFF_MAX_MS;
}

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

    case WIFI_EVENT_STA_DISCONNECTED:
        handle_station_disconnected((const wifi_event_sta_disconnected_t *)data);
        break;

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

/**
 * @brief React to the station role obtaining an address.
 *
 * The only reliable sign a connection succeeded: an association can still be followed
 * by a DHCP lease that never arrives, and that failure belongs here, not to
 * WIFI_EVENT_STA_CONNECTED.
 */
static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    if (id != IP_EVENT_STA_GOT_IP) {
        return;
    }

    const ip_event_got_ip_t *event = data;
    snprintf(s_station_ip, sizeof(s_station_ip), IPSTR, IP2STR(&event->ip_info.ip));
    s_station_connected = true;
    s_backoff_ms = RECONNECT_BACKOFF_MIN_MS;

    ESP_LOGI(TAG, "station: joined \"%s\", %s", s_station_ssid, s_station_ip);

    if (s_ap_visible) {
        const esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err == ESP_OK) {
            s_ap_visible = false;
            ESP_LOGI(TAG, "setup: access point down, network reachable directly");
        } else {
            ESP_LOGE(TAG, "setup: %s", esp_err_to_name(err));
        }
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

    /* esp_wifi_scan_start() only works in WIFI_MODE_STA or WIFI_MODE_APSTA, and the
     * station control block it scans through — and a stored network later connects
     * through — is created from this netif when Wi-Fi starts below. */
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

    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event,
                                             NULL, NULL);
    if (err != ESP_OK) {
        return err;
    }

    const esp_timer_create_args_t reconnect_timer_args = {
        .callback = &reconnect_timer_callback,
        .name = "wifi_reconnect",
    };
    err = esp_timer_create(&reconnect_timer_args, &s_reconnect_timer);
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
    /* Both of these are this file's own making — a six-character prefix and four hex
     * digits, and a ten-character password — so neither can be too long for the field
     * it goes into. The bound is kept here rather than argued about at each of the two
     * places it would have to be re-argued if either ever stopped being generated. */
    if (!wifi_field_set(config.ap.ssid, sizeof(config.ap.ssid), s_setup_ssid) ||
        !wifi_field_set(config.ap.password, sizeof(config.ap.password), s_setup_password)) {
        ESP_LOGE(TAG, "setup: access point credentials do not fit the configuration");
        return ESP_ERR_INVALID_SIZE;
    }

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

    wifi_store_credentials_t stored;
    if (wifi_store_load_network(&stored)) {
        /* Nothing is answering on the setup access point yet at this point in boot —
         * the HTTP server does not exist until later in app_main — so there is no
         * in-flight response for an immediate connection attempt to race with here,
         * unlike the portal case below. A stored network that cannot be staged is
         * reported by configure_station and left at that: the setup access point is
         * up, which is where this device belongs when it has nowhere else to be. */
        if (configure_station(stored.ssid, stored.password) == ESP_OK) {
            const esp_err_t connect_err = esp_wifi_connect();
            if (connect_err != ESP_OK) {
                ESP_LOGE(TAG, "station: %s", esp_err_to_name(connect_err));
            }
        }
    }

    return ESP_OK;
}

esp_err_t wifi_manager_join(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_store_credentials_t credentials = {0};
    snprintf(credentials.ssid, sizeof(credentials.ssid), "%s", ssid);
    snprintf(credentials.password, sizeof(credentials.password), "%s",
             password != NULL ? password : "");

    const esp_err_t err = wifi_store_save_network(&credentials);
    if (err != ESP_OK) {
        return err;
    }

    /* Saved first and staged second, so a network this device was told to remember is
     * remembered even if the driver will not take it now — the same attempt is made
     * again at every boot from here on. The refusal is still returned rather than
     * swallowed: a submission that changed nothing this side of a reboot is not one to
     * answer as though it had worked. */
    const esp_err_t staged = configure_station(credentials.ssid, credentials.password);
    if (staged != ESP_OK) {
        return staged;
    }

    arm_reconnect(INITIAL_CONNECT_DELAY_MS, "network submitted");

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

/*
 * The three getters below are written from the event task (a connection succeeding or
 * failing) and from whichever task calls wifi_manager_join() (the HTTP server's), and
 * read from whichever task the caller happens to be — normally app_main's, once a
 * second. None of that is locked. The values only ever change together and only ever
 * grow more current, so the one thing a reader can see that a writer did not intend is
 * a display refresh one second late, or a network name mid-update on the one occasion
 * a redraw and a save land in the same instant — not a corrupted read, since every
 * buffer here is fixed-size and always left NUL-terminated by whichever snprintf wrote
 * it last. A mutex would buy correctness this state does not need at a cost — code the
 * display path has to run through on every single redraw — that it does not have to
 * pay.
 */

const char *wifi_manager_station_ssid(void)
{
    return s_station_ssid;
}

bool wifi_manager_station_connected(void)
{
    return s_station_connected;
}

const char *wifi_manager_station_ip(void)
{
    return s_station_ip;
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
