#include "wifi_creds.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace {

constexpr char kTag[] = "wifi_creds";

// 单独一个 namespace，与 Wi-Fi 驱动自己用的 "nvs.net80211" 分开。
// 混在一起的话，想"清除配网信息"就得动驱动的地盘，风险不值得。
constexpr char kNamespace[] = "miplay_wifi";
constexpr char kKeySsid[] = "ssid";
constexpr char kKeyPass[] = "pass";

// 打开 namespace。不存在时返回 nullptr——"没配过网"不是错误。
nvs_handle_t open_readonly()
{
    nvs_handle_t h = 0;
    if (nvs_open(kNamespace, NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }
    return h;
}

nvs_handle_t open_readwrite()
{
    nvs_handle_t h = 0;
    const esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs_open failed: %s", esp_err_to_name(err));
        return 0;
    }
    return h;
}

// 读一个字符串键。缓冲不足或键不存在都返回 false。
bool read_str(nvs_handle_t h, const char *key, char *out, size_t out_len)
{
    if (h == 0 || out == nullptr || out_len == 0) {
        return false;
    }
    size_t len = out_len;
    const esp_err_t err = nvs_get_str(h, key, out, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return false;
    }
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "nvs_get_str(%s) failed: %s", key, esp_err_to_name(err));
        return false;
    }
    return true;
}

}  // namespace

bool wifi_creds_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    // 先清空，让"读不到"与"读到空串"表现一致——调用方只需判断返回值。
    if (ssid != nullptr && ssid_len > 0) {
        ssid[0] = '\0';
    }
    if (pass != nullptr && pass_len > 0) {
        pass[0] = '\0';
    }

    const nvs_handle_t h = open_readonly();
    if (h == 0) {
        return false;
    }

    // 密码键允许缺失：开放网络（无密码）是合法的配置，
    // 此时 ssid 存在而 pass 不存在，不能因此判为"未配网"。
    if (pass != nullptr && pass_len > 0) {
        read_str(h, kKeyPass, pass, pass_len);
    }
    const bool ok = read_str(h, kKeySsid, ssid, ssid_len);
    nvs_close(h);

    if (!ok || ssid[0] == '\0') {
        return false;
    }
    return true;
}

bool wifi_creds_save(const char *ssid, const char *pass)
{
    if (ssid == nullptr || ssid[0] == '\0') {
        return false;
    }

    const nvs_handle_t h = open_readwrite();
    if (h == 0) {
        return false;
    }

    esp_err_t err = nvs_set_str(h, kKeySsid, ssid);
    if (err == ESP_OK) {
        // 空密码也要写：否则残留的旧密码会跟着新的开放网络一起被用上。
        err = nvs_set_str(h, kKeyPass, pass != nullptr ? pass : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err != ESP_OK) {
        ESP_LOGE(kTag, "save failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(kTag, "credentials saved (ssid=\"%s\")", ssid);
    return true;
}

bool wifi_creds_clear(void)
{
    const nvs_handle_t h = open_readwrite();
    if (h == 0) {
        return false;
    }
    esp_err_t err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}