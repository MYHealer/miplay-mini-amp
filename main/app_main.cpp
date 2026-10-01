// 启动编排。职责有三件：按正确顺序初始化各层、把回调接到实现上、
// 以及跑一个 Wi-Fi 状态机决定"该连网还是该开热点"。
//
// 顺序有讲究：Wi-Fi 必须先拿到 IP，miplay_remote 的 mDNS 注册需要它；
// 回调必须在 miplay_remote_start() 之前注册，否则首个控制会话的
// 连接事件会被丢掉（表现是"连上了但灯不变"）。

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "app_volume.h"
#include "audio_i2s.h"
#include "miplay_remote.h"
#include "status_led.h"
#include "web_prov.h"
#include "wifi_creds.h"
#include "wifi_sta.h"

namespace {

constexpr char kTag[] = "app_main";

#ifndef CONFIG_MIPLAY_DEFAULT_VOLUME
#define CONFIG_MIPLAY_DEFAULT_VOLUME 70
#endif
#ifndef CONFIG_MIPLAY_WIFI_MAX_RETRY
#define CONFIG_MIPLAY_WIFI_MAX_RETRY 3
#endif

// 单次连接等待上限。与配网页上写的"最多等 15 秒"必须一致，
// 否则用户会在设备其实还在尝试的时候就被告知失败。
constexpr int kConnectTimeoutMs = 15000;

// 配网成功后等多久再拆热点。页面已经收到 200 响应，但 TCP 发送缓冲里
// 可能还有没落地的字节；立刻拆 netif 会让手机看到"请求中断"，
// 而设备其实已经连上了。
constexpr int kHttpDrainMs = 1000;

// 配网失败后的重试间隔。用错误灯闪一会儿再重来，而不是空转刷屏日志。
constexpr int kErrorRetryMs = 5000;

// 在线状态下探测掉线的周期。Wi-Fi 掉线本身没有回调暴露给这一层，
// 两秒一次的 volatile 读取代价可以忽略，换来的是不需要再给 wifi_sta
// 加一套事件订阅 API。
constexpr int kOnlinePollMs = 2000;

EventGroupHandle_t s_events = nullptr;
constexpr int kBitProvisioned = BIT0;

// web_prov 在 HTTP 任务上下文回调这里，所以只做"记录 + 置位"两件不阻塞的事。
// 真正的状态迁移（关热点、起 MiPlay）由 supervisor 任务执行。
char s_new_ssid[33] = {};

void on_provisioned(const char *ssid)
{
    strncpy(s_new_ssid, ssid, sizeof(s_new_ssid) - 1);
    s_new_ssid[sizeof(s_new_ssid) - 1] = '\0';
    xEventGroupSetBits(s_events, kBitProvisioned);
}

// ── 音频输出 ──

// 签名必须与 miplay_write_speaker_fn 完全一致。
int on_speaker_write(const int16_t *stereo_samples, size_t frame_count,
                     int sample_rate)
{
    return audio_i2s_write(stereo_samples, frame_count, sample_rate);
}

// 返回 false 会中止本次流（手机端表现为投屏立即结束）。
bool on_stream_start(void)
{
    return audio_i2s_stream_start();
}

void on_stream_stop(void)
{
    audio_i2s_stream_stop();
}

// ── 音量 ──

void on_volume_set(int percent)
{
    app_volume_set(percent);
}

int on_volume_get(void)
{
    return app_volume_get();
}

// ── 状态灯 ──

void on_connection_changed(bool connected)
{
    status_led_set(connected ? STATUS_CONNECTED : STATUS_IDLE);
}

void on_pause_changed(bool paused)
{
    // 暂停时退回"已连接"，恢复播放则回到"播放中"。
    status_led_set(paused ? STATUS_CONNECTED : STATUS_PLAYING);
}

// 无屏幕，元数据只记日志。保留回调是为了让协议层拿到 title/artist
// 时不至于走空指针分支，也方便调试时确认手机确实推了信息。
void on_media_meta(const char *title, const char *artist, const char *album,
                   int64_t duration_ms, int64_t position_ms)
{
    ESP_LOGI(kTag, "meta: title=\"%s\" artist=\"%s\" album=\"%s\" "
                   "%lld/%lld ms",
             title ? title : "", artist ? artist : "", album ? album : "",
             (long long)position_ms, (long long)duration_ms);
}

// ── Wi-Fi 状态机 ──

// 用 NVS 里的凭据连网，最多试 CONFIG_MIPLAY_WIFI_MAX_RETRY 轮。
// 返回 true 表示已拿到 IP。
bool connect_with_saved_creds()
{
    char ssid[33] = {};
    char pass[64] = {};
    if (!wifi_creds_load(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGI(kTag, "no saved credentials — provisioning required");
        return false;
    }

    status_led_set(STATUS_WIFI);
    for (int i = 0; i < CONFIG_MIPLAY_WIFI_MAX_RETRY; i++) {
        if (wifi_sta_connect(ssid, pass, kConnectTimeoutMs)) {
            ESP_LOGI(kTag, "online as \"%s\" (attempt %d)", ssid, i + 1);
            return true;
        }
        ESP_LOGW(kTag, "connect attempt %d/%d failed",
                 i + 1, CONFIG_MIPLAY_WIFI_MAX_RETRY);
    }
    return false;
}

// 开热点 + 起配网 Web 后台，阻塞到用户配好网为止。返回 false 表示
// 起热点或起服务器就失败了（不是"用户没配"——那会一直等下去）。
bool run_provisioning()
{
    status_led_set(STATUS_AP);

    esp_err_t err = wifi_ap_start();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "AP start failed: %s", esp_err_to_name(err));
        return false;
    }
    err = web_prov_start(on_provisioned);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "web server start failed: %s", esp_err_to_name(err));
        wifi_ap_stop();
        return false;
    }

    ESP_LOGI(kTag, "waiting for provisioning — connect to the open AP "
                   "and open http://192.168.4.1");

    xEventGroupClearBits(s_events, kBitProvisioned);
    xEventGroupWaitBits(s_events, kBitProvisioned, pdTRUE, pdFALSE,
                        portMAX_DELAY);

    vTaskDelay(pdMS_TO_TICKS(kHttpDrainMs));
    web_prov_stop();
    wifi_ap_stop();
    ESP_LOGI(kTag, "provisioning done: ssid=\"%s\"", s_new_ssid);
    return true;
}

// 已联网：点亮蓝灯，把 MiPlay 服务挂上去。
// miplay_remote_start() 是幂等的，所以掉线重连后重复调用没有副作用。
void go_online()
{
    status_led_set(STATUS_IDLE);
    const esp_err_t err = miplay_remote_start();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "miplay start failed: %s", esp_err_to_name(err));
        status_led_set(STATUS_ERROR);
    }
}

void supervisor_task(void *)
{
    for (;;) {
        bool online = connect_with_saved_creds();

        if (!online) {
            if (!run_provisioning()) {
                status_led_set(STATUS_ERROR);
                vTaskDelay(pdMS_TO_TICKS(kErrorRetryMs));
                continue;
            }
            // 配网成功。web_prov 内部已经完成了一次成功连接，但
            // wifi_ap_stop() 刚把模式从 APSTA 切回 STA——万一那次连接
            // 没保住，就回顶部用刚存下的凭据重走一遍。
            if (!wifi_is_connected()) {
                ESP_LOGW(kTag, "connection lost while closing AP, retrying");
                continue;
            }
            online = true;
        }

        if (online) {
            go_online();
        }

        // 在线保持。掉线就回顶部重试——同一个循环同时覆盖了
        // "路由器重启"和"用户改了密码"两种情况，不需要两套重试逻辑。
        while (wifi_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(kOnlinePollMs));
        }
        ESP_LOGW(kTag, "wifi lost, reconnecting");
    }
}

}  // namespace

extern "C" void app_main(void)
{
    ESP_LOGI(kTag, "=== MiPlay Mini Amp ===");

    status_led_init();
    status_led_set(STATUS_BOOT);

    app_volume_init(CONFIG_MIPLAY_DEFAULT_VOLUME);

    // 回调必须在启动服务之前注册：手机连上来的那一刻就会触发
    // connection_changed，晚注册等于丢掉首个事件。
    miplay_media_set_speaker_callback(on_speaker_write);
    miplay_media_set_stream_callback(on_stream_start, on_stream_stop);
    miplay_media_set_volume_callback(on_volume_set, on_volume_get);
    miplay_media_set_meta_callback(on_media_meta);
    miplay_media_set_pause_callback(on_pause_changed);
    miplay_remote_set_connection_callback(on_connection_changed);

    // NVS / netif / 事件循环 / Wi-Fi 驱动都在这里就位。凭据读写和
    // 热点开关都依赖它。
    if (wifi_init_once() != ESP_OK) {
        ESP_LOGE(kTag, "wifi init failed, halting");
        status_led_set(STATUS_ERROR);
        return;
    }

    s_events = xEventGroupCreate();
    if (s_events == nullptr) {
        ESP_LOGE(kTag, "event group alloc failed, halting");
        status_led_set(STATUS_ERROR);
        return;
    }

    // 6KB：状态机本身很浅，但 wifi_sta_connect() 会一路走到驱动内部，
    // 那几层调用的栈深度不由我们控制。给足余量比事后追一个随机栈溢出便宜。
    if (xTaskCreate(supervisor_task, "wifi_sup", 6144, nullptr, 5, nullptr) !=
        pdPASS) {
        ESP_LOGE(kTag, "supervisor task create failed, halting");
        status_led_set(STATUS_ERROR);
        return;
    }

    // 后续一切由 supervisor 与各服务任务驱动。app_main 可以退出——
    // FreeRTOS 会回收它的栈，把 8KB 还给系统。
    ESP_LOGI(kTag, "boot complete, handing off to supervisor");
}
