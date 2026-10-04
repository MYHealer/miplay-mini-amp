// I2S 音频输出。
//
// 架构：解码 → 环形缓冲 → I2S 输出任务（两段解耦）。
//   audio_i2s_write() 只做"音量缩放 + 压入环形缓冲"，0 超时不阻塞。
//   i2s_output_task() 专职从环读数据写硬件，采样率切换也在这里做。
//   网络收包/解码永远不会被 I2S 阻塞——这是低延迟的关键。
//   环满时丢帧（实时音频宁丢帧不延迟）。

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
#define CONFIG_MIPLAY_I2S_BCLK_GPIO 8
#endif
#ifndef CONFIG_MIPLAY_I2S_LRCK_GPIO
#define CONFIG_MIPLAY_I2S_LRCK_GPIO 7
#endif
#ifndef CONFIG_MIPLAY_I2S_DIN_GPIO
#define CONFIG_MIPLAY_I2S_DIN_GPIO 6
#endif
#ifndef CONFIG_MIPLAY_I2S_MCLK_GPIO
#define CONFIG_MIPLAY_I2S_MCLK_GPIO 9
#endif

// DMA 描述符 × 帧数。12 × 512 帧 ≈ 128ms @48kHz 立体声。
constexpr int kDmaDescNum = 12;
constexpr int kDmaFrameNum = 512;

// 环形缓冲。NOSPLIT 类型保证"一次 send 对应一次 receive"。
// NOSPLIT 会把容量对半取整：32KB 请求 → ~16KB 实际。
// 16KB ÷ ~4KB/item ≈ 4 项 ≈ 84ms 缓冲深度。够用且省内存。
constexpr size_t kRingBufBytes = 32 * 1024;

// 输出任务栈：含 i2s_channel_write 内部调用链，给足 8KB。
constexpr uint32_t kOutTaskStack = 8192;

i2s_chan_handle_t s_tx = nullptr;
uint32_t s_sample_rate = 0;

RingbufHandle_t s_ring = nullptr;
StaticRingbuffer_t s_ring_struct = {};
TaskHandle_t s_out_task = nullptr;
volatile bool s_out_running = false;

// 写入环的 item 格式：[int32 rate][int32 channels][int16 pcm × N]
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
    cfg->gpio_cfg.mclk = I2S_GPIO_UNUSED;
    cfg->gpio_cfg.invert_flags = {};
}

// I2S 输出任务：从环读 → 写 DMA。采样率切换在此完成。
void i2s_output_task(void *)
{
    // 内部 DMA RAM 中转缓冲。I2S GDMA 不能可靠读 PSRAM，拷一次确保正确。
    static int16_t *s_stage = nullptr;
    static size_t s_stage_cap = 0;

    while (s_out_running) {
        size_t item_size = 0;
        void *item = xRingbufferReceive(s_ring, &item_size, pdMS_TO_TICKS(100));
        if (item == nullptr) continue;

        if (item_size < sizeof(ItemHeader) ||
            s_tx == nullptr) {
            vRingbufferReturnItem(s_ring, item);
            continue;
        }

        const auto *hdr = static_cast<const ItemHeader *>(item);
        const auto *pcm = reinterpret_cast<const int16_t *>(
            static_cast<const uint8_t *>(item) + sizeof(ItemHeader));
        const size_t samples = (item_size - sizeof(ItemHeader)) / sizeof(int16_t);
        const size_t frame_count = (hdr->channels >= 2) ? samples / 2 : samples;
        const size_t bytes = samples * sizeof(int16_t);

        // 采样率切换。
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
        const size_t need = samples;
        if (need > s_stage_cap) {
            int16_t *grown = static_cast<int16_t *>(
                heap_caps_malloc(need * sizeof(int16_t),
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
            if (grown != nullptr) {
                heap_caps_free(s_stage);
                s_stage = grown;
                s_stage_cap = need;
            }
        }

        const void *src = pcm;
        if (s_stage != nullptr && need <= s_stage_cap) {
            memcpy(s_stage, pcm, bytes);
            src = s_stage;
        }

        size_t written = 0;
        // 输出侧用正常阻塞（200ms）：DMA 满是正常的，等它腾出空间。
        // 这里阻塞不影响网络——只有这个输出任务在等。
        i2s_channel_write(s_tx, src, bytes, &written, pdMS_TO_TICKS(200));

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

    // GPIO 9 接 PCM5102A SCK（已接地）。驱动为低电平。
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << CONFIG_MIPLAY_I2S_MCLK_GPIO;
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io);
    gpio_set_level((gpio_num_t)CONFIG_MIPLAY_I2S_MCLK_GPIO, 0);

    // 创建环形缓冲（仅首次）。存储区放 PSRAM，省内部 SRAM 给 Wi-Fi/I2S DMA。
    if (s_ring == nullptr) {
        auto *storage = static_cast<uint8_t *>(
            heap_caps_malloc(kRingBufBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (storage == nullptr) {
            ESP_LOGE(kTag, "ring storage alloc failed (%u B from PSRAM)",
                     (unsigned)kRingBufBytes);
            return false;
        }
        s_ring = xRingbufferCreateStatic(kRingBufBytes, RINGBUF_TYPE_NOSPLIT,
                                         storage, &s_ring_struct);
        if (s_ring == nullptr) {
            ESP_LOGE(kTag, "ring buffer create failed");
            heap_caps_free(storage);
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

    // 启动输出任务。
    // 输出任务优先级 6 > 网络任务(4)：必须优先喂 I2S DMA，
    // 否则 DMA 饿死 → 一个字卡一下。
    s_out_running = true;
    if (xTaskCreate(i2s_output_task, "i2s_out", kOutTaskStack, nullptr, 6,
                    &s_out_task) != pdPASS) {
        ESP_LOGE(kTag, "output task create failed");
        s_out_running = false;
        i2s_channel_disable(s_tx);
        i2s_del_channel(s_tx);
        s_tx = nullptr;
        return false;
    }

    ESP_LOGI(kTag, "stream started: BCLK=%d LRCK=%d DIN=%d "
                   "(ring=%u B, DMA %dx%d, SCK GND PLL)",
             CONFIG_MIPLAY_I2S_BCLK_GPIO, CONFIG_MIPLAY_I2S_LRCK_GPIO,
             CONFIG_MIPLAY_I2S_DIN_GPIO, (unsigned)kRingBufBytes,
             kDmaDescNum, kDmaFrameNum);
    return true;
}

void audio_i2s_stream_stop(void)
{
    if (s_tx == nullptr) return;

    s_out_running = false;
    // 等输出任务退出（它最多阻塞 200ms）。
    vTaskDelay(pdMS_TO_TICKS(300));
    s_out_task = nullptr;

    i2s_channel_disable(s_tx);
    i2s_del_channel(s_tx);
    s_tx = nullptr;
    s_sample_rate = 0;

    // 清空环中残留。
    if (s_ring != nullptr) {
        size_t sz;
        void *item;
        while ((item = xRingbufferReceive(s_ring, &sz, 0)) != nullptr) {
            vRingbufferReturnItem(s_ring, item);
        }
    }
    ESP_LOGI(kTag, "stream stopped, channel released");
}

// 解码侧调用：压入环形缓冲，0 超时不阻塞。环满丢帧。
int audio_i2s_write(const int16_t *stereo_samples, size_t frame_count,
                    int sample_rate)
{
    if (stereo_samples == nullptr || frame_count == 0 || s_ring == nullptr) {
        return -1;
    }

    // 软件音量（在压入环之前做，输出任务不需要关心音量）。
    app_volume_apply(const_cast<int16_t *>(stereo_samples), frame_count * 2);

    // 组装 item：header + PCM。用栈上临时缓冲——frame_count 最大 1024
    // × 2ch × 2B = 4KB + 8B header，栈深度 6KB 足够。
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
        return 0;  // 丢帧，下帧追上
    }
    return 0;
}
