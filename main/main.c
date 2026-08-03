/**
 * @file main.c
 * @brief Firmware entry point.
 *
 * `app_main` is not C's `main`. By the time it runs, ESP-IDF's startup code has
 * already run the second-stage bootloader, initialised the heap, and started the
 * FreeRTOS scheduler; `app_main` is then spawned as an ordinary task. Returning
 * from it is legal and deletes only that task — the scheduler keeps running
 * everything else that was started. Nothing long-lived exists yet, so returning
 * is currently the correct thing to do.
 */

#include <inttypes.h>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "boot";

/**
 * @brief Human-readable form of the reason the CPU last started executing.
 *
 * Worth logging on every boot: this device's whole job is to be reachable when
 * the machine it controls is off, so an unexplained restart is a fault, not
 * noise. The reset reason is what distinguishes a power cut from a panic, a
 * watchdog timeout, or a deliberate reboot — and it is the one diagnostic that
 * survives having no debugger attached.
 */
static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_EXT:      return "external pin";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT:      return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO:     return "SDIO";
    case ESP_RST_UNKNOWN:  return "unknown";
    default:               return "unrecognised";
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "reset reason: %s", reset_reason_name(esp_reset_reason()));

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "chip: %s rev %u.%u, %d core(s)",
             CONFIG_IDF_TARGET, chip.revision / 100, chip.revision % 100, chip.cores);

    /* Flash size is read from the chip rather than taken from sdkconfig, so a
     * board that does not match the configured size shows up here instead of as
     * a mysterious failure later, once something writes past the end of it. */
    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        ESP_LOGI(TAG, "flash: %" PRIu32 " KiB", flash_size / 1024);
    } else {
        ESP_LOGW(TAG, "flash: size unavailable");
    }

    ESP_LOGI(TAG, "free heap: %" PRIu32 " bytes", esp_get_free_heap_size());
}
