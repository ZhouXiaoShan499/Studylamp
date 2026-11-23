#include "voice_pipeline.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "audio_in.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lamp_state.h"

#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_vad.h"
#include "model_path.h"

static const char *TAG = "voice";

/*
 * MultiNet 的识别窗口。窗口内没听到命令就进入 ESP_MN_STATE_TIMEOUT，
 * 之后 detect() 不再出结果，必须 clean() 才能重开。
 * 官方示例是"超时后停下等唤醒词"，本项目的目标是"直说就能开灯"，
 * 所以超时立刻重开（见循环里的 TIMEOUT 分支）。10 分钟足够长，实际几乎不会触发。
 */
#define MN_WINDOW_MS 600000

/* 电平日志周期：每 N 帧打一次 mic peak/rms。这是判断"麦克风有没有信号"的唯一证据。 */
#define LEVEL_LOG_FRAMES 60

typedef struct {
    int id;
    const char *pinyin;
    const char *label;
} cmd_entry_t;

/* pinyin 不带声调，音节空格分隔，ID 必须 > 0。 */
static const cmd_entry_t kCmds[] = {
    {1, "da kai deng", "开灯"},
    {2, "guan deng", "关灯"},
    {3, "liang yi dian", "亮一点"},
    {4, "an yi dian", "暗一点"},
    {5, "zui liang", "最亮"},
};
#define CMD_COUNT (int)(sizeof(kCmds) / sizeof(kCmds[0]))

/*
 * 意图 → 灯。数值语义照搬 Arduino main.cpp:265-310 的 onVoiceCommand()：
 *   开灯   回到关灯前的亮度（没有记录时 70）
 *   关灯   记住当前亮度再关
 *   亮一点 +20，上限 100
 *   暗一点 -20，下限 5
 *   最亮   100
 */
static void handle_command(int id)
{
    const int level = lamp_state_level();
    switch (id) {
        case 1: {
            const int last = lamp_state_last_on_level();
            lamp_state_set_level(last > 0 ? last : 70, "voice:on");
            break;
        }
        case 2:
            lamp_state_set_level(0, "voice:off");
            break;
        case 3: {
            const int next = level + 20;
            lamp_state_set_level(next > 100 ? 100 : next, "voice:up");
            break;
        }
        case 4: {
            const int next = level - 20;
            lamp_state_set_level(next < 5 ? 5 : next, "voice:down");
            break;
        }
        case 5:
            lamp_state_set_level(100, "voice:max");
            break;
        default:
            ESP_LOGW(TAG, "unknown command id %d", id);
            return;
    }
}

static void level_log(const int16_t *pcm, size_t n, int32_t *peak, int64_t *sq_sum, int64_t *sq_cnt)
{
    for (size_t i = 0; i < n; ++i) {
        const int32_t v = (pcm[i] < 0) ? -pcm[i] : pcm[i];
        if (v > *peak) {
            *peak = v;
        }
    }
    int64_t sum = 0;
    for (size_t i = 0; i < n; ++i) {
        sum += (int64_t)pcm[i] * pcm[i];
    }
    *sq_sum += sum;
    *sq_cnt += (int64_t)n;
}

static void sr_task(void *arg)
{
    (void)arg;

    /* 1) 音频源。失败就直接退出任务：按键和灯不该被麦克风拖下水。 */
    if (audio_in_start() != ESP_OK) {
        ESP_LOGE(TAG, "audio source %s failed, voice disabled (button still works)",
                 audio_in_source_name());
        vTaskDelete(NULL);
        return;
    }

    /* 2) 挂载 model 分区里的模型包（srmodels.bin 由 idf.py flash 自动烧进去）。 */
    srmodel_list_t *models = esp_srmodel_init("model");
    if (models == NULL) {
        ESP_LOGE(TAG, "esp_srmodel_init failed: model partition missing or empty");
        ESP_LOGE(TAG, "check partitions.csv has a 'model' partition and flash included srmodels.bin");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "model partition: %d model(s)", models->num);

    /* 3) 选中文 MultiNet。 */
    char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (mn_name == NULL) {
        ESP_LOGE(TAG, "no Chinese MultiNet model; enable CONFIG_SR_MN_CN_MULTINET7_QUANT");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "MultiNet model: %s", mn_name);

    esp_mn_iface_t *multinet = esp_mn_handle_from_name(mn_name);
    if (multinet == NULL) {
        ESP_LOGE(TAG, "esp_mn_handle_from_name failed");
        vTaskDelete(NULL);
        return;
    }
    model_iface_data_t *model_data = multinet->create(mn_name, MN_WINDOW_MS);
    if (model_data == NULL) {
        ESP_LOGE(TAG, "multinet->create failed (out of PSRAM?)");
        vTaskDelete(NULL);
        return;
    }

    /* 4) 注册命令词。2.6.x 必须先 alloc 链表，否则 add 返回 ESP_ERR_INVALID_STATE。 */
    ESP_ERROR_CHECK(esp_mn_commands_alloc(multinet, model_data));
    esp_mn_commands_clear();
    for (int i = 0; i < CMD_COUNT; i++) {
        const esp_err_t err = esp_mn_commands_add(kCmds[i].id, kCmds[i].pinyin);
        ESP_LOGI(TAG, "add [%d] %-14s (%s) -> %s", kCmds[i].id, kCmds[i].pinyin, kCmds[i].label,
                 esp_err_to_name(err));
    }
    if (esp_mn_commands_update() != NULL) {
        ESP_LOGW(TAG, "some commands were rejected, check pinyin spelling");
    }

    /* 5) AFE。增益链路与 lamp_sr 一致：前置增益在音频源实现里，AFE 不再放大。 */
    afe_config_t *afe_cfg = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (afe_cfg == NULL) {
        ESP_LOGE(TAG, "afe_config_init failed");
        vTaskDelete(NULL);
        return;
    }
    afe_cfg->afe_linear_gain = 1.0f;
    const esp_afe_sr_iface_t *afe = esp_afe_handle_from_config(afe_cfg);
    esp_afe_sr_data_t *afe_data = afe->create_from_config(afe_cfg);
    if (afe_data == NULL) {
        ESP_LOGE(TAG, "afe create failed (PSRAM not enabled?)");
        vTaskDelete(NULL);
        return;
    }

    const int feed_chunk = afe->get_feed_chunksize(afe_data);
    const int feed_nch = afe->get_feed_channel_num(afe_data);
    const int mn_chunk = multinet->get_samp_chunksize(model_data);
    ESP_LOGI(TAG, "afe feed=%d nch=%d | mn chunk=%d", feed_chunk, feed_nch, mn_chunk);
    if (feed_chunk != mn_chunk) {
        ESP_LOGE(TAG, "chunk mismatch: afe=%d mn=%d (MultiNet needs the same frame size)", feed_chunk,
                 mn_chunk);
    }

    const size_t want = (size_t)feed_chunk * (size_t)feed_nch;
    int16_t *pcm = (int16_t *)malloc(want * sizeof(int16_t));
    if (pcm == NULL) {
        ESP_LOGE(TAG, "pcm buffer alloc failed (%u samples)", (unsigned)want);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGW(TAG, "==================================================");
    ESP_LOGW(TAG, "ready: say 'da kai deng' / 'guan deng' / 'liang yi dian' / 'an yi dian' / 'zui liang'");
    ESP_LOGW(TAG, "wake word is not required; timeout auto-reopens the window");
    ESP_LOGW(TAG, "==================================================");

    int logged = 0;
    int32_t peak = 0;
    int64_t sq_sum = 0;
    int64_t sq_cnt = 0;
    int64_t last_vad_log = 0;
    int timeouts = 0;
    int starved = 0;

    while (true) {
        /* 唯一与 lamp_sr 不同的地方：音频从抽象层来，不再直接读 I2S。 */
        const size_t got = audio_in_read(pcm, want, 1000);
        if (got == 0) {
            /* 关断（半双工）或空读：喂一帧静音，让 AFE 的状态机继续走。 */
            starved++;
            for (size_t i = 0; i < want; ++i) {
                pcm[i] = 0;
            }
        }

        level_log(pcm, want, &peak, &sq_sum, &sq_cnt);
        if (++logged >= LEVEL_LOG_FRAMES) {
            logged = 0;
            ESP_LOGI(TAG, "mic peak=%d rms=%d src=%s starved=%d/%d", (int)peak,
                     sq_cnt ? (int)(sqrtf((float)sq_sum / (float)sq_cnt)) : 0, audio_in_source_name(),
                     starved, LEVEL_LOG_FRAMES);
            peak = 0;
            sq_sum = 0;
            sq_cnt = 0;
            starved = 0;
        }

        afe->feed(afe_data, pcm);
        afe_fetch_result_t *res = afe->fetch(afe_data);
        if (res == NULL || res->ret_value == ESP_FAIL) {
            continue;
        }

        if (res->vad_state == VAD_SPEECH) {
            const int64_t now = esp_timer_get_time();
            if (now - last_vad_log > 1000000) {
                last_vad_log = now;
                ESP_LOGW(TAG, "vad=speech dbfs=%.1f", res->data_volume);
            }
        }

        const esp_mn_state_t mn_state = multinet->detect(model_data, res->data);
        if (mn_state == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *r = multinet->get_results(model_data);
            if (r != NULL) {
                for (int i = 0; i < r->num; i++) {
                    ESP_LOGW(TAG, "MN id=%d prob=%.3f text=%s", r->command_id[i], r->prob[i], r->string);
                    handle_command(r->command_id[i]);
                }
            }
            multinet->clean(model_data);
        } else if (mn_state == ESP_MN_STATE_TIMEOUT) {
            multinet->clean(model_data);
            if (timeouts < 3) {
                ESP_LOGI(TAG, "mn window expired, reopened (normal when idle)");
            }
            timeouts++;
        }
    }
}

void voice_pipeline_start(void)
{
    /* 栈要够：AFE + MultiNet 的调用链比看起来深；优先级高于灯斜坡、低于 WiFi 任务。 */
    xTaskCreatePinnedToCore(sr_task, "voice_task", 8 * 1024, NULL, 5, NULL, 1);
}
