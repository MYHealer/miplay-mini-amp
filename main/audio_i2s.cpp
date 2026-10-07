// I2S 音频输出。
//
// 架构：解码 → 环形缓冲 → I2S 输出任务（双任务解耦，参考 esp-miply-1.85touch）。
//   audio_i2s_write() 只做音量 + 压环，0 超时不阻塞（解码/网络永不被 I2S 卡住）。
//   i2s_output_task() 专职从环读 + 写 DMA + 切采样率，阻塞只在输出侧。
//   环满丢帧（实时音频宁丢帧不延迟），环足够深（约 200ms）吸收网络抖动。

#include "audio_i2s.h"

#include <string.h>

#include "app_volume.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace {

constexpr char kTag[] = "audio_i2s";

#ifndef CONFIG_MIPLAY_I2S_BCLK_GPIO
#define CONFIG_MIPLAY_I2S_BCLK_GPIO 12
#endif
#ifndef CONFIG_MIPLAY_I2S_LRCK_GPIO
#define CONFIG_MIPLAY_I2S_LRCK_GPIO 10
#endif
#ifndef CONFIG_MIPLAY_I2S_DIN_GPIO
#define CONFIG_MIPLAY_I2S_DIN_GPIO 11
#endif
#ifndef CONFIG_MIPLAY_I2S_MCLK_GPIO
#define CONFIG_MIPLAY_I2S_MCLK_GPIO 13
#endif

// DMA 描述符 × 帧数。参考 esp-miply-1.85touch：12 × 512 ≈ 128ms。
constexpr int kDmaDescNum = 12;
constexpr int kDmaFrameNum = 512;

// 环形缓冲（PSRAM）。NOSPLIT：一次 send 对应一次 receive。
// 128KB 请求 → ~64KB 实际 ≈ 16 个 AAC 帧（每帧 ~4KB）≈ 16×21ms ≈ 340ms。
// 足够吸收网络抖动，环满只丢帧不延迟。
constexpr size_t kRingBufBytes = 128 * 1024;

// 输出任务栈：含 i2s_channel_write 调用链，8KB 足够。
constexpr uint32_t kOutTaskStack = 8192;
// 输出任务优先级：6 高于网络(4)，保证 DMA 不饿死。
constexpr UBaseType_t kOutTaskPriority = 6;

i2s_chan_handle_t s_tx = nullptr;
uint32_t s_sample_rate = 0;

RingbufHandle_t s_ring = nullptr;
StaticRingbuffer_t s_ring_struct = {};
uint8_t *s_ring_storage = nullptr;
TaskHandle_t s_out_task = nullptr;
volatile bool s_out_running = false;

// 环 item 格式：[int32 rate][int32 channels][int16 pcm × N]
struct ItemHeader {
    int32_t rate;
    int32_t channels;
};

void fill_std_config(i2s_std_config_t *cfg, uint32_t sample_rate)
{
    *cfg = {};
    cfg->clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    cfg->slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    cfg->gpio_cfg.bclk = (gpio_num_t)CONFIG_MIPLAY_I2S_BCLK_GPIO;
    cfg->gpio_cfg.ws = (gpio_num_t)CONFIG_MIPLAY_I2S_LRCK_GPIO;
    cfg->gpio_cfg.dout = (gpio_num_t)CONFIG_MIPLAY_I2S_DIN_GPIO;
    // SCK 接 GPIO 9：从这里输出 MCLK 给 DAC。
    cfg->gpio_cfg.mclk = (gpio_num_t)CONFIG_MIPLAY_I2S_MCLK_GPIO;
    cfg->gpio_cfg.invert_flags = {};
}

// I2S 输出任务：从环读 → 写 DMA。
void i2s_output_task(void *)
{
    // 内部 DMA RAM 中转。GDMA 不能可靠读 PSRAM，拷一次确保正确。
    static int16_t *s_stage = nullptr;
    static size_t s_stage_cap = 0;

    while (s_out_running) {
        size_t item_size = 0;
        void *item = xRingbufferReceive(s_ring, &item_size, pdMS_TO_TICKS(50));
        if (item == nullptr) continue;
        if (item_size < sizeof(ItemHeader) || s_tx == nullptr) {
            vRingbufferReturnItem(s_ring, item);
            continue;
        }

        const auto *hdr = static_cast<const ItemHeader *>(item);
        const auto *pcm = reinterpret_cast<const int16_t *>(
            static_cast<const uint8_t *>(item) + sizeof(ItemHeader));
        const size_t samples = (item_size - sizeof(ItemHeader)) / sizeof(int16_t);
        const size_t bytes = samples * sizeof(int16_t);

        // 采样率切换（在输出任务内，不阻塞解码）。
        if (hdr->rate > 0 && (uint32_t)hdr->rate != s_sample_rate) {
            const auto rate = static_cast<uint32_t>(hdr->rate);
            i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
            i2s_channel_disable(s_tx);
            esp_err_t err = i2s_channel_reconfig_std_clock(s_tx, &clk_cfg);
            i2s_channel_enable(s_tx);
            if (err == ESP_OK) {
                s_sample_rate = rate;
                ESP_LOGI(kTag, "sample rate -> %u Hz", (unsigned)rate);
            }
        }

        // 内部 DMA RAM 中转。
        if (samples > s_stage_cap) {
            int16_t *grown = static_cast<int16_t *>(
                heap_caps_malloc(samples * sizeof(int16_t),
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
            if (grown != nullptr) {
                heap_caps_free(s_stage);
                s_stage = grown;
                s_stage_cap = samples;
            }
        }

        const void *src = pcm;
        if (s_stage != nullptr && samples <= s_stage_cap) {
            memcpy(s_stage, pcm, bytes);
            src = s_stage;
        }

        // 阻塞写：DMA 满就等，这里阻塞不影响解码/网络。
        i2s_channel_write(s_tx, src, bytes, nullptr, pdMS_TO_TICKS(200));

        vRingbufferReturnItem(s_ring, item);
    }

    vTaskDelete(nullptr);
}

}  // namespace

bool audio_i2s_stream_start(void)
{
    if (s_tx != nullptr) {
        ESP_LOGW(kTag, "stream_start while channel exists, ignoring");
        return true;
    }

    // 环形缓冲（仅首次，PSRAM）。
    if (s_ring == nullptr) {
        s_ring_storage = static_cast<uint8_t *>(
            heap_caps_malloc(kRingBufBytes,
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (s_ring_storage == nullptr) {
            ESP_LOGE(kTag, "ring storage alloc failed (%u B from PSRAM)",
                     (unsigned)kRingBufBytes);
            return false;
        }
        s_ring = xRingbufferCreateStatic(
            kRingBufBytes, RINGBUF_TYPE_NOSPLIT, s_ring_storage,
            &s_ring_struct);
        if (s_ring == nullptr) {
            ESP_LOGE(kTag, "ring buffer create failed");
            heap_caps_free(s_ring_storage);
            s_ring_storage = nullptr;
            return false;
        }
        ESP_LOGI(kTag, "ring buffer ready: %u B (PSRAM, NOSPLIT)",
                 (unsigned)kRingBufBytes);
    }

    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = kDmaDescNum;
    chan_cfg.dma_frame_num = kDmaFrameNum;
    chan_cfg.auto_clear = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "i2s_new_channel failed: %s", esp_err_to_name(err));
        s_tx = nullptr;
        return false;
    }

    i2s_std_config_t std_cfg;
    fill_std_config(&std_cfg, 48000);
    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "i2s_channel_init_std_mode failed: %s",
                 esp_err_to_name(err));
        i2s_del_channel(s_tx);
        s_tx = nullptr;
        return false;
    }

    err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "i2s_channel_enable failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_tx);
        s_tx = nullptr;
        return false;
    }

    s_sample_rate = 48000;

    // 输出任务。
    s_out_running = true;
    if (xTaskCreate(i2s_output_task, "i2s_out", kOutTaskStack, nullptr,
                    kOutTaskPriority, &s_out_task) != pdPASS) {
        ESP_LOGE(kTag, "output task create failed");
        s_out_running = false;
        i2s_channel_disable(s_tx);
        i2s_del_channel(s_tx);
        s_tx = nullptr;
        return false;
    }

    ESP_LOGI(kTag, "stream started: BCLK=%d LRCK=%d DIN=%d MCLK=%d "
                   "(ring=%u B, DMA %dx%d, MCLK->SCK)",
             CONFIG_MIPLAY_I2S_BCLK_GPIO, CONFIG_MIPLAY_I2S_LRCK_GPIO,
             CONFIG_MIPLAY_I2S_DIN_GPIO, CONFIG_MIPLAY_I2S_MCLK_GPIO,
             (unsigned)kRingBufBytes, kDmaDescNum, kDmaFrameNum);
    return true;
}

void audio_i2s_stream_stop(void)
{
    if (s_tx == nullptr) return;

    s_out_running = false;
    vTaskDelay(pdMS_TO_TICKS(300));  // 等输出任务退出
    s_out_task = nullptr;

    i2s_channel_disable(s_tx);
    i2s_del_channel(s_tx);
    s_tx = nullptr;
    s_sample_rate = 0;

    // 清空环残留。
    if (s_ring != nullptr) {
        size_t sz;
        void *item;
        while ((item = xRingbufferReceive(s_ring, &sz, 0)) != nullptr) {
            vRingbufferReturnItem(s_ring, item);
        }
    }
    ESP_LOGI(kTag, "stream stopped, channel released");
}

int audio_i2s_write(const int16_t *stereo_samples, size_t frame_count,
                    int sample_rate)
{
    if (stereo_samples == nullptr || frame_count == 0 || s_ring == nullptr) {
        return -1;
    }

    // 软件音量。
    app_volume_apply(const_cast<int16_t *>(stereo_samples), frame_count * 2);

    // 组装 item：header + PCM（栈上缓冲）。
    const size_t pcm_bytes = frame_count * 2 * sizeof(int16_t);
    const size_t item_size = sizeof(ItemHeader) + pcm_bytes;

    alignas(4) uint8_t stack_buf[sizeof(ItemHeader) + 4096];
    if (item_size > sizeof(stack_buf)) return -1;

    auto *hdr = reinterpret_cast<ItemHeader *>(stack_buf);
    hdr->rate = sample_rate;
    hdr->channels = 2;
    memcpy(stack_buf + sizeof(ItemHeader), stereo_samples, pcm_bytes);

    // 0 超时：环满立即丢帧返回，绝不阻塞解码/网络。
    if (xRingbufferSend(s_ring, stack_buf, item_size, 0) != pdTRUE) {
        return 0;  // 丢帧
    }
    return 0;
}