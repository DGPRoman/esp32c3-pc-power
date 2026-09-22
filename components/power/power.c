#include "power.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "hw_gpio.h"

static const char *TAG = "power";

/**
 * @brief How often the sense line is read, in milliseconds.
 *
 * Five samples inside the debounce below, which is enough for the debounce to mean
 * something and infrequent enough that a timer callback every one of them is not
 * worth thinking about. Nothing on the other end moves faster: a power LED changes
 * state when a machine does.
 */
#define SAMPLE_MS 10u

/*
 * Durations of an ATX front-panel press, not of this board.
 *
 * PRESS_MS is a firm tap, well clear of the four seconds a motherboard reads as a
 * hold, and well clear of the few tens of milliseconds it might dismiss as a
 * bounce. HOLD_MS is past four seconds by enough that the threshold's exact value
 * on any particular board does not matter.
 *
 * The two waits are asymmetric because the things they wait for are. A machine
 * that is going to come on drives its LED within a second or so of the supply
 * coming up; a machine being asked to shut down hands the request to an operating
 * system, which may spend a minute on it or put a dialog about unsaved work on a
 * screen nobody is looking at. Waiting a minute costs nothing but a state that
 * reads turning_off for a minute, which is the truth.
 */
#define DEBOUNCE_MS 50u
#define PRESS_MS 200u
#define HOLD_MS 6000u
#define TURN_ON_MS 10000u
#define TURN_OFF_MS 60000u

static power_wiring_t s_wiring;
static power_machine_t s_machine;
static esp_timer_handle_t s_sampler;
static bool s_running;

/*
 * A spinlock rather than a mutex. Everything guarded here is a few loads and
 * stores over one structure, and the two callers are a timer callback and an HTTP
 * task: a mutex would let the HTTP task block the sampler for as long as the
 * scheduler felt like, to protect a section shorter than the call to take it.
 */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/** @brief Device uptime in milliseconds, which is the clock every deadline uses. */
static uint64_t now_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000u;
}

/** @brief Read the sense pin and translate the board's polarity away. */
static bool sense_lit(void)
{
    return hw_gpio_read(s_wiring.sense_gpio) == s_wiring.sense_lit_level;
}

/** @brief Drive the button line, translating the board's polarity away. */
static void drive_button(bool pressed)
{
    hw_gpio_write(s_wiring.button_gpio,
                  pressed ? s_wiring.button_pressed_level : !s_wiring.button_pressed_level);
}

/**
 * @brief One sample: read the line, advance the machine, drive the button.
 *
 * The pin is written on every tick rather than only on a change. It is a single
 * store to a register that already holds that value, and it means the line cannot
 * be left asserted by a missed edge — which for a power button is the difference
 * between a tap and a cut-off.
 */
static void sample(void *unused)
{
    (void)unused;

    const bool lit = sense_lit();
    const uint64_t moment = now_ms();

    power_state_t before;
    power_state_t after;
    bool pressed;

    portENTER_CRITICAL(&s_lock);
    before = power_machine_state(&s_machine);
    power_machine_advance(&s_machine, lit, moment);
    after = power_machine_state(&s_machine);
    pressed = power_machine_button(&s_machine);
    portEXIT_CRITICAL(&s_lock);

    drive_button(pressed);

    /* Outside the critical section: logging takes locks of its own, and taking one
     * inside a spinlock is how a system stops. */
    if (after != before) {
        ESP_LOGI(TAG, "%s -> %s", power_state_name(before), power_state_name(after));
    }
}

esp_err_t power_start(const power_wiring_t *wiring)
{
    if (wiring == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_running) {
        return ESP_ERR_INVALID_STATE;
    }

    s_wiring = *wiring;

    /* Released first, and before anything else here can fail. A pin claimed as an
     * output while the opto is on the other end of it is a keypress, so the level
     * has to be the resting one from the first instant it is driven at all —
     * hw_gpio_output_init latches before it enables for exactly this reason. */
    hw_gpio_output_init(s_wiring.button_gpio, !s_wiring.button_pressed_level);

    /* No internal pull: the divider on the other end defines both levels, and a
     * 45 kΩ pull-up across it would shift them by an amount that depends on
     * resistors this firmware cannot see. */
    hw_gpio_input_init(s_wiring.sense_gpio, false);

    const power_config_t timing = {
        .debounce_ms = DEBOUNCE_MS,
        .press_ms = PRESS_MS,
        .hold_ms = HOLD_MS,
        .turn_on_ms = TURN_ON_MS,
        .turn_off_ms = TURN_OFF_MS,
    };
    power_machine_init(&s_machine, &timing, now_ms());

    const esp_timer_create_args_t sampler_args = {
        .callback = &sample,
        .name = "power_sample",
    };
    esp_err_t err = esp_timer_create(&sampler_args, &s_sampler);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_timer_start_periodic(s_sampler, (uint64_t)SAMPLE_MS * 1000u);
    if (err != ESP_OK) {
        (void)esp_timer_delete(s_sampler);
        s_sampler = NULL;
        return err;
    }

    s_running = true;
    ESP_LOGI(TAG, "button on GPIO%u, sense on GPIO%u, sampled every %ums",
             (unsigned)s_wiring.button_gpio, (unsigned)s_wiring.sense_gpio,
             (unsigned)SAMPLE_MS);
    return ESP_OK;
}

power_status_t power_read(void)
{
    power_status_t status;
    memset(&status, 0, sizeof(status));

    if (!s_running) {
        status.state = POWER_UNKNOWN;
        status.pending = POWER_REQUEST_NONE;
        return status;
    }

    portENTER_CRITICAL(&s_lock);
    status.state = power_machine_state(&s_machine);
    status.pending = power_machine_pending(&s_machine);
    status.observed_at_ms = power_machine_state_since(&s_machine);
    portEXIT_CRITICAL(&s_lock);

    status.uptime_ms = now_ms();
    return status;
}

bool power_request(power_request_t what)
{
    if (!s_running) {
        return false;
    }

    const uint64_t moment = now_ms();

    portENTER_CRITICAL(&s_lock);
    const bool taken = power_machine_request(&s_machine, what, moment);
    /* Assert the line here rather than waiting for the next sample. Ten
     * milliseconds would not matter to the motherboard, but leaving it until the
     * sampler means the pulse is a sample period longer than it was asked to be. */
    const bool pressed = power_machine_button(&s_machine);
    portEXIT_CRITICAL(&s_lock);

    if (taken) {
        drive_button(pressed);
        ESP_LOGI(TAG, "%s requested", power_request_name(what));
    } else {
        ESP_LOGW(TAG, "%s refused: the line is busy or the state is not known",
                 power_request_name(what));
    }

    return taken;
}
