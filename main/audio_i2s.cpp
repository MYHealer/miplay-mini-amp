#include "audio_i2s.h"

#include "app_volume.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

namespace {

constexpr char kTag[] = "audio_i2s";

// SuperMini 上 GPIO 1-13 可用（避开 USB-JTAG 的 19/20、strapping 的 0/3/45/46、
// 片内 Flash/PSRAM 的 26-32）。本工程用 6-9，全在 P2 区（无限制）。
// 下面的兜底值只在 sdkconfig 缺失时生效，正常构建走 Kconfig 的值。
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

// DMA 描述符 × 帧数。6 × 240 帧 ≈ 1440 帧 ≈ 30ms @48kHz 立体声。
// 这 30ms 就是本工程的抖动缓冲与流量控制：DMA 满时 i2s_channel_write()
// 自然阻塞，反过来给网络收包留出喘息。
constexpr int kDmaDescNum = 6;
constexpr int kDmaFrameNum = 240;

i2s_chan_handle_t s_tx = nullptr;
// 0 表示当前无活跃流。用 uint32_t 而不是 int：I2S_STD_CLK_DEFAULT_CONFIG()
// 展开成指定初始化器，而 sample_rate_hz 字段在 IDF v5.5 是 uint32_t。
// 传 int 进去在 C++ 里是窄化转换，-Werror=narrowing 直接编译失败；
// 在这里把类型改对，比在宏参数上强转更诚实。
uint32_t s_sample_rate = 0;

// 互斥锁：串行化 stream_start / stream_stop / write 三条路径。
// 没有它的后果（实测踩过）：miplay_rtsp 任务在 i2s_channel_write 里阻塞等 DMA，
// 控制会话任务同时调 stream_stop → i2s_del_channel 销毁内部队列 →
// spinlock_acquire 断言 lock->count == 0 炸掉。
SemaphoreHandle_t s_i2s_mutex = nullptr;

// 组装 I2S 配置。mclk 用 I2S_GPIO_UNUSED 时驱动不会占用该引脚。
void fill_std_config(i2s_std_config_t *cfg, uint32_t sample_rate)
{
    *cfg = {};
    cfg->clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    cfg->slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    cfg->gpio_cfg.bclk = (gpio_num_t)CONFIG_MIPLAY_I2S_BCLK_GPIO;
    cfg->gpio_cfg.ws = (gpio_num_t)CONFIG_MIPLAY_I2S_LRCK_GPIO;
    cfg->gpio_cfg.dout = (gpio_num_t)CONFIG_MIPLAY_I2S_DIN_GPIO;
#ifdef CONFIG_MIPLAY_I2S_ENABLE_MCLK
    cfg->gpio_cfg.mclk = (gpio_num_t)CONFIG_MIPLAY_I2S_MCLK_GPIO;
#else
    cfg->gpio_cfg.mclk = I2S_GPIO_UNUSED;
#endif
    cfg->gpio_cfg.invert_flags = {};
}

}  // namespace

bool audio_i2s_stream_start(void)
{
    if (s_i2s_mutex == nullptr) {
        s_i2s_mutex = xSemaphoreCreateMutex();
        if (s_i2s_mutex == nullptr) {
            ESP_LOGE(kTag, "mutex alloc failed");
            return false;
        }
    }

    xSemaphoreTake(s_i2s_mutex, portMAX_DELAY);

    if (s_tx != nullptr) {
        // 重复调用是正常的：media_task 与 RTSP 循环各会 acquire 一次。
        // 通道已存在时不能重建（会打断正在播放的流），但这必须留日志——
        // 否则"启动被跳过"和"启动成功"在日志里长得一模一样。
        ESP_LOGI(kTag, "stream_start: channel already exists, reusing");
        xSemaphoreGive(s_i2s_mutex);
        return true;
    }

    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = kDmaDescNum;
    chan_cfg.dma_frame_num = kDmaFrameNum;
    chan_cfg.auto_clear = true;  // 无数据时输出 0，避免残留噪声

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "i2s_new_channel failed: %s", esp_err_to_name(err));
        s_tx = nullptr;
        xSemaphoreGive(s_i2s_mutex);
        return false;
    }

    // 先用 48kHz 建通道占位。真实采样率在首帧 PCM 到达时由
    // audio_i2s_write() 按实测值重配——AAC 流开始前无从得知。
    i2s_std_config_t std_cfg;
    fill_std_config(&std_cfg, 48000);
    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "i2s_channel_init_std_mode failed: %s",
                 esp_err_to_name(err));
        i2s_del_channel(s_tx);
        s_tx = nullptr;
        xSemaphoreGive(s_i2s_mutex);
        return false;
    }

    err = i2s_channel_enable(s_tx);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "i2s_channel_enable failed: %s", esp_err_to_name(err));
        i2s_del_channel(s_tx);
        s_tx = nullptr;
        xSemaphoreGive(s_i2s_mutex);
        return false;
    }

    s_sample_rate = 48000;
#ifdef CONFIG_MIPLAY_I2S_ENABLE_MCLK
    ESP_LOGI(kTag, "stream started: BCLK=%d LRCK=%d DIN=%d MCLK=%d",
             CONFIG_MIPLAY_I2S_BCLK_GPIO, CONFIG_MIPLAY_I2S_LRCK_GPIO,
             CONFIG_MIPLAY_I2S_DIN_GPIO, CONFIG_MIPLAY_I2S_MCLK_GPIO);
#else
    ESP_LOGI(kTag, "stream started: BCLK=%d LRCK=%d DIN=%d (no MCLK)",
             CONFIG_MIPLAY_I2S_BCLK_GPIO, CONFIG_MIPLAY_I2S_LRCK_GPIO,
             CONFIG_MIPLAY_I2S_DIN_GPIO);
#endif
    xSemaphoreGive(s_i2s_mutex);
    return true;
}

void audio_i2s_stream_stop(void)
{
    if (s_i2s_mutex == nullptr) {
        return;
    }
    xSemaphoreTake(s_i2s_mutex, portMAX_DELAY);
    if (s_tx == nullptr) {
        xSemaphoreGive(s_i2s_mutex);
        return;
    }
    i2s_channel_disable(s_tx);
    i2s_del_channel(s_tx);
    s_tx = nullptr;
    s_sample_rate = 0;
    ESP_LOGI(kTag, "stream stopped, channel released");
    xSemaphoreGive(s_i2s_mutex);
}

int audio_i2s_write(const int16_t *stereo_samples, size_t frame_count,
                    int sample_rate)
{
    // 静默失败是最难查的一种。这里三条前置条件各自留一句日志——
    // "没声音且毫无输出"和"没声音但说是 mutex 没建"是完全不同的病因。
    if (s_i2s_mutex == nullptr) {
        static bool s_warned = false;
        if (!s_warned) {
            s_warned = true;
            ESP_LOGE(kTag, "write before stream_start (mutex not created) — "
                           "audio silently dropped");
        }
        return -1;
    }
    if (stereo_samples == nullptr || frame_count == 0) {
        return -1;
    }

    // 整个写路径持锁：防止 stream_stop 在 i2s_channel_write 阻塞期间
    // 销毁通道（curoredump 实测根因）。
    xSemaphoreTake(s_i2s_mutex, portMAX_DELAY);

    if (s_tx == nullptr) {
        static bool s_warned_no_chan = false;
        if (!s_warned_no_chan) {
            s_warned_no_chan = true;
            ESP_LOGE(kTag, "write with no channel (s_tx=null) — dropped");
        }
        xSemaphoreGive(s_i2s_mutex);
        return -1;
    }

    // 采样率自适应。AAC 流中途可能从 44100 切到 48000（切歌/换源）。
    //
    // sample_rate 在边界上校验并转成 uint32_t：公开签名是 int（协议层的
    // 回调类型如此），但 <=0 对 I2S 无意义——I2S_STD_CLK_DEFAULT_CONFIG()
    // 会把它塞进 uint32_t 字段，负值变成一个天文数字，驱动要么拒绝要么
    // 配出一个荒谬的时钟。在进入驱动之前就挡掉。
    if (sample_rate > 0 && (uint32_t)sample_rate != s_sample_rate) {
        const auto rate = static_cast<uint32_t>(sample_rate);
        i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(rate);
        // 顺序不可颠倒：IDF v5.5 的 i2s_channel_reconfig_std_clock() 在通道
        // 已启动时不会改变状态，必须先 disable 再重配再 enable。
        // 省略 disable 的后果是重配静默失败——输出仍是旧采样率，声音变调。
        // 见 driver/i2s_std.h 该函数上方注释。
        i2s_channel_disable(s_tx);
        esp_err_t err = i2s_channel_reconfig_std_clock(s_tx, &clk_cfg);
        i2s_channel_enable(s_tx);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "reconfig clock to %u failed: %s", (unsigned)rate,
                     esp_err_to_name(err));
        } else {
            ESP_LOGI(kTag, "sample rate -> %u Hz", (unsigned)rate);
            s_sample_rate = rate;
        }
    }

    // 软件音量。在 int16 上原地缩放，用 int32 中间量防溢出。
    //
    // 关于 const_cast：协议层的 pcm_buf 是复用的成员缓冲，但每次
    // decode_and_play() 都会完整覆盖 decoded_size 字节，且回调返回后
    // 不再读取它；解码器的重叠变换状态维护在 esp_aac_dec 的 handle
    // 内部，不在这个输出缓冲里。因此原地修改安全。
    //
    // ⚠ 已知边界：协议层按 AAC 实际声道数折算 frame_count
    //    （单声道流时 frame_count = 单声道样本数）。本函数固定按立体声
    //    处理（frame_count × 2 个样本），单声道流会多读一倍数据。
    //    手机投屏音频为立体声，暂不处理；若日后遇到单声道源，
    //    需在协议层做上混或在本层加声道数参数。
    app_volume_apply(const_cast<int16_t *>(stereo_samples), frame_count * 2);

    const size_t bytes = frame_count * 2 * sizeof(int16_t);
    size_t written = 0;
    // 带超时的阻塞写：DMA 满时在此等待，天然形成背压。
    // 超时给足 1 秒——短超时会在大突发下误判为失败并丢音频。
    const esp_err_t err = i2s_channel_write(s_tx, stereo_samples, bytes,
                                           &written, pdMS_TO_TICKS(1000));
    xSemaphoreGive(s_i2s_mutex);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "i2s write failed: %s", esp_err_to_name(err));
        return -1;
    }
    return 0;
}
