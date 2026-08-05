/**
 * @file main.c
 * @brief Firmware entry point.
 *
 * `app_main` is not C's `main`. By the time it runs, ESP-IDF's startup code has
 * already run the second-stage bootloader, initialised the heap, and started the
 * FreeRTOS scheduler; `app_main` is then spawned as an ordinary task. Returning
 * from it would delete only that task and leave the scheduler running — which is
 * also why the heartbeat below can simply loop here instead of needing a task of
 * its own. It moves into one as soon as there is a second thing to do at the same
 * time.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <string.h>

#include "board.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_server.h"
#include "hw_gpio.h"
#include "hw_i2c.h"
#include "ssd1306.h"
#include "wifi_manager.h"

/** @brief Port the provisioning page is served on. */
#define HTTP_PORT 80u

/** @brief Address the access point answers on, fixed by esp_netif's default AP config. */
#define SETUP_ADDRESS "192.168.4.1"

/**
 * @brief Bytes reserved per rendered network in the list below.
 *
 * An SSID is at most 32 octets, and HTML-escaping the worst case — every one of them a
 * character that expands to an entity — multiplies that by up to five. Plus a fixed
 * allowance for the surrounding markup and the "(open)" suffix: generous enough that
 * the per-item snprintf below never truncates in practice, while the check that
 * follows it means nothing breaks on the day it does.
 */
#define NETWORK_ITEM_BUDGET ((WIFI_MANAGER_SSID_MAX * 5u) + 32u)

static const char *TAG = "boot";

/*
 * Room for every network a scan can return, at the per-item budget above. Static, not
 * a stack local: it is built inside the HTTP server's request handler, which runs on a
 * task with a 4 KiB stack, and a buffer this size on that stack is the exact fault this
 * codebase has already been careful to avoid everywhere else a response is assembled.
 */
static char s_network_list[WIFI_MANAGER_SCAN_MAX * NETWORK_ITEM_BUDGET];

/*
 * A brief flash on a long period rather than an even blink. It reads as a
 * heartbeat at a glance, spends almost no time lit inside a closed case, and
 * leaves the *shape* of the pattern free to carry meaning later: a lost Wi-Fi
 * link or a command in flight can change the pattern without changing the rate.
 * The FreeRTOS tick is 100 Hz by default, so intervals shorter than 10 ms would
 * round away — these are chosen to be well clear of that.
 */
enum {
    HEARTBEAT_LIT_MS = 50,
    HEARTBEAT_DARK_MS = 950,
};

/**
 * @brief Name for @p reason, or NULL if this build has no name for it.
 *
 * Worth logging on every boot: this device's whole job is to be reachable when
 * the machine it controls is off, so an unexplained restart is a fault rather
 * than noise. The reset reason separates a power cut from a panic, a watchdog
 * timeout, or a deliberate reboot, and it is the one diagnostic that survives
 * having no debugger attached. ESP-IDF ships no helper for it.
 *
 * There is deliberately no `default` case. The enum grows between IDF versions —
 * five of the values below did not exist a few releases ago — and a `default`
 * turns that growth into a log line that says nothing, at exactly the moment the
 * log matters. Without one, the next addition is a compiler warning instead. The
 * NULL return then covers only what a `default` cannot help with anyway: a value
 * that is not a valid enumerator at all.
 */
static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_UNKNOWN:    return "unknown";
    case ESP_RST_POWERON:    return "power-on";
    case ESP_RST_EXT:        return "external pin";
    case ESP_RST_SW:         return "software";
    case ESP_RST_PANIC:      return "panic";
    case ESP_RST_INT_WDT:    return "interrupt watchdog";
    case ESP_RST_TASK_WDT:   return "task watchdog";
    case ESP_RST_WDT:        return "other watchdog";
    case ESP_RST_DEEPSLEEP:  return "deep-sleep wake";
    case ESP_RST_BROWNOUT:   return "brownout";
    case ESP_RST_SDIO:       return "SDIO";
    case ESP_RST_USB:        return "USB peripheral";
    case ESP_RST_JTAG:       return "JTAG";
    case ESP_RST_EFUSE:      return "efuse error";
    case ESP_RST_PWR_GLITCH: return "power glitch";
    case ESP_RST_CPU_LOCKUP: return "CPU lockup";
    }
    return NULL;
}

/**
 * @brief Claim the status LED's pin, starting dark.
 *
 * Dark rather than lit, because a lit LED is a statement — once the heartbeat is
 * running it means "firmware is alive", and it should not say that before the
 * firmware has got as far as saying it deliberately.
 */
static void status_led_init(void)
{
    hw_gpio_output_init(BOARD_STATUS_LED_GPIO, !BOARD_STATUS_LED_LIT_LEVEL);
}

static void status_led_set(bool lit)
{
    hw_gpio_write(BOARD_STATUS_LED_GPIO,
                  lit ? BOARD_STATUS_LED_LIT_LEVEL : !BOARD_STATUS_LED_LIT_LEVEL);
}

/**
 * @brief Report every address on the I²C bus that answers.
 *
 * The first thing worth knowing about a bus whose wiring is undocumented, and worth
 * keeping afterwards: a display that has come loose shows up here as an address that
 * stopped answering, which is a far better diagnostic than a blank screen.
 *
 * Addresses below 0x08 and above 0x77 are reserved by the I²C specification and are
 * not probed — 0x00 is the general call, which a device may acknowledge without that
 * saying anything about where it lives.
 */
static void i2c_scan(void)
{
    unsigned found = 0;

    for (uint8_t address = 0x08; address <= 0x77; address++) {
        const hw_i2c_result_t result = hw_i2c_probe(address);

        if (result == HW_I2C_OK) {
            ESP_LOGI(TAG, "i2c: device at 0x%02X", address);
            found++;
        } else if (result != HW_I2C_NACK) {
            /* A NACK is the ordinary answer from an empty address. Anything else
             * describes the bus rather than the address, and will not improve by
             * asking the next one — so say so once and stop. */
            ESP_LOGE(TAG, "i2c: probing 0x%02X failed: %s", address,
                     hw_i2c_result_name(result));
            return;
        }
    }

    ESP_LOGI(TAG, "i2c: scan complete, %u device(s) on SDA=GPIO%u SCL=GPIO%u", found,
             (unsigned)BOARD_I2C_SDA_GPIO, (unsigned)BOARD_I2C_SCL_GPIO);
}

/**
 * @brief Append @p ssid to @p out as HTML text content, escaping the three characters
 *        that would otherwise be read as markup.
 *
 * An SSID is 802.11's to hand out, not this device's: anyone in radio range names their
 * own access point, and that name lands in a page this device serves. It is not this
 * project's threat model that gains from it — the only person who can read this page is
 * the one standing in front of the hardware provisioning it — but a neighbour's SSID
 * closing a tag it did not open is a defect either way, and escaping it costs one
 * switch per character.
 *
 * @return Bytes written, not counting the terminator, or 0 if @p size left no room.
 */
static size_t append_escaped(char *out, size_t size, const char *ssid)
{
    size_t pos = 0;

    for (const char *p = ssid; *p != '\0'; p++) {
        const char *entity;
        switch (*p) {
        case '&': entity = "&amp;"; break;
        case '<': entity = "&lt;"; break;
        case '>': entity = "&gt;"; break;
        default:  entity = NULL;   break;
        }

        const size_t needed = entity != NULL ? strlen(entity) : 1u;
        if (pos + needed >= size) {
            /* No room left, and no partial entity: better to end the name one
             * character early than to hand the browser "&am" and have it wait for
             * a semicolon that is never coming. */
            break;
        }

        if (entity != NULL) {
            memcpy(&out[pos], entity, needed);
        } else {
            out[pos] = *p;
        }
        pos += needed;
    }

    out[pos] = '\0';
    return pos;
}

/**
 * @brief Render the networks currently in range into @ref s_network_list.
 *
 * Scanning blocks for as long as the radio needs to visit every channel — about a
 * second and a half — which is why this runs only when the page that shows the result
 * is actually being requested, and not on a timer no one is looking at.
 */
static void build_network_list(void)
{
    static wifi_manager_network_t networks[WIFI_MANAGER_SCAN_MAX];
    uint16_t count = WIFI_MANAGER_SCAN_MAX;

    const esp_err_t err = wifi_manager_scan(networks, &count);

    if (err != ESP_OK) {
        snprintf(s_network_list, sizeof(s_network_list), "<li>Scan failed</li>");
        return;
    }

    if (count == 0) {
        snprintf(s_network_list, sizeof(s_network_list), "<li>None found</li>");
        return;
    }

    size_t pos = 0;
    for (uint16_t i = 0; i < count; i++) {
        /* Each entry fits within NETWORK_ITEM_BUDGET by construction, so this never
         * runs short before sizeof(s_network_list) does — but the check stays, because
         * "cannot happen" is not the same claim as "checked". */
        if (sizeof(s_network_list) - pos < NETWORK_ITEM_BUDGET) {
            break;
        }

        const int written = snprintf(&s_network_list[pos], sizeof(s_network_list) - pos,
                                     "<li>");
        pos += (size_t)written;
        pos += append_escaped(&s_network_list[pos], sizeof(s_network_list) - pos,
                              networks[i].ssid);
        if (!networks[i].secured) {
            pos += (size_t)snprintf(&s_network_list[pos], sizeof(s_network_list) - pos,
                                    " (open)");
        }
        pos += (size_t)snprintf(&s_network_list[pos], sizeof(s_network_list) - pos,
                                "</li>");
    }
}

/**
 * @brief Show what someone needs in order to put this device on a network.
 *
 * Three lines, in the order they get used: join this network, type this key, open this
 * address. Twelve characters leaves no room for labels, so the sequence carries the
 * meaning instead — and the values are self-identifying anyway, since one is a network
 * name, one is ten uppercase characters, and one has dots in it.
 *
 * The setup password exists only here. It is generated on the device and drawn on the
 * panel, never logged and never held anywhere a network can reach, so the only way to
 * learn it is to be standing in front of the hardware. For a box that lives inside a PC
 * case, that is exactly the right bar.
 */
static void draw_setup_screen(void)
{
    ssd1306_clear();
    ssd1306_draw_text(0, 0u * SSD1306_TEXT_LINE_HEIGHT, "WIFI SETUP", 1u);
    ssd1306_draw_text(0, 1u * SSD1306_TEXT_LINE_HEIGHT, wifi_manager_setup_ssid(), 1u);
    ssd1306_draw_text(0, 2u * SSD1306_TEXT_LINE_HEIGHT, wifi_manager_setup_password(),
                      1u);
    ssd1306_draw_text(0, 3u * SSD1306_TEXT_LINE_HEIGHT, SETUP_ADDRESS, 1u);
}

/**
 * @brief The provisioning page.
 *
 * Everything is inline. A device serving a page over its own access point has no
 * internet behind it, so a stylesheet or font from a CDN is a request that hangs until
 * the browser gives up — and the page renders unstyled after a delay that looks like the
 * device being broken.
 */
static const char SETUP_PAGE[] =
    "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>PC power controller</title><style>"
    "body{font:16px/1.5 system-ui,-apple-system,sans-serif;margin:0;padding:1.5rem;"
    "background:#14161a;color:#e8eaed}"
    "h1{font-size:1.2rem;margin:0 0 .5rem}p{margin:0 0 1.5rem;color:#9aa0a6}"
    "dt{font-size:.8rem;color:#9aa0a6;text-transform:uppercase;letter-spacing:.05em}"
    "dd{margin:.15rem 0 1rem;font-family:ui-monospace,monospace;color:#8ab4f8}"
    "h2{font-size:.9rem;margin:0 0 .5rem;color:#9aa0a6;font-weight:400}"
    "ul{list-style:none;margin:0;padding:0}"
    "li{padding:.4rem 0;border-top:1px solid #2a2d33}"
    "</style></head><body><h1>PC power controller</h1>"
    "<p>Setup mode. This device is not on a network yet.</p>"
    "<dl><dt>Access point</dt><dd>%s</dd>"
    "<dt>Firmware</dt><dd>%s</dd></dl>"
    "<h2>Networks in range</h2><ul>%s</ul></body></html>";

/**
 * @brief Answer one request.
 *
 * Runs on the server's task, which is why it still doesn't touch the display — a
 * handler that waited on the I2C bus would hold the only connection slot for the 32 ms
 * a redraw takes. It does now block on a Wi-Fi scan, for about a second and a half:
 * the request asking for this page is, so far, always a person watching their phone
 * wait for exactly that.
 */
static void on_http_request(const http_request_t *request, http_response_t *response)
{
    if (strcmp(request->method, "GET") != 0) {
        response->status = 405;
        return;
    }
    if (strcmp(request->target, "/") != 0) {
        response->status = 404;
        return;
    }

    build_network_list();

    const int written =
        snprintf(response->body, response->body_capacity, SETUP_PAGE,
                 wifi_manager_setup_ssid(), esp_app_get_description()->version,
                 s_network_list);

    if (written < 0 || (size_t)written >= response->body_capacity) {
        /* Truncated output would be a broken page. Reporting a server error says which
         * side the fault is on, which a half-rendered page does not. */
        response->status = 500;
        return;
    }

    response->status = 200;
    response->content_type = "text/html; charset=utf-8";
    response->body_length = (size_t)written;
}

void app_main(void)
{
    /* The numeric code is logged alongside the name, unconditionally. A name this
     * build does not have is still a number someone can look up, which is the
     * difference between a diagnosable boot and a dead end. */
    const esp_reset_reason_t reason = esp_reset_reason();
    const char *name = reset_reason_name(reason);
    ESP_LOGI(TAG, "reset reason: %s (%d)", name ? name : "unnamed", (int)reason);

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "chip: %s rev %u.%u, %d core(s)",
             CONFIG_IDF_TARGET, chip.revision / 100, chip.revision % 100, chip.cores);

    /* Flash size is read from the chip rather than taken from sdkconfig, so a
     * board that does not match the configured size shows up here instead of as a
     * mysterious failure later, once something writes past the end of it. */
    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        ESP_LOGI(TAG, "flash: %" PRIu32 " KiB", flash_size / 1024);
    } else {
        ESP_LOGW(TAG, "flash: size unavailable");
    }

    ESP_LOGI(TAG, "free heap: %" PRIu32 " bytes", esp_get_free_heap_size());

    status_led_init();
    ESP_LOGI(TAG, "heartbeat on GPIO%u", (unsigned)BOARD_STATUS_LED_GPIO);

    hw_i2c_result_t display = hw_i2c_init(BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO,
                                          BOARD_I2C_HZ);
    if (display != HW_I2C_OK) {
        /* The bus could not be freed, so nothing read from it means anything. Say so
         * and stop, rather than scanning a bus that is going to invent devices. */
        ESP_LOGE(TAG, "i2c: %s", hw_i2c_result_name(display));
    } else {
        i2c_scan();
        display = ssd1306_init();
    }

    if (display == HW_I2C_OK) {
        ESP_LOGI(TAG, "display: %ux%u at 0x%02X, %ux%u characters",
                 (unsigned)SSD1306_WIDTH, (unsigned)SSD1306_HEIGHT, SSD1306_ADDRESS,
                 (unsigned)SSD1306_TEXT_COLUMNS, (unsigned)SSD1306_TEXT_ROWS);
    } else {
        ESP_LOGE(TAG, "display: %s", hw_i2c_result_name(display));
    }

    const esp_err_t wifi = wifi_manager_start();
    if (wifi != ESP_OK) {
        ESP_LOGE(TAG, "wifi: setup mode failed: %s", esp_err_to_name(wifi));
    } else {
        const esp_err_t server = http_server_start(HTTP_PORT, &on_http_request);
        if (server == ESP_OK) {
            ESP_LOGI(TAG, "setup: join \"%s\" and open http://%s",
                     wifi_manager_setup_ssid(), SETUP_ADDRESS);
        } else {
            ESP_LOGE(TAG, "http: %s", esp_err_to_name(server));
        }
    }

    if (display == HW_I2C_OK && wifi == ESP_OK) {
        draw_setup_screen();
        display = ssd1306_flush();
        if (display != HW_I2C_OK) {
            ESP_LOGE(TAG, "display: %s", hw_i2c_result_name(display));
        }
    }

    /*
     * The heartbeat is all this loop does for now. The screen is static until there is
     * something to change it — the Wi-Fi driver's work happens in its own task, and
     * redrawing an unchanged panel once a second would cost 32 ms of bus traffic to
     * display the same thing.
     */
    for (;;) {
        status_led_set(true);
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_LIT_MS));
        status_led_set(false);
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_DARK_MS));
    }
}
