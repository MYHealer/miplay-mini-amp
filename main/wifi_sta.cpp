#include "wifi_sta.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

namespace {

constexpr char kTag[] = "wifi";

// 事件位。STA 侧的状态机就靠这几个位驱动，比在回调里直接做事更容易推理。
constexpr int kBitConnected = BIT0;
constexpr int kBitDisconnected = BIT1;
constexpr int kBitAuthFailed = BIT2;   // 认证类失败：重试无意义
constexpr int kBitScanDone = BIT3;

EventGroupHandle_t s_events = nullptr;
esp_netif_t *s_sta_netif = nullptr;
esp_netif_t *s_ap_netif = nullptr;
bool s_inited = false;
bool s_ap_started = false;
volatile bool s_connected = false;

// 认证类失败：同一个密码再试一次不会变好，早点告诉用户比耗满超时有用。
//
// 对照表取自 IDF 的 wifi_err_reason_t。分两类：
//   致命 —— 密码错、AP 不存在、握手失败。立刻返回。
//   瞬时 —— 认证过期、关联过期等。交给上层再试。
bool is_fatal_reason(uint8_t reason)
{
    switch (reason) {
        case WIFI_REASON_NO_AP_FOUND:             // 201：扫到了但连不上，多半已走远
        case WIFI_REASON_AUTH_FAIL:               // 202：密码错
        case WIFI_REASON_ASSOC_FAIL:              // 203
        case WIFI_REASON_HANDSHAKE_TIMEOUT:       // 204
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:  // 15：密码错的另一种表现
        case WIFI_REASON_CONNECTION_FAIL:         // 205
        case WIFI_REASON_NOT_AUTHED:              // 6
            return true;
        default:
            return false;
    }
}

void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        xEventGroupSetBits(s_events, kBitScanDone);
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto *d = static_cast<wifi_event_sta_disconnected_t *>(data);
        s_connected = false;
        // 先清 CONNECTED 再置 DISCONNECTED：等待方看到 DISCONNECTED 时，
        // CONNECTED 一定已经是干净的，不会读到"上一次的"成功。
        xEventGroupClearBits(s_events, kBitConnected);
        xEventGroupSetBits(s_events, kBitDisconnected);
        // 把 reason 原样打出来。is_fatal_reason() 把七个 reason 归并成一个
        // 状态位，而页面文案又把它们统一说成"密码不对"——真出问题时，
        // 这个编号是唯一能把"密码错"和"压根没找到 AP / 握手超时"分开的证据。
        const bool fatal = is_fatal_reason(d->reason);
        if (fatal) {
            xEventGroupSetBits(s_events, kBitAuthFailed);
        }
        ESP_LOGW(kTag, "disconnected: reason=%u (%s)", (unsigned)d->reason,
                 fatal ? "fatal" : "transient");
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto *e = static_cast<ip_event_got_ip_t *>(data);
        ESP_LOGI(kTag, "got ip: " IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        xEventGroupSetBits(s_events, kBitConnected);
        return;
    }
}

// 目标 Wi-Fi 模式：开着热点就得是 APSTA（否则扫描不可用），否则纯 STA。
wifi_mode_t desired_mode()
{
    return s_ap_started ? WIFI_MODE_APSTA : WIFI_MODE_STA;
}

esp_err_t ensure_mode(wifi_mode_t want)
{
    wifi_mode_t cur = WIFI_MODE_NULL;
    esp_err_t err = esp_wifi_get_mode(&cur);
    if (err != ESP_OK) {
        return err;
    }
    if (cur == want) {
        return ESP_OK;
    }
    return esp_wifi_set_mode(want);
}

// 在已有记录里找同名 SSID，返回下标；没有则 -1。
int find_ssid(const wifi_ap_record_t *recs, int n, const uint8_t *ssid)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(reinterpret_cast<const char *>(recs[i].ssid),
                   reinterpret_cast<const char *>(ssid)) == 0) {
            return i;
        }
    }
    return -1;
}

}  // namespace

esp_err_t wifi_init_once(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    // NVS 是 Wi-Fi 驱动存校准数据的地方，必须先初始化。
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(kTag, "nvs needs erase, reinitializing");
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_events = xEventGroupCreate();
    if (s_events == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    err = esp_netif_init();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "event loop create failed: %s", esp_err_to_name(err));
        return err;
    }

    // STA netif 只建一次，整个生命周期复用——它在 AP 开关过程中不受影响。
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == nullptr) {
        return ESP_FAIL;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    // instance 出参传 nullptr：本工程从不注销这两个 handler（它们的生命周期
    // 与整个程序相同），拿回句柄反而要额外保存两份无用状态。
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &on_wifi_event, nullptr, nullptr);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &on_wifi_event, nullptr, nullptr);

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "wifi start failed: %s", esp_err_to_name(err));
        return err;
    }

    s_inited = true;
    ESP_LOGI(kTag, "wifi stack ready");
    return ESP_OK;
}

int wifi_scan(wifi_ap_record_t *out, int max)
{
    if (!s_inited || out == nullptr || max <= 0) {
        return -1;
    }

    // esp_wifi_scan_start() 在纯 AP 模式下不可用，必须确保 STA 侧在场。
    if (ensure_mode(desired_mode()) != ESP_OK) {
        ESP_LOGE(kTag, "cannot enter scan-capable mode");
        return -1;
    }

    xEventGroupClearBits(s_events, kBitScanDone);

    // show_hidden=false：隐藏 SSID 没有名字可显示，用户也无从选择。
    // 默认扫描参数（active，每信道 120ms）足够快，够用。
    wifi_scan_config_t scan_cfg = {};
    scan_cfg.show_hidden = false;
    scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, false);
    if (err != ESP_OK) {
        // 正在连接时扫描会被拒（ESP_ERR_WIFI_STATE）。配网态下 STA 不该在连，
        // 所以这条日志通常意味着状态机出了问题。
        ESP_LOGE(kTag, "scan_start failed: %s", esp_err_to_name(err));
        return -1;
    }

    const EventBits_t bits = xEventGroupWaitBits(
        s_events, kBitScanDone, pdTRUE, pdFALSE, pdMS_TO_TICKS(8000));
    if ((bits & kBitScanDone) == 0) {
        ESP_LOGW(kTag, "scan timed out");
        esp_wifi_scan_stop();
        return -1;
    }

    uint16_t num = 0;
    err = esp_wifi_scan_get_ap_num(&num);
    if (err != ESP_OK) {
        return -1;
    }
    if (num == 0) {
        return 0;
    }
    // 超出 max 的部分直接丢掉。esp_wifi_scan_get_ap_records() 会把驱动侧的
    // 整张表都释放掉，所以这里不需要额外调用 clear_ap_list()。
    if (num > (uint16_t)max) {
        num = (uint16_t)max;
    }
    err = esp_wifi_scan_get_ap_records(&num, out);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "get_ap_records failed: %s", esp_err_to_name(err));
        return -1;
    }

    // 按信号强度降序（插入排序，n 最多几十条，不值得引 qsort 的比较函数）。
    for (int i = 1; i < (int)num; i++) {
        const wifi_ap_record_t key = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].rssi < key.rssi) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }

    // 同名 SSID 去重：一个路由器常开 2.4G/5G 两个同名 BSS，列表里出现两条
    // 对用户是纯噪音，而 ESP32-S3 只能连 2.4G。保留信号最强的那条。
    int kept = 0;
    for (int i = 0; i < (int)num; i++) {
        if (out[i].ssid[0] == '\0') {
            continue;
        }
        if (find_ssid(out, kept, out[i].ssid) >= 0) {
            // 被丢掉的那条要留痕迹：双频合一时，若丢掉的是 2.4G 而留下 5G，
            // 页面显示的网络名照样正确、密码也对，却永远连不上——
            // 这是"显示密码错误"最容易被误判成密码问题的一种来源。
            ESP_LOGW(kTag, "dedupe dropped \"%.32s\" ch=%u rssi=%d (kept stronger)",
                     reinterpret_cast<const char *>(out[i].ssid),
                     (unsigned)out[i].primary, (int)out[i].rssi);
            continue;
        }
        out[kept++] = out[i];
    }

    ESP_LOGI(kTag, "scan found %d ap(s), %d after dedupe", (int)num, kept);
    return kept;
}

bool wifi_sta_connect(const char *ssid, const char *pass, int timeout_ms)
{
    if (!s_inited || ssid == nullptr || ssid[0] == '\0') {
        return false;
    }
    const char *pwd = pass != nullptr ? pass : "";

    wifi_config_t wcfg = {};
    // 长度上限取自 wifi_sta_config_t 的数组尺寸（ssid[32] / password[64]）。
    // 超长时 esp_wifi_set_config() 只会静默截断：设备照常"启动成功"，
    // 只是永远连不上一个不存在的 AP，日志里毫无异常。在拷贝前显式拦掉。
    if (strlen(ssid) >= sizeof(wcfg.sta.ssid) ||
        strlen(pwd) >= sizeof(wcfg.sta.password)) {
        ESP_LOGE(kTag, "credentials too long: ssid=%u/%u pass=%u/%u",
                 (unsigned)strlen(ssid), (unsigned)sizeof(wcfg.sta.ssid),
                 (unsigned)strlen(pwd), (unsigned)sizeof(wcfg.sta.password));
        return false;
    }
    strncpy(reinterpret_cast<char *>(wcfg.sta.ssid), ssid,
            sizeof(wcfg.sta.ssid) - 1);
    strncpy(reinterpret_cast<char *>(wcfg.sta.password), pwd,
            sizeof(wcfg.sta.password) - 1);
    // OPEN = 0，阈值判定是 authmode >= threshold，所以置 OPEN 等于"接受任何
    // 加密方式"。这里是对的：AP 是用户从扫描列表里亲手挑的，不该因为
    // WPA3/WPA2 混用之类的原因被我们自己的阈值挡掉。
    wcfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    if (ensure_mode(desired_mode()) != ESP_OK) {
        return false;
    }

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wcfg);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "set_config failed: %s", esp_err_to_name(err));
        return false;
    }

    xEventGroupClearBits(s_events,
                         kBitConnected | kBitDisconnected | kBitAuthFailed);

    ESP_LOGI(kTag, "connecting to \"%s\" ...", ssid);
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        return false;
    }

    // 等三种结局里的任意一种：拿到 IP、认证失败、超时。
    // 用 pdFALSE 的 clear-on-exit：位留给上层观察，不由本函数消费。
    const EventBits_t bits = xEventGroupWaitBits(
        s_events, kBitConnected | kBitAuthFailed | kBitDisconnected, pdFALSE,
        pdFALSE, pdMS_TO_TICKS(timeout_ms));

    if (bits & kBitConnected) {
        return true;
    }

    // 失败路径统一收尾：停掉 STA，避免它继续在后台尝试连接、
    // 拖慢后续的扫描（扫描与连接互斥）。
    esp_wifi_disconnect();

    if (bits & kBitAuthFailed) {
        ESP_LOGW(kTag, "auth failed — wrong password or AP unreachable");
    } else if ((bits & kBitDisconnected) == 0) {
        ESP_LOGW(kTag, "connect timed out after %d ms", timeout_ms);
    } else {
        ESP_LOGW(kTag, "connect failed");
    }
    return false;
}

// 名字不能叫 wifi_sta_disconnect：libnet80211.a 里有同名全局符号。
// 内部调用的仍是 IDF 的 esp_wifi_disconnect()，两者不冲突。
void wifi_sta_drop(void)
{
    if (!s_inited) {
        return;
    }
    esp_wifi_disconnect();
    s_connected = false;
}

bool wifi_is_connected(void)
{
    return s_connected;
}

esp_err_t wifi_ap_start(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ap_started) {
        return ESP_OK;
    }

    // 先切到 APSTA。顺序很重要：netif 要在模式已经包含 AP 之后建，
    // 否则 attach 会失败。
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "set APSTA failed: %s", esp_err_to_name(err));
        return err;
    }

    if (s_ap_netif == nullptr) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_ap_netif == nullptr) {
            return ESP_FAIL;
        }
    }

    // SSID 取 MAC 后两字节。同名多台设备时能一眼区分是哪一台——
    // 没有这个后缀，两台设备的开放热点会同名，手机只会连上其中一台。
    uint8_t mac[6] = {};
    esp_wifi_get_mac(WIFI_IF_AP, mac);

    wifi_config_t apcfg = {};
    snprintf(reinterpret_cast<char *>(apcfg.ap.ssid),
             sizeof(apcfg.ap.ssid), "MiPlay-Amp-%02X%02X", mac[4], mac[5]);
    apcfg.ap.ssid_len = (uint8_t)strlen(
        reinterpret_cast<const char *>(apcfg.ap.ssid));
    // 开放热点：配网页面本身无敏感信息，而"输对热点密码"这道门槛
    // 只会让第一次配网变难，不会让设备更安全。
    apcfg.ap.password[0] = '\0';
    apcfg.ap.authmode = WIFI_AUTH_OPEN;
    apcfg.ap.max_connection = 4;
    // 信道 1。APSTA 下 AP 信道必须与 STA 一致，STA 未连接时 IDF 用它作默认；
    // 等 STA 连上后驱动会把 AP 一起挪过去。
    apcfg.ap.channel = 1;

    err = esp_wifi_set_config(WIFI_IF_AP, &apcfg);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "AP set_config failed: %s", esp_err_to_name(err));
        return err;
    }

    s_ap_started = true;
    ESP_LOGI(kTag, "provisioning AP up: \"%s\" (open, http://192.168.4.1)",
             reinterpret_cast<const char *>(apcfg.ap.ssid));
    return ESP_OK;
}

void wifi_ap_stop(void)
{
    if (!s_inited || !s_ap_started) {
        return;
    }

    // 先退回纯 STA，再销毁 netif。反过来的话 netif 还挂在 AP 接口上，
    // 驱动状态会不一致。
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_ap_started = false;

    if (s_ap_netif != nullptr) {
        // 必须销毁：esp_netif_create_default_wifi_ap() 不能对同一个接口
        // 建两次，留着的话下次 wifi_ap_start() 会直接 assert 掉。
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = nullptr;
    }
    ESP_LOGI(kTag, "provisioning AP down");
}