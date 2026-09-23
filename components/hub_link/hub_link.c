#include "hub_link.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <errno.h>

#include "device_auth.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "wifi_manager.h"

static const char *TAG = "hub_link";

/** @brief NVS namespace holding everything this component owns. */
#define NAMESPACE "hub"

#define KEY_ORIGIN "origin"
#define KEY_DEVICE_ID "device_id"
#define KEY_KEY "key"

/**
 * @brief How long the whole exchange gets.
 *
 * The hub gives a device five seconds when it polls one. Matching it is not a
 * requirement, but a device that waits far longer than the hub would is a device
 * holding a task open for a peer that has already given up on the other direction.
 */
#define REQUEST_TIMEOUT_MS 5000

/**
 * @brief How much of the reply is read.
 *
 * Only the status line is wanted, and it is the first thing on the wire. This is
 * generous for one, and it is a ceiling rather than an expectation: whatever is on
 * the other end does not get to make this device read an unbounded amount by
 * answering at length.
 */
#define RESPONSE_BUFFER 128

/** @brief How often the schedule is asked whether anything is owed. */
#define TICK_MS 1000

/**
 * @brief Stack for the announcing task.
 *
 * What sits on it is one request buffer under 768 bytes, one reply buffer of 128, and
 * the socket calls under them. No TLS and no HTTP client library, so nothing here
 * allocates the way one of those would.
 */
#define TASK_STACK 4096

/** @brief Below the Wi-Fi driver and the HTTP server, above the idle task. */
#define TASK_PRIORITY 4

static hub_settings_t s_settings;
static hub_announce_state_t s_state;
static SemaphoreHandle_t s_lock;

/**
 * @brief Bumped every time the settings change, so a reply can be dated.
 *
 * An announcement takes up to five seconds and the settings can be saved during it.
 * Without this, the reply from the old hub would be recorded against the schedule the
 * save had just reset — and if that reply was a 204, the schedule would then believe
 * the *new* hub already had this device's address and never announce to it. The
 * device would look configured, the new hub would never poll it, and nothing would
 * say why until the next reboot or address change.
 */
static uint32_t s_generation;

/**
 * @brief The line ::hub_link_status hands out.
 *
 * Written by the announcing task and by a save, read by whoever is logging or
 * drawing it. Deliberately outside the mutex, the same way ::wifi_manager_station_ip
 * is: a lock here would be one held across a redraw, and the worst this can produce
 * is a line that is out of date or that mixes two. vsnprintf terminates either way,
 * so what is read is always a string — just possibly the wrong one, about a status
 * that the next second's tick will restate.
 */
static char s_status[64] = "not configured";

/** @brief Milliseconds since boot, on the clock ::hub_announce_due expects. */
static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

__attribute__((format(printf, 1, 2))) static void set_status(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(s_status, sizeof(s_status), format, arguments);
    va_end(arguments);
}

/** @brief Read a stored string into @p out, emptying it when there is none. */
static void load_string(nvs_handle_t handle, const char *key, char *out, size_t size)
{
    size_t length = size;
    if (nvs_get_str(handle, key, out, &length) != ESP_OK) {
        out[0] = '\0';
    }
}

static void load_settings(void)
{
    nvs_handle_t handle;
    if (nvs_open(NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        /* No namespace yet is the ordinary state of a device nobody has configured,
         * not a fault. Everything stays empty and nothing is announced. */
        memset(&s_settings, 0, sizeof(s_settings));
        return;
    }

    load_string(handle, KEY_ORIGIN, s_settings.origin, sizeof(s_settings.origin));
    load_string(handle, KEY_DEVICE_ID, s_settings.device_id, sizeof(s_settings.device_id));
    load_string(handle, KEY_KEY, s_settings.key, sizeof(s_settings.key));
    nvs_close(handle);
}

/**
 * @brief Turn a status code into one of the three things the schedule acts on.
 *
 * The four the hub answers deliberately, plus the one it answers when this device has
 * already got the others wrong too often. All of them are somebody's configuration
 * rather than the weather, and hurrying makes the last one worse.
 *
 * Anything else — a redirect, a 5xx, or a status line from something that is not the
 * hub at all — is the device not having been recorded, with no reason to believe that
 * is permanent.
 */
static hub_result_t classify(int status)
{
    if (status >= 200 && status < 300) {
        set_status("announced");
        return HUB_RESULT_ACCEPTED;
    }

    if (status == 401 || status == 403 || status == 404 || status == 422 || status == 429) {
        set_status("refused: %d", status);
        return HUB_RESULT_REFUSED;
    }

    if (status < 0) {
        set_status("reply was not the hub's");
        return HUB_RESULT_UNREACHABLE;
    }

    set_status("unexpected: %d", status);
    return HUB_RESULT_UNREACHABLE;
}

/**
 * @brief Connect to @p target, giving up after @p timeout_ms.
 *
 * Non-blocking for the duration of the connect, then put back. A blocking connect()
 * returns when lwIP has exhausted its SYN retries, which is far longer than the
 * timeout this component claims to honour — and a claim about a timeout that the code
 * does not keep is worse than no claim.
 *
 * @return 0 on success, -1 otherwise.
 */
static int connect_within(int fd, const struct sockaddr_in *target, int timeout_ms)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }

    int result = connect(fd, (const struct sockaddr *)target, sizeof(*target));
    if (result < 0) {
        if (errno != EINPROGRESS) {
            return -1;
        }

        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(fd, &writable);

        struct timeval limit = {
            .tv_sec = timeout_ms / 1000,
            .tv_usec = (timeout_ms % 1000) * 1000,
        };

        if (select(fd + 1, NULL, &writable, NULL, &limit) <= 0) {
            return -1;
        }

        /* Writable means the connect finished, not that it succeeded. A refused
         * connection arrives here exactly like an accepted one until this is read. */
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0 || error != 0) {
            return -1;
        }
    }

    return fcntl(fd, F_SETFL, flags);
}

/** @brief Write all of @p length bytes, or report that it could not. */
static bool send_all(int fd, const char *data, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        const ssize_t written = send(fd, data + sent, length - sent, 0);
        if (written <= 0) {
            return false;
        }
        sent += (size_t)written;
    }
    return true;
}

/**
 * @brief Send one announcement and turn the answer into an outcome.
 *
 * Nothing follows a redirect, because there is nothing here that could: this reads a
 * status line and stops. That is the same position the hub takes when it polls back,
 * and for the same reason — a redirect is the far end choosing where this device's
 * next request goes, and the key would travel with it.
 */
static hub_result_t announce_once(const hub_settings_t *settings, const char *address)
{
    char host[HUB_ORIGIN_MAX + 1u];
    uint16_t port = 0;
    if (!hub_origin_split(settings->origin, host, sizeof(host), &port)) {
        set_status("settings unusable");
        return HUB_RESULT_REFUSED;
    }

    char request[HUB_REQUEST_MAX];
    const esp_app_desc_t *const description = esp_app_get_description();
    const int length =
        hub_announce_request(request, sizeof(request), settings, address, device_auth_api_key(),
                             description == NULL ? NULL : description->version);
    if (length < 0) {
        set_status("request could not be built");
        return HUB_RESULT_REFUSED;
    }

    struct sockaddr_in target;
    memset(&target, 0, sizeof(target));
    target.sin_family = AF_INET;
    target.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &target.sin_addr) != 1) {
        set_status("settings unusable");
        memset(request, 0, sizeof(request));
        return HUB_RESULT_REFUSED;
    }

    hub_result_t result = HUB_RESULT_UNREACHABLE;
    const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (fd < 0) {
        set_status("no socket: %d", errno);
    } else {
        const struct timeval limit = {
            .tv_sec = REQUEST_TIMEOUT_MS / 1000,
            .tv_usec = (REQUEST_TIMEOUT_MS % 1000) * 1000,
        };
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));

        if (connect_within(fd, &target, REQUEST_TIMEOUT_MS) < 0) {
            set_status("no answer from %s", settings->origin);
        } else if (!send_all(fd, request, (size_t)length)) {
            set_status("could not send to %s", settings->origin);
        } else {
            char reply[RESPONSE_BUFFER];
            const ssize_t read_bytes = recv(fd, reply, sizeof(reply), 0);

            if (read_bytes <= 0) {
                set_status("no reply from %s", settings->origin);
            } else {
                const int status = hub_announce_status(reply, (size_t)read_bytes);
                result = classify(status);
            }
        }

        close(fd);
    }

    /* The request carried this device's own key. Wiped rather than left on a stack
     * frame that the next thing on this task will write over piecemeal. */
    memset(request, 0, sizeof(request));
    return result;
}

static void hub_link_task(void *argument)
{
    (void)argument;

    for (;;) {
        char address[HUB_ADDRESS_MAX + 1u] = "";
        if (wifi_manager_station_connected()) {
            /* A failure here leaves the address empty, which is the same as having no
             * address at all — nothing is announced, and nothing is claimed. */
            (void)hub_announce_address(address, sizeof(address), wifi_manager_station_ip());
        }

        hub_settings_t settings;
        memset(&settings, 0, sizeof(settings));
        bool usable = false;
        uint32_t generation = 0;

        if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
            settings = s_settings;
            generation = s_generation;
            usable = hub_settings_complete(&settings) &&
                     hub_announce_due(&s_state, address, now_ms());
            xSemaphoreGive(s_lock);
        }

        if (usable) {
            ESP_LOGI(TAG, "announcing %s to %s", address, settings.origin);
            const hub_result_t result = announce_once(&settings, address);

            if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
                if (generation == s_generation) {
                    hub_announce_record(&s_state, address, result, now_ms());
                } else {
                    /* The settings changed while this was in flight, so this answer
                     * is a previous hub's and says nothing about the current one.
                     * Dropped rather than recorded — see ::s_generation. */
                    ESP_LOGI(TAG, "hub changed mid-announcement; ignoring the reply");
                }
                xSemaphoreGive(s_lock);
            }

            ESP_LOGI(TAG, "hub: %s", s_status);
        }

        /* The settings this attempt used held the hub's key. */
        memset(&settings, 0, sizeof(settings));

        /* Polled rather than woken. The schedule's shortest wait is two seconds and
         * its longest is five minutes, so a tick a second costs two string compares
         * against a budget that has a Wi-Fi driver in it — and it means a settings
         * change takes effect within a second without a notification path that has to
         * be right on every route into it. */
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

esp_err_t hub_link_start(void)
{
    if (s_lock != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    hub_announce_reset(&s_state);
    load_settings();

    if (hub_settings_complete(&s_settings)) {
        set_status("configured, not announced yet");
    } else if (s_settings.origin[0] != '\0' || s_settings.device_id[0] != '\0' ||
               s_settings.key[0] != '\0') {
        /* Something is stored and it is not usable. Saying so beats "not configured",
         * which would send whoever set it looking for a save that did happen. */
        set_status("stored settings are incomplete");
    }

    if (xTaskCreate(hub_link_task, "hub_link", TASK_STACK, NULL, TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

void hub_link_settings(hub_settings_t *out)
{
    if (out == NULL) {
        return;
    }

    if (s_lock != NULL && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        *out = s_settings;
        xSemaphoreGive(s_lock);
        return;
    }

    memset(out, 0, sizeof(*out));
}

bool hub_link_configured(void)
{
    hub_settings_t settings;
    hub_link_settings(&settings);
    return hub_settings_complete(&settings);
}

esp_err_t hub_link_save(const hub_settings_t *settings)
{
    if (!hub_settings_complete(settings)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, KEY_ORIGIN, settings->origin);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_DEVICE_ID, settings->device_id);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, KEY_KEY, settings->key);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        return err;
    }

    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        s_settings = *settings;
        /* A hub that has just been named has never heard of this device, so nothing
         * the previous one accepted says anything about what this one knows. */
        hub_announce_reset(&s_state);
        s_generation++;
        xSemaphoreGive(s_lock);
    }

    set_status("configured, not announced yet");
    ESP_LOGI(TAG, "hub set: %s as \"%s\"", settings->origin, settings->device_id);
    return ESP_OK;
}

esp_err_t hub_link_clear(void)
{
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        /* Not nvs_erase_all: the namespace is this component's, but erasing by key
         * says so explicitly and survives somebody storing something else here. */
        (void)nvs_erase_key(handle, KEY_ORIGIN);
        (void)nvs_erase_key(handle, KEY_DEVICE_ID);
        (void)nvs_erase_key(handle, KEY_KEY);
        err = nvs_commit(handle);
        nvs_close(handle);
    }

    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        memset(&s_settings, 0, sizeof(s_settings));
        hub_announce_reset(&s_state);
        s_generation++;
        xSemaphoreGive(s_lock);
    }

    set_status("not configured");
    return err;
}

const char *hub_link_status(void)
{
    return s_status;
}
