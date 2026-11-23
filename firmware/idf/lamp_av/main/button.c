#include "button.h"

#include "board.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lamp_state.h"
#include "sdkconfig.h"

static const char *TAG = "btn";

#define BTN_DEBOUNCE_MS 30
#define BTN_LONGPRESS_MS 1200
#define BTN_POLL_MS 10

static inline bool raw_pressed(void)
{
    const int level = gpio_get_level(PIN_BTN);
#if CONFIG_LAMP_BTN_ACTIVE_LOW
    return level == 0;
#else
    return level == 1;
#endif
}

static void button_task(void *arg)
{
    (void)arg;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_BTN,
        .mode = GPIO_MODE_INPUT,
#if CONFIG_LAMP_BTN_ACTIVE_LOW
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
#else
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
#endif
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_LOGI(TAG, "GPIO%d idle=%d (%s)", PIN_BTN, gpio_get_level(PIN_BTN),
             raw_pressed() ? "PRESSED -- 松开手还读按下，说明极性反了，改 Kconfig" : "released");

    bool stable = raw_pressed();
    bool last_read = stable;
    bool long_fired = false;
    int64_t pressed_at = 0;
    int64_t last_change = esp_timer_get_time() / 1000;

    while (true) {
        const int64_t now = esp_timer_get_time() / 1000;
        const bool raw = raw_pressed();

        if (raw != last_read) {
            last_read = raw;
            last_change = now;
        }
        if ((now - last_change) >= BTN_DEBOUNCE_MS && raw != stable) {
            stable = raw;
            if (stable) {
                pressed_at = now;
                long_fired = false;
            } else if (!long_fired) {
                /* 松开的那一刻才认一次短按，避免按住不放连续翻转。 */
                lamp_state_toggle("key:short");
            } else {
                long_fired = false; /* 长按已执行，松手不再补一次短按 */
            }
        }
        if (stable && !long_fired && (now - pressed_at) >= BTN_LONGPRESS_MS) {
            long_fired = true;
            lamp_state_next_preset("key:long");
        }

        vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
    }
}

void button_start(void)
{
    xTaskCreatePinnedToCore(button_task, "btn_task", 3 * 1024, NULL, 3, NULL, 1);
}
