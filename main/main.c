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

#include "board.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hw_gpio.h"
#include "hw_i2c.h"

static const char *TAG = "boot";

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

    hw_i2c_init(BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO, BOARD_I2C_HZ);
    i2c_scan();

    for (bool lit = true;; lit = !lit) {
        status_led_set(lit);
        vTaskDelay(pdMS_TO_TICKS(lit ? HEARTBEAT_LIT_MS : HEARTBEAT_DARK_MS));
    }
}
