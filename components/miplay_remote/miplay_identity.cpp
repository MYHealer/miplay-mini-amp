// 设备身份生成与基础工具。
//
// 身份参数的派生方式与参考实现逐字节一致——手机侧会校验 idHash 与 MAC 的
// 一致性，任何偏差都会导致设备无法被发现。唯二改动：
//   1. 显示名换成本项目的名字（name 是自由文本，唯一硬约束是 4 字节内的
//      name_len 前缀；FusionPlay-Android 的 name= 字段无长度要求）
//   2. 去掉本项目不需要的 hex dump 调试函数
//
// idHash 的权威定义（FusionPlay-Android discovery.rs:2055-2058）：
//   idHash = 标准 base64(short_id)，其中 short_id = base64url(SHA256(seed))[:3]
//   即 3 字节的 base64 → 恒为 4 字符。别把 short_id(3 字符) 当成 idHash 发出。
#include "miplay_remote_internal.h"

#include <stdio.h>
#include <string.h>

#include "esp_efuse.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/md5.h"
#include "mbedtls/sha256.h"

static const char *TAG = "miplay_remote";

char s_device_id[16];
char s_inst_name[40];
char s_idhash[8];
char s_appsdata[16];
char s_mac_b64[12];
char s_lyra_appdata[96];
uint8_t s_mac[6];
volatile bool s_running = false;

namespace {
constexpr char kB64Tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr char kB64UrlTab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
}  // namespace

size_t b64_encode_3(const unsigned char in[3], char *out)
{
    unsigned int v = ((unsigned)in[0] << 16) | ((unsigned)in[1] << 8) | in[2];
    out[0] = kB64Tab[(v >> 18) & 63];
    out[1] = kB64Tab[(v >> 12) & 63];
    out[2] = kB64Tab[(v >> 6) & 63];
    out[3] = kB64Tab[v & 63];
    out[4] = 0;
    return 4;
}

uint32_t miplay_get_my_ipv4(void)
{
    esp_netif_ip_info_t ip;
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (n && esp_netif_get_ip_info(n, &ip) == ESP_OK) {
        return ip.ip.addr;
    }
    return 0x7F000001UL;
}

void miplay_init_device_identity(void)
{
    // 幂等保护：身份只在首次调用时计算。
    if (s_device_id[0] != '\0') {
        return;
    }

    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    memcpy(s_mac, mac, 6);

    // device_id = MAC 后 4 字节的大写 hex（8 字符），也是后续哈希的种子。
    snprintf(s_device_id, sizeof(s_device_id), "%02X%02X%02X%02X",
             mac[2], mac[3], mac[4], mac[5]);

    // idHash 三步派生（参考实现 Rust 的 idm_identity 逻辑）：
    //   1. SHA256(device_id) 前 3 字节 → base64url → short_id(3 字符)
    //   2. idHash = 标准 base64(short_id 的 3 个 ASCII 字节)
    // 第 2 步是把 3 个 ASCII 字符当 3 字节直接做标准 base64，不做位索引，
    // 因此结果恰好 4 字符。
    {
        uint8_t hash[32];
        mbedtls_sha256(reinterpret_cast<const unsigned char *>(s_device_id),
                       strlen(s_device_id), hash, 0);

        uint32_t v = ((uint32_t)hash[0] << 16) | ((uint32_t)hash[1] << 8) | hash[2];
        char short_id[4];
        short_id[0] = kB64UrlTab[(v >> 18) & 63];
        short_id[1] = kB64UrlTab[(v >> 12) & 63];
        short_id[2] = kB64UrlTab[(v >> 6) & 63];
        short_id[3] = '\0';

        uint32_t sv = ((uint32_t)(uint8_t)short_id[0] << 16) |
                      ((uint32_t)(uint8_t)short_id[1] << 8) |
                      (uint32_t)(uint8_t)short_id[2];
        s_idhash[0] = kB64Tab[(sv >> 18) & 63];
        s_idhash[1] = kB64Tab[(sv >> 12) & 63];
        s_idhash[2] = kB64Tab[(sv >> 6) & 63];
        s_idhash[3] = kB64Tab[sv & 63];
        s_idhash[4] = '\0';
    }

    // inst_name = "<显示名>(<SHA256 前 10 个 base64url 字符>)"
    // 后缀随 MAC 变化，确保同网内不同设备的实例名唯一。
    {
        uint8_t hash[32];
        mbedtls_sha256(reinterpret_cast<const unsigned char *>(s_device_id),
                       strlen(s_device_id), hash, 0);
        char suffix[11];
        for (int i = 0; i < 10; i++) {
            int byte_idx = (i * 6) / 8;
            int bit_offset = (i * 6) % 8;
            uint32_t val = ((uint32_t)hash[byte_idx] << 16);
            if (byte_idx + 1 < 32) val |= ((uint32_t)hash[byte_idx + 1] << 8);
            if (byte_idx + 2 < 32) val |= hash[byte_idx + 2];
            suffix[i] = kB64UrlTab[(val >> (18 - bit_offset)) & 63];
        }
        suffix[10] = '\0';
        snprintf(s_inst_name, sizeof(s_inst_name), "%s(%s)",
                 MIPLAY_DEVICE_DISPLAY_NAME, suffix);
    }

    // appsData 是硬编码校验值，不能用 PC 端自算的描述符替代：
    // 那样会让 HyperOS 设置 hasLyraBymiplay=true 并把接收端排除在音频
    // picker 之外。解码后为 81 00 04 04 83 22 C3，0x0483 是 Lyra 能力位，
    // 0x22C3 是控制端口 8899。
    memcpy(s_appsdata, "gQAEBIMiww==", sizeof(s_appsdata));

    // MAC 的 base64（用于 TXT 的 mac 字段）。
    b64_encode_3(mac, s_mac_b64);
    b64_encode_3(mac + 3, s_mac_b64 + 4);
    s_mac_b64[8] = '\0';

    // lyra AppData：二进制结构 + base64。逐字节固定，其中：
    //   [2] = 0x03 表示申报 TV（0x15 是 PC，会导致遥控器面板不出现）
    //   [3..6] = MAC 后 4 字节
    //   末段 name 为长度前缀 + 内容
    {
        uint8_t appdata[80];
        int o = 0;
        const char *dev_name = MIPLAY_DEVICE_DISPLAY_NAME;
        const int name_len = (int)strlen(dev_name);

        appdata[o++] = 0x00;
        appdata[o++] = 0x40;
        appdata[o++] = 0x03;  // TV，不是 0x15(PC)
        appdata[o++] = mac[2];
        appdata[o++] = mac[3];
        appdata[o++] = mac[4];
        appdata[o++] = mac[5];
        static const uint8_t kFixed[] = {
            0x00, 0x05, 0x19, 0x24,
            0x10, 0x01, 0x03, 0x0a, 0x03, 0x01, 0xda, 0xae, 0x01, 0x01, 0x80, 0x02,
        };
        memcpy(appdata + o, kFixed, sizeof(kFixed));
        o += sizeof(kFixed);
        appdata[o++] = (uint8_t)name_len;
        memcpy(appdata + o, dev_name, name_len);
        o += name_len;
        appdata[o++] = 0x25;
        appdata[o++] = 0x01;
        appdata[o++] = 0x03;

        int bi = 0;
        for (int i = 0; i + 2 < o; i += 3) {
            uint32_t v = ((uint32_t)appdata[i] << 16) |
                         ((uint32_t)appdata[i + 1] << 8) | appdata[i + 2];
            s_lyra_appdata[bi++] = kB64Tab[(v >> 18) & 63];
            s_lyra_appdata[bi++] = kB64Tab[(v >> 12) & 63];
            s_lyra_appdata[bi++] = kB64Tab[(v >> 6) & 63];
            s_lyra_appdata[bi++] = kB64Tab[v & 63];
        }
        const int rem = o % 3;
        if (rem == 1) {
            uint32_t v = (uint32_t)appdata[o - 1] << 16;
            s_lyra_appdata[bi++] = kB64Tab[(v >> 18) & 63];
            s_lyra_appdata[bi++] = kB64Tab[(v >> 12) & 63];
            s_lyra_appdata[bi++] = '=';
            s_lyra_appdata[bi++] = '=';
        } else if (rem == 2) {
            uint32_t v = ((uint32_t)appdata[o - 2] << 16) |
                         ((uint32_t)appdata[o - 1] << 8);
            s_lyra_appdata[bi++] = kB64Tab[(v >> 18) & 63];
            s_lyra_appdata[bi++] = kB64Tab[(v >> 12) & 63];
            s_lyra_appdata[bi++] = kB64Tab[(v >> 6) & 63];
            s_lyra_appdata[bi++] = '=';
        }
        s_lyra_appdata[bi] = '\0';
    }

    ESP_LOGI(TAG, "identity: id=%s inst=%s idHash=%s",
             s_device_id, s_inst_name, s_idhash);
}
