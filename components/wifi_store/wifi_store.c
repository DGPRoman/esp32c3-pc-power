#include "wifi_store.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_store";

/** @brief NVS namespace holding everything this component owns. */
#define NAMESPACE "wifi"

#define KEY_SSID "sta_ssid"
#define KEY_PASSWORD "sta_pass"
#define KEY_SETUP_PASSWORD "ap_pass"

/**
 * @brief Alphabet for the generated setup password.
 *
 * Thirty-two symbols, with 0/O and 1/I/l left out. The person typing this is reading
 * it off a 0.42-inch panel and entering it on a phone keyboard, where a character
 * that could be either of two things is a failed connection they cannot diagnose.
 *
 * A power of two also means an index can be taken from random bits directly, without
 * the modulo bias that a 26- or 36-symbol alphabet would introduce.
 *
 * The size is deduced rather than written down, so that the terminator survives: an
 * array of exactly 32 fits the symbols and drops it, which gcc 15 reports and which
 * would be a read past the end the first time this met a string function.
 */
static const char ALPHABET[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";

/* The index into it is five bits wide, so a shorter alphabet would be read past and a
 * longer one would have symbols nothing can reach. */
_Static_assert(sizeof(ALPHABET) == 33u, "thirty-two symbols and a terminator");

esp_err_t wifi_store_init(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* The partition is unusable as it stands — full, or written by a newer NVS
         * format than this build understands. Erasing costs the stored credentials,
         * which costs one trip through setup mode. Refusing to boot would cost the
         * device, on hardware whose entire purpose is to be reachable. */
        ESP_LOGW(TAG, "nvs unusable (%s), erasing", esp_err_to_name(err));

        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }

    return err;
}

/**
 * @brief Read a string value into @p out, reporting whether it was there.
 */
static bool load_string(nvs_handle_t handle, const char *key, char *out, size_t size)
{
    size_t length = size;

    if (nvs_get_str(handle, key, out, &length) != ESP_OK) {
        out[0] = '\0';
        return false;
    }

    return out[0] != '\0';
}

bool wifi_store_load_network(wifi_store_credentials_t *out)
{
    nvs_handle_t handle;
    if (nvs_open(NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        /* The namespace does not exist until something is written to it, so a device
         * that has never been provisioned lands here. Not an error. */
        return false;
    }

    const bool has_ssid = load_string(handle, KEY_SSID, out->ssid, sizeof(out->ssid));

    /* An empty password is legitimate — it is how an open network is stored — so its
     * absence is not treated as failure. Only the SSID decides whether this device
     * has been told where to connect. */
    load_string(handle, KEY_PASSWORD, out->password, sizeof(out->password));

    nvs_close(handle);
    return has_ssid;
}

esp_err_t wifi_store_save_network(const wifi_store_credentials_t *credentials)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, KEY_SSID, credentials->ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_PASSWORD, credentials->password);
    }
    if (err == ESP_OK) {
        /* Both keys land in one commit, so a power cut cannot leave an SSID stored
         * against the previous network's password — a state that fails to connect and
         * gives no hint why. */
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "stored network \"%s\"", credentials->ssid);
    }

    return err;
}

esp_err_t wifi_store_clear_network(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    /* ESP_ERR_NVS_NOT_FOUND means the goal is already met, so it is not a failure. */
    err = nvs_erase_key(handle, KEY_SSID);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        const esp_err_t password_err = nvs_erase_key(handle, KEY_PASSWORD);
        if (password_err != ESP_OK && password_err != ESP_ERR_NVS_NOT_FOUND) {
            err = password_err;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

/** @brief Fill @p out with ::WIFI_STORE_SETUP_PASSWORD_LEN random characters. */
static void generate_setup_password(char *out)
{
    uint8_t bytes[WIFI_STORE_SETUP_PASSWORD_LEN];

    /*
     * esp_fill_random draws from the hardware generator, which samples on-chip noise
     * continuously. ESP-IDF only calls that output cryptographically secure while the
     * radio is running, and this necessarily runs before it: the password has to exist
     * before the access point it protects can be configured.
     *
     * For a WPA2 key on a provisioning network that is up for minutes on a home LAN,
     * that is an acceptable trade, and it is written down here rather than glossed over
     * because the same shortcut applied to a long-lived key would not be.
     */
    esp_fill_random(bytes, sizeof(bytes));

    for (size_t i = 0; i < WIFI_STORE_SETUP_PASSWORD_LEN; i++) {
        /* The alphabet has exactly 32 entries, so five bits index it without bias. */
        out[i] = ALPHABET[bytes[i] & 0x1Fu];
    }
    out[WIFI_STORE_SETUP_PASSWORD_LEN] = '\0';
}

esp_err_t wifi_store_setup_password(char *out, size_t size)
{
    if (size <= WIFI_STORE_SETUP_PASSWORD_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    if (load_string(handle, KEY_SETUP_PASSWORD, out, size)) {
        nvs_close(handle);
        return ESP_OK;
    }

    generate_setup_password(out);

    /* Persisted, so the password on the panel is the same one after a reboot. A device
     * that invented a new password on every restart would be correct and unusable. */
    err = nvs_set_str(handle, KEY_SETUP_PASSWORD, out);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err == ESP_OK) {
        /* Deliberately not logging the value. */
        ESP_LOGI(TAG, "generated a setup password");
    }

    return err;
}
