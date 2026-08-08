#include "device_auth.h"

#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "device_auth";

/** @brief NVS namespace holding everything this component owns. */
#define NAMESPACE "auth"

#define KEY_API_KEY "api_key"

/**
 * @brief Alphabet the key is drawn from.
 *
 * The same thirty-two symbols wifi_store draws the setup password from, reused rather
 * than widened: this key is copied and pasted into a hub's configuration, never typed,
 * so the original reason for excluding look-alike characters no longer applies — but
 * neither does any reason to pick a second alphabet just to have one. Thirty-two
 * characters from thirty-two symbols is 160 bits, which is not the constraint on how
 * hard this key is to guess; the constraint is that it is never logged.
 */
static const char ALPHABET[32] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";

static char s_api_key[DEVICE_AUTH_API_KEY_LEN + 1u];

esp_err_t device_auth_init(void)
{
    /*
     * Not assuming some other component already brought NVS up: nvs_flash_init() is
     * safe to call more than once — the second caller just gets ESP_OK back — so this
     * component's own correctness does not depend on running after wifi_store's.
     */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs unusable (%s), erasing", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t handle;
    err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    size_t length = sizeof(s_api_key);
    err = nvs_get_str(handle, KEY_API_KEY, s_api_key, &length);
    if (err == ESP_OK) {
        nvs_close(handle);
        return ESP_OK;
    }

    if (err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return err;
    }

    /* esp_fill_random draws from the hardware generator, which — like the setup
     * password's — is not yet certified cryptographically secure this early in boot,
     * before the radio has run. The same trade applies here as there: acceptable for a
     * key generated once and then held in flash, not for one regenerated on any kind
     * of schedule. */
    uint8_t bytes[DEVICE_AUTH_API_KEY_LEN];
    esp_fill_random(bytes, sizeof(bytes));
    for (size_t i = 0; i < DEVICE_AUTH_API_KEY_LEN; i++) {
        s_api_key[i] = ALPHABET[bytes[i] & 0x1Fu];
    }
    s_api_key[DEVICE_AUTH_API_KEY_LEN] = '\0';

    err = nvs_set_str(handle, KEY_API_KEY, s_api_key);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err == ESP_OK) {
        /* Deliberately not logging the value. */
        ESP_LOGI(TAG, "generated an API key");
    }

    return err;
}

const char *device_auth_api_key(void)
{
    return s_api_key;
}

bool device_auth_verify(const char *presented, size_t presented_length)
{
    if (presented_length != strlen(s_api_key)) {
        return false;
    }

    /*
     * Every byte is compared regardless of earlier mismatches, and the result is only
     * inspected once the loop ends. A comparison that returns on the first differing
     * byte leaks, through timing, how many leading bytes an attacker already has
     * right — enough tries and the whole key comes out one byte at a time, over a
     * comparison whose result is otherwise a plain boolean.
     */
    unsigned char difference = 0;
    for (size_t i = 0; i < presented_length; i++) {
        difference |= (unsigned char)presented[i] ^ (unsigned char)s_api_key[i];
    }

    return difference == 0;
}
