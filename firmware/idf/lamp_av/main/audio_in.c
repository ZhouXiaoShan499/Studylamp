#include "audio_in.h"

#include <stddef.h>

#include "audio_impl.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "audio";

/* Kconfig 二选一（main/Kconfig.projbuild）。两个实现都编进固件，链接器会把
 * 没用到的那份丢掉，所以固件里只有一份真正在用。 */
#if CONFIG_LAMP_AUDIO_SRC_UAC
#define ACTIVE_SRC_NAME "uac"
#else
#define ACTIVE_SRC_NAME "i2s"
#endif

static bool sEnabled = true;

esp_err_t audio_in_start(void)
{
#if CONFIG_LAMP_AUDIO_SRC_UAC
    const esp_err_t err = uac_mic_start();
#else
    const esp_err_t err = i2s_mic_start();
#endif
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "source %s failed: %s", ACTIVE_SRC_NAME, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "source %s ready @ %d Hz mono int16", ACTIVE_SRC_NAME, AUDIO_IN_RATE);
    return ESP_OK;
}

size_t audio_in_read(int16_t *pcm, size_t samples, uint32_t timeout_ms)
{
    if (!sEnabled || pcm == NULL || samples == 0) {
        return 0;
    }
#if CONFIG_LAMP_AUDIO_SRC_UAC
    return uac_mic_read(pcm, samples, timeout_ms);
#else
    return i2s_mic_read(pcm, samples, timeout_ms);
#endif
}

void audio_in_set_enabled(bool on)
{
    if (sEnabled != on) {
        ESP_LOGW(TAG, "capture %s", on ? "resumed" : "suspended (half-duplex)");
    }
    sEnabled = on;
}

bool audio_in_enabled(void)
{
    return sEnabled;
}

const char *audio_in_source_name(void)
{
    return ACTIVE_SRC_NAME;
}
