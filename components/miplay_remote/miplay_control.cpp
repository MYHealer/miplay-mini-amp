// MiPlay 控制通道实现：8899 监听 + 握手状态机（第一部分：工具与握手材料）。
//
// 分层可见性：本文件依赖 miplay_proto（帧/密码学）与 miplay_session（会话状态），
// 但两者都不反向依赖本文件。UI 侧只经由 miplay_remote 的启动/停止入口，
// 不包含本头文件——避免把 LVGL 拖进协议层。
#include "miplay_control.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mbedtls/md5.h"
#include "mbedtls/sha1.h"
#include "mbedtls/sha256.h"

#include "miplay_remote_internal.h"
#include "miplay_safety.h"
#include "miplay_session.h"
#include "miplay_media.h"

namespace {
constexpr char kTag[] = "miplay_ctrl";

// 接收端自报版本。手机侧要求 >= 0x01000000 才继续协商；
// 这个字符串是设备端的展示版本，与二进制版本号不同。
constexpr char kReceiverVersion[] = "2.1.5071614";

// 内存媒体信息体缓冲。NOTIFY 6 需要一段"空媒体信息"，其长度是协议约定的
// 8192（手机据此判断字段区大小），不能按实际内容长度缩减。
constexpr size_t kMediaInfoBufferSize = 8192;

int s_listen_sock = -1;
TaskHandle_t s_listen_task = nullptr;
volatile bool s_control_running = false;
volatile bool s_connected = false;
miplay_connection_changed_fn s_conn_cb = nullptr;

void set_connected(bool val)
{
    if (s_connected == val) return;
    s_connected = val;
    if (s_conn_cb) s_conn_cb(val);
}

// ── 基础工具 ──

void hex_to_lower(const uint8_t *src, size_t len, char *dst)
{
    static const char kHex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        dst[i * 2] = kHex[(src[i] >> 4) & 0x0F];
        dst[i * 2 + 1] = kHex[src[i] & 0x0F];
    }
    dst[len * 2] = '\0';
}

// 分包 hex dump。只在短帧上调用——单条日志过长会被 UART 行缓冲截断，
// 反而看不到关键字节。
void dump_hex(const uint8_t *data, int len, const char *label)
{
    if (!data || len <= 0) {
        return;
    }
    char line[3 * 32 + 1];
    for (int off = 0; off < len; off += 32) {
        const int chunk = (len - off > 32) ? 32 : (len - off);
        int o = 0;
        for (int i = 0; i < chunk; i++) {
            o += snprintf(line + o, sizeof(line) - static_cast<size_t>(o),
                          "%02X ", data[off + i]);
        }
        ESP_LOGI(kTag, "%s[%02X] %s", label, off, line);
    }
}

// ── 握手材料生成 ──

// 会话密钥派生。四元组顺序必须是 (本端 IP, 本端端口, 对端 IP, 对端端口)——
// 手机视角是 remote 在前，若接收端把顺序写反，双方算出的 authKey 不同，
// 表现为"能握手但所有加密帧解不开"。
void derive_session_key(miplay_session_t *session, uint32_t peer_ip,
                        uint16_t peer_port, uint32_t local_ip,
                        uint16_t local_port)
{
    char buf[64];
    int off = 0;
    // s_addr 已是网络字节序（大端），按字节读即是点分十进制的正确顺序。
    const uint8_t *lip = reinterpret_cast<const uint8_t *>(&local_ip);
    off += snprintf(buf + off, sizeof(buf) - static_cast<size_t>(off),
                    "%u.%u.%u.%u%u", lip[0], lip[1], lip[2], lip[3],
                    local_port);
    const uint8_t *pip = reinterpret_cast<const uint8_t *>(&peer_ip);
    off += snprintf(buf + off, sizeof(buf) - static_cast<size_t>(off),
                    "%u.%u.%u.%u%u", pip[0], pip[1], pip[2], pip[3],
                    peer_port);

    // 数字→字母变换：把 0-9 映射为 a-j，避免数字段与 IP 段歧义。
    for (int i = 0; i < off; i++) {
        if (buf[i] >= '0' && buf[i] <= '9') {
            buf[i] = static_cast<char>('a' + (buf[i] - '0'));
        }
    }

    uint8_t md5[16];
    mbedtls_md5(reinterpret_cast<const uint8_t *>(buf), static_cast<size_t>(off),
                md5);
    char auth_key[33];
    hex_to_lower(md5, 16, auth_key);

    // AES key 与 IV 都取 authKey 前 16 字节的 ASCII。
    // IV 初值与密钥相同是协议观察到的行为（type 1，而非 type 2），
    // 换成别的来源会导致首帧解密失败。
    memcpy(session->aes_key, auth_key, 16);
    memcpy(session->encrypt_iv, auth_key, 16);
    memcpy(session->decrypt_iv, auth_key, 16);
    memcpy(session->auth_key, auth_key, 32);
    session->auth_key[32] = '\0';
    session->has_session_key = true;

    ESP_LOGI(kTag, "Session key derived: authKey=%s", auth_key);
}

// 设备挑战应答材料：16 字节随机数的 32 hex 表示。
void generate_auth_msg(miplay_session_t *session)
{
    uint8_t rand_bytes[16];
    for (int i = 0; i < 16; i += 4) {
        const uint32_t r = esp_random();
        memcpy(rand_bytes + i, &r, 4);
    }
    hex_to_lower(rand_bytes, 16, reinterpret_cast<char *>(session->auth_msg));
}

// 16~17 位十进制挑战串。手机把它与 authKey 一起做 HMAC，
// 位数不足时补前导 1 凑成 17 位——手机不校验位数，但不能是空串。
void generate_challenge(char *out, size_t out_max)
{
    const uint64_t r = (static_cast<uint64_t>(esp_random()) << 32) | esp_random();
    const uint64_t val = r % 10000000000000000ULL;  // 10^16
    if (val < 1000000000000000ULL) {
        snprintf(out, out_max, "1%015llu", static_cast<unsigned long long>(val));
    } else {
        snprintf(out, out_max, "%016llu", static_cast<unsigned long long>(val));
    }
}

uint32_t my_ipv4(void)
{
    esp_netif_ip_info_t ip;
    esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (n && esp_netif_get_ip_info(n, &ip) == ESP_OK) {
        return ip.ip.addr;
    }
    return 0x7F000001UL;
}

// ── 设备信息响应（OPack 编码的 key/value 对）──

// body 结构：重复 [keyLen(1), key, 0x0C, valLen(2 BE), value]，
// 前置 3 字节魔术 {0x00, 0x01, 0x55}。
int build_device_info_payload(uint8_t *out, size_t out_max)
{
    struct Field {
        const char *key;
        const char *val;
    };
    const Field fields[] = {
        {"alonePlayCapacity", "0"},
        // canAlonePlayCtrl=1：申报"本机可被遥控控制"。取 0（PC 原型默认值）
        // 会让遥控面板不下发任何按键——现象是界面能点、设备无反应。
        // canRevCtrl 是反方向（设备→手机），两条互不影响，都要置 1。
        {"canAlonePlayCtrl", "1"},
        {"canHeadsetCtrl", "0"},
        {"canRevCtrl", "1"},
        {"channel", ""},
        {"deviceId", s_device_id},  // MAC 后 4 字节的 8 hex
        {"deviceType", MIPLAY_DEV}, // 与 _mi-connect 的 dev=2 对齐
        {"model", "Android TV"},
        {"needAblum", "1"},
        {"needLrc", "1"},
        {"needPos", "1"},
        {"romVersion", ""},
        {"support", "audio"},
    };
    const int num_fields = static_cast<int>(sizeof(fields) / sizeof(fields[0]));

    constexpr size_t kBodyCap = 512;
    uint8_t *body = static_cast<uint8_t *>(heap_caps_malloc(kBodyCap, MALLOC_CAP_SPIRAM));
    if (!body) {
        return -1;
    }
    int body_off = 0;
    for (int i = 0; i < num_fields; i++) {
        const size_t klen = strlen(fields[i].key);
        const size_t vlen = strlen(fields[i].val);
        if (body_off + 1 + static_cast<int>(klen) + 1 + 2 + static_cast<int>(vlen) >
            static_cast<int>(kBodyCap)) {
            free(body);
            return -1;
        }
        body[body_off++] = static_cast<uint8_t>(klen);
        memcpy(body + body_off, fields[i].key, klen);
        body_off += static_cast<int>(klen);
        body[body_off++] = 0x0C;
        body[body_off++] = static_cast<uint8_t>((vlen >> 8) & 0xFF);
        body[body_off++] = static_cast<uint8_t>(vlen & 0xFF);
        memcpy(body + body_off, fields[i].val, vlen);
        body_off += static_cast<int>(vlen);
    }

    const int total = 3 + body_off;
    if (total > static_cast<int>(out_max)) {
        free(body);
        return -1;
    }
    out[0] = 0x00;
    out[1] = 0x01;
    out[2] = 0x55;
    memcpy(out + 3, body, static_cast<size_t>(body_off));
    free(body);
    return total;
}

// ── 媒体信息体（仅够通过握手，不含真实媒体状态）──

// NOTIFY 6 需要一段 mediaInfoEx。这里产出的是一份"空媒体信息"骨架，
// 字段占位但无内容——手机据此确认接收端支持 mediaInfoEx 通道。
// 真正的媒体元数据由 SET_MEDIA_INFO(0x12) 从手机侧推来，本层不解析。

// 写一个 OPack 字符串字段：label + 长度 + 内容。
bool opack_write_string_field(uint8_t *data, size_t capacity, size_t *offset,
                              const char *label, const char *value)
{
    const size_t llen = strlen(label);
    const size_t vlen = strlen(value);
    if (*offset + 1 + llen + 2 + vlen > capacity) {
        return false;
    }
    data[(*offset)++] = static_cast<uint8_t>(llen);
    memcpy(data + *offset, label, llen);
    *offset += llen;
    data[(*offset)++] = static_cast<uint8_t>(0x04);  // string 类型标记
    data[(*offset)++] = static_cast<uint8_t>((vlen >> 8) & 0xFF);
    data[(*offset)++] = static_cast<uint8_t>(vlen & 0xFF);
    memcpy(data + *offset, value, vlen);
    *offset += vlen;
    return true;
}

// 构建空 mediaInfoEx。返回写入长度，失败返回 -1。
int build_empty_media_info_ex(uint8_t *data, size_t capacity)
{
    size_t off = 0;
    // 0x0B = 字段区长度（由下面逐字段累积修正），先占位。
    struct Field {
        const char *key;
        const char *val;
    };
    const Field fields[] = {
        {"id", ""},        {"audioId", ""},  {"title", ""},
        {"artist", ""},    {"album", ""},    {"coverUrl", ""},
        {"duration", "0"}, {"position", "0"}, {"state", "0"},
    };
    for (const Field &f : fields) {
        if (!opack_write_string_field(data, capacity, &off, f.key, f.val)) {
            return -1;
        }
    }
    return static_cast<int>(off);
}

// ── OPack 字段解析（SET_MEDIA_INFO 0x12 用）──
//
// 手机推来的 SET_MEDIA_INFO payload 是 OPack 编码的键值对集合。
// 格式与 build_empty_media_info_ex 写出的对称：
//   keyLen(1) + key(N) + valueType(1) + valueLen(2 BE) + value(N)
// valueType: 0x04=string, 0x07=u32, 0x09=u64

struct OPackField {
    const uint8_t *value;
    size_t value_len;
    uint8_t type;
};

bool opack_find_field(const uint8_t *data, size_t len,
                      const char *key, OPackField *out)
{
    const size_t klen = strlen(key);
    size_t off = 0;
    while (off + 1 < len) {
        uint8_t field_key_len = data[off++];
        if (off + field_key_len > len) break;
        if (off + field_key_len + 3 > len) break;
        const uint8_t *field_key = data + off;
        off += field_key_len;
        uint8_t type = data[off++];
        uint16_t vlen = (static_cast<uint16_t>(data[off]) << 8) | data[off + 1];
        off += 2;
        if (off + vlen > len) break;
        if (field_key_len == klen && memcmp(field_key, key, klen) == 0) {
            out->value = data + off;
            out->value_len = vlen;
            out->type = type;
            return true;
        }
        off += vlen;
    }
    return false;
}

void opack_field_to_str(const OPackField &f, char *buf, size_t buf_size)
{
    if (f.type == 0x04) {
        size_t cplen = f.value_len < buf_size - 1 ? f.value_len : buf_size - 1;
        memcpy(buf, f.value, cplen);
        buf[cplen] = '\0';
    } else if (f.type == 0x07 && f.value_len >= 4) {
        uint32_t v = (static_cast<uint32_t>(f.value[0]) << 24) |
                     (static_cast<uint32_t>(f.value[1]) << 16) |
                     (static_cast<uint32_t>(f.value[2]) << 8) |
                     f.value[3];
        snprintf(buf, buf_size, "%lu", (unsigned long)v);
    } else if (f.type == 0x09 && f.value_len >= 8) {
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) v = (v << 8) | f.value[i];
        snprintf(buf, buf_size, "%llu", (unsigned long long)v);
    } else {
        buf[0] = '\0';
    }
}

}  // namespace

// ── 握手命令处理 ──
//
// 握手序列（与手机逐步对时）：
//   [设备→手机] 0x28 DEVICE_ID      14 位数字 ID, seq=4
//   [手机→设备] 0x36 NATIVE_VERSION 手机版本
//   [设备→手机] 0x37 VERSION_ACK    "2.1.5071614\0"
//   [手机→设备] 0x29 SAFETY_ACK     32 hex
//   [设备→手机] 0x22 NOTIFY(5,6,7)  能力声明，必须在 SafetyInfo 之前
//   [手机→设备] 0x1400 SafetyInfo   进入安全通道
//   [设备→手机] 0x1401 + 0x1402     SafetyInfoAck + SafetyAuth challenge
//   [手机→设备] 0x1402 SafetyAuth   手机回挑战应答
//   [设备→手机] 0x1403 SafetyAuthAck  安全通道建立

namespace {

// 发送需要加密的帧。会话未建立密钥时降级为明文——0x29 之前的帧
// 本来就不能加密（双方还没协商出 key）。
int send_maybe_encrypted(miplay_session_t *session, uint16_t cmd, uint16_t seq,
                         const uint8_t *payload, uint32_t payload_len)
{
    if (session->has_session_key) {
        return miplay_send_encrypted(session, cmd, seq, payload, payload_len);
    }
    return miplay_send_frame(session->sock, cmd, seq, payload, payload_len);
}

// 空 mediaInfoEx NOTIFY（29 字节，对齐参考 esp_miplay-main）。
// HyperOS 在消费一条加密的空 mediaInfoEx NOTIFY 时安装 reverse-control /
// skip-metadata callback。此后手机推送的 SetMediaInfo 才会携带 mCoverUrl/mTitle。
// 缺此 NOTIFY → CMD_SET_MEDIA_INFO(0x0012) 永远不会到达。
static const uint8_t s_empty_media_info_ex[29] = {
    0x0B, 'm', 'e', 'd', 'i', 'a', 'I', 'n', 'f', 'o', 'E', 'x',
    0x16, 0x00, 0x00, 0x00, 0x0C, 0x06,
    'm', 'T', 'i', 't', 'l', 'e',
    0x14, 0x00, 0x00, 0x00, 0x00,
};

// 发送空 mediaInfoEx NOTIFY 并记日志。SafetyAuth 成功后 + OPEN_DEVICE 后各调一次。
void send_empty_media_info_ex(miplay_session_t *session, const char *reason)
{
    send_maybe_encrypted(session, CMD_NOTIFY,
                         miplay_next_notify_seq(session),
                         s_empty_media_info_ex,
                         sizeof(s_empty_media_info_ex));
    ESP_LOGI(kTag, "-> NOTIFY empty mediaInfoEx (%s)", reason);
}

// 处理 0x36 版本交换：回 VERSION_ACK。
// 注意 payload 带尾部 NUL（snprintf 的 +1），手机按字符串比较。
// **必须明文发送**：握手前段（0x28/0x36/0x37/0x29/NOTIFY）协议规定不加密。
// 虽然此时 has_session_key 已为 true（密钥在连接建立时派生），若走加密路径
// 会把 encrypt_iv 消耗一个 CBC 链值——手机侧还停在初值，随后 0x1402 challenge
// 的密文它解不开，读不到 authMsg，回 authMsgAck:"" + result:"1" 拒绝。
// 症状上表现为"握手到 0x1403 必败"，且每次重连都一样。
void handle_native_version(miplay_session_t *session, const uint8_t *payload,
                           uint32_t plen, uint16_t seq)
{
    if (plen > 0) {
        ESP_LOGI(kTag, "Phone version: %.*s", static_cast<int>(plen - 1), payload);
    }
    uint8_t ver_payload[24];
    const int vlen =
        snprintf(reinterpret_cast<char *>(ver_payload), sizeof(ver_payload), "%s",
                 kReceiverVersion) + 1;
    miplay_send_frame(session->sock, CMD_NATIVE_VERSION_ACK, seq, ver_payload,
                      static_cast<uint32_t>(vlen));
    ESP_LOGI(kTag, "-> VersionAck: %s", kReceiverVersion);
}

// 处理 0x29 AUTH_ACK：登录手机的 auth 值并声明三项能力。
//
// 手机的 32 hex 只用于日志与握手确认，**不可**写回 session->auth_msg：
// 0x1402 下发的 authMsg 必须是本端独立生成的随机值（见 generate_auth_msg）。
// 若被手机的值覆盖，手机侧会识别为"挑战与自己发出的一致"并回
// result:"1" 拒绝整条安全通道——表现为握手走到 0x1403 后失败。
//
// NOTIFY(5,6,7) 必须在 SafetyInfo 之前发出——顺序反了手机不会进入安全通道，
// 表现为"版本交换成功但设备始终未就绪"。
void handle_safety_ack(miplay_session_t *session, const uint8_t *payload,
                       uint32_t plen)
{
    if (plen > 0) {
        char auth_hex[33] = {0};
        const int copy_len = plen > 32 ? 32 : static_cast<int>(plen);
        memcpy(auth_hex, payload, static_cast<size_t>(copy_len));
        ESP_LOGI(kTag, "AUTH_ACK: %s", auth_hex);
    }

    // NOTIFY 5: mode
    {
        const uint8_t mode_body[] = {4, 'm', 'o', 'd', 'e', 3, 2};
        miplay_send_frame(session->sock, CMD_NOTIFY, 5, mode_body,
                          sizeof(mode_body));
    }
    // NOTIFY 6: 空 mediaInfoEx。手机按此确认 mediaInfoEx 通道可用。
    {
        uint8_t *mi_body =
            static_cast<uint8_t *>(heap_caps_malloc(kMediaInfoBufferSize, MALLOC_CAP_SPIRAM));
        if (mi_body) {
            const int mi_len = build_empty_media_info_ex(mi_body, kMediaInfoBufferSize);
            if (mi_len > 0) {
                miplay_send_frame(session->sock, CMD_NOTIFY, 6, mi_body,
                                  static_cast<uint32_t>(mi_len));
            } else {
                ESP_LOGW(kTag, "mediaInfoEx build failed");
            }
            free(mi_body);
        }
    }
    // NOTIFY 7: state
    {
        const uint8_t state_body[] = {5, 's', 't', 'a', 't', 'e', 3, 0};
        miplay_send_frame(session->sock, CMD_NOTIFY, 7, state_body,
                          sizeof(state_body));
    }
    ESP_LOGI(kTag, "-> NOTIFY capabilities (5,6,7)");
}

// 处理 0x1E GET_DEVICE_INFO：回 OPack 编码的设备信息。
// canAlonePlayCtrl=1 在此下发给手机，是"遥控面板可下发按键"的前提。
void handle_get_device_info(miplay_session_t *session, uint16_t seq)
{
    uint8_t body[512];
    const int len = build_device_info_payload(body, sizeof(body));
    if (len <= 0) {
        ESP_LOGW(kTag, "device_info build failed");
        return;
    }
    send_maybe_encrypted(session, CMD_GET_DEVICE_INFO_ACK, seq, body,
                         static_cast<uint32_t>(len));
    ESP_LOGI(kTag, "-> DeviceInfoAck (%d bytes)", len);
}

// 处理 0x1A 心跳：回 HEARTBEAT_ACK。
// 手机看门狗约 10s 超时，收不到 ACK 会主动断开。
void handle_heartbeat(miplay_session_t *session, uint16_t seq)
{
    send_maybe_encrypted(session, CMD_HEARTBEAT_ACK, seq, nullptr, 0);
}

// 帧头合理性校验。outer 只允许三种：普通(0x00) / Alone(0x04) / Safety(0x14)。
bool frame_hdr_plausible(uint8_t outer_type, uint32_t plen)
{
    if (plen > MIPLAY_MAX_FRAME_PAYLOAD) {
        return false;
    }
    return outer_type == 0x00 || outer_type == 0x04 || outer_type == 0x14;
}

// 断开清理：关 socket、清零密钥、归还 slot。
// 顺序有讲究——先清密钥再关 socket，否则手机可能在我们仍持旧密钥时重连，
// 落到残留旧密钥的 slot 上。
void disconnect_cleanup(miplay_session_t *session)
{
    if (!session) {
        return;
    }
    const int client_sock = session->sock;
    set_connected(false);
    // 先 shutdown 阻止新的 recv/send，让 RTSP 任务感知到断连
    shutdown(client_sock, SHUT_RDWR);
    // 递增 media generation，等 RTSP 任务真正退出后再关 socket
    miplay_media_stop();
    // 等 tcpip_thread 排空该 socket 的待处理回调，减少 close 后 use-after-free
    vTaskDelay(pdMS_TO_TICKS(100));
    // SO_LINGER(l_linger=0)：close() 时直接发 RST，
    // 减少 lwIP tcpip_thread 回调在 socket 释放后仍触发的窗口。
    struct linger lg = {1, 0};
    setsockopt(client_sock, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    miplay_session_lock(1000);
    close(client_sock);
    // 会话级敏感材料必须清零：跨会话残留会让新连接用旧密钥解密，
    // 表现为"握手看着成功、后续帧全烂"。
    memset(session->aes_key, 0, sizeof(session->aes_key));
    memset(session->encrypt_iv, 0, sizeof(session->encrypt_iv));
    memset(session->decrypt_iv, 0, sizeof(session->decrypt_iv));
    memset(session->auth_key, 0, sizeof(session->auth_key));
    memset(session->auth_msg, 0, sizeof(session->auth_msg));
    session->has_session_key = false;
    session->reverse_control_ready = false;
    session->play_source_registered = false;
    session->media_session_opened = false;
    session->notify_seq = 8;
    miplay_session_unlock();
    miplay_session_release(session);
    ESP_LOGI(kTag, "session closed (sock=%d)", client_sock);
}

}  // namespace

// ── 会话主循环 ──

namespace {

void client_loop(miplay_session_t *session)
{
    const int client_sock = session->sock;
    struct sockaddr_in client_addr = {};
    client_addr.sin_family = AF_INET;
    client_addr.sin_port = htons(session->peer_port);
    client_addr.sin_addr.s_addr = session->peer_ip;

    ESP_LOGI(kTag, "=== Client %s:%u ===", inet_ntoa(client_addr.sin_addr),
             session->peer_port);

    // 取本端四元组供密钥派生。顺序必须是 (本端, 对端)——手机视角 remote 在前，
    // 顺序写反会导致双方算出的 authKey 不同。
    struct sockaddr_in local_addr = {};
    socklen_t addr_len = sizeof(local_addr);
    getsockname(client_sock, reinterpret_cast<struct sockaddr *>(&local_addr),
                &addr_len);
    const uint32_t peer_ip = client_addr.sin_addr.s_addr;
    const uint16_t peer_port = session->peer_port;
    const uint32_t local_ip = local_addr.sin_addr.s_addr;
    const uint16_t local_port = ntohs(local_addr.sin_port);

    // 接收缓冲 256KB 级，必须 PSRAM。这是协议规定的单帧上限，
    // 缩小它会在收到大帧时截断并失步。
    uint8_t *buf = static_cast<uint8_t *>(
        heap_caps_malloc(MIPLAY_RX_BUF_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf) {
        ESP_LOGE(kTag, "rx buffer alloc failed (%u bytes)",
                 static_cast<unsigned>(MIPLAY_RX_BUF_LEN));
        disconnect_cleanup(session);
        return;
    }
    int buf_used = 0;

    char challenge[20];
    generate_challenge(challenge, sizeof(challenge));

    // 安全通道握手期间用的一次性状态。0x1403 要回的是「手机 0x1402 的
    // authMsg 的 HMAC」，但本机的 ack 只有等手机先校验通过后才补发，
    // 所以算出来的 HMAC 要先暂存。
    bool auth_challenge_sent = false;
    bool pending_ack_valid = false;
    char pending_ack_hex[65] = {0};
    uint16_t pending_ack_seq = 0;

    // 握手第一步：先算密钥、再下发 14 位设备 ID。
    miplay_session_lock(2000);
    derive_session_key(session, peer_ip, peer_port, local_ip, local_port);
    generate_auth_msg(session);
    {
        uint8_t md5[16];
        char id_buf[32];
        const int id_len =
            snprintf(id_buf, sizeof(id_buf), "%02X%02X%02X%02X%02X%02X",
                     s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);
        mbedtls_md5(reinterpret_cast<uint8_t *>(id_buf),
                    static_cast<size_t>(id_len), md5);
        uint64_t num_id = (static_cast<uint64_t>(md5[0]) << 56) |
                          (static_cast<uint64_t>(md5[1]) << 48) |
                          (static_cast<uint64_t>(md5[2]) << 40) |
                          (static_cast<uint64_t>(md5[3]) << 32) |
                          (static_cast<uint64_t>(md5[4]) << 24) |
                          (static_cast<uint64_t>(md5[5]) << 16) |
                          (static_cast<uint64_t>(md5[6]) << 8) |
                          static_cast<uint64_t>(md5[7]);
        num_id %= 100000000000000ULL;  // 10^14
        char did_str[16];
        snprintf(did_str, sizeof(did_str), "%014llu",
                 static_cast<unsigned long long>(num_id));
        miplay_send_frame(client_sock, CMD_SAFETY_CHALLENGE, 4,
                          reinterpret_cast<const uint8_t *>(did_str),
                          strlen(did_str));
        ESP_LOGI(kTag, "-> DEVICE_ID(0x28): %s seq=4", did_str);
    }
    miplay_session_unlock();

    ESP_LOGI(kTag, "Endpoints: local=%lu:%u peer=%lu:%u challenge=%s",
             static_cast<unsigned long>(local_ip), local_port,
             static_cast<unsigned long>(peer_ip), peer_port, challenge);

    while (s_control_running) {
        struct pollfd pfd = {};
        pfd.fd = client_sock;
        pfd.events = POLLIN;
        const int pr = poll(&pfd, 1, 500);  // 500ms timeout
        if (pr < 0) {
            ESP_LOGW(kTag, "poll error: %d", errno);
            break;
        }
        if (pr == 0 || !(pfd.revents & POLLIN)) {
            continue;
        }

        const int space = static_cast<int>(MIPLAY_RX_BUF_LEN) - buf_used;
        if (space <= 0) {
            // 缓冲塞满却无完整帧 = 帧头已损坏。丢弃重来优于继续堆积，
            // 否则会永久解析不出帧。
            ESP_LOGW(kTag, "rx buffer full without complete frame, resync");
            buf_used = 0;
            continue;
        }
        const int n =
            recv(client_sock, buf + buf_used, static_cast<size_t>(space), 0);
        if (n <= 0) {
            if (n == 0) {
                ESP_LOGI(kTag, "client disconnected");
            } else {
                ESP_LOGW(kTag, "recv error: %d", errno);
            }
            break;
        }
        buf_used += n;

        while (buf_used >= MIPLAY_FRAME_HDR_LEN) {
            if (buf[0] != MIPLAY_FRAME_MAGIC) {
                // 跳到下一个 magic，而非逐字节后移——后者在坏数据里会
                // 退化为 O(n^2) 并刷屏。
                int skip = 1;
                while (skip < buf_used && buf[skip] != MIPLAY_FRAME_MAGIC) {
                    skip++;
                }
                static uint32_t s_bad_magic_logs = 0;
                if ((s_bad_magic_logs++ % 64U) == 0U) {
                    ESP_LOGW(kTag, "bad magic, skip %d (count=%lu)", skip,
                             static_cast<unsigned long>(s_bad_magic_logs));
                }
                memmove(buf, buf + skip, static_cast<size_t>(buf_used - skip));
                buf_used -= skip;
                continue;
            }

            const uint8_t outer_type = buf[1];
            const uint16_t cmd = (static_cast<uint16_t>(buf[1]) << 8) | buf[2];
            const uint16_t seq = (static_cast<uint16_t>(buf[3]) << 8) | buf[4];
            const uint32_t wire_plen = (static_cast<uint32_t>(buf[5]) << 24) |
                                       (static_cast<uint32_t>(buf[6]) << 16) |
                                       (static_cast<uint32_t>(buf[7]) << 8) |
                                       static_cast<uint32_t>(buf[8]);
            const uint32_t total = MIPLAY_FRAME_HDR_LEN + wire_plen;

            if (!frame_hdr_plausible(outer_type, wire_plen)) {
                // 只跳 1 字节：在超大 payload 里把每个 0x24 当帧头会彻底失步。
                static uint32_t s_bad_hdr_logs = 0;
                if ((s_bad_hdr_logs++ % 64U) == 0U) {
                    ESP_LOGW(kTag, "implausible hdr outer=0x%02X plen=%lu",
                             outer_type, static_cast<unsigned long>(wire_plen));
                }
                memmove(buf, buf + 1, static_cast<size_t>(buf_used - 1));
                buf_used -= 1;
                continue;
            }
            if (buf_used < static_cast<int>(total)) {
                break;  // 数据不全，等下一轮 recv
            }

            const uint8_t *payload = buf + MIPLAY_FRAME_HDR_LEN;
            uint32_t plen = wire_plen;
            ESP_LOGI(kTag, "RX cmd=0x%04X seq=%u len=%lu outer=0x%02X", cmd, seq,
                     static_cast<unsigned long>(plen), outer_type);

            // 统一解密：凡是符合 SafetyData 头的帧都尝试解密，**即使本命令
            // 无需处理**。CBC 的 IV 链式演进，跳过一帧不解密会让 IV 与手机
            // 不同步，之后所有帧全部解不开——症状是"握手成功后突然全失效"。
            uint8_t *dec_buf = nullptr;
            if (session->has_session_key && plen >= 9 && payload[0] == 0x00 &&
                payload[1] == 0x07 && payload[2] == 0x01 && payload[3] == 0xE0) {
                dec_buf = static_cast<uint8_t *>(
                    heap_caps_malloc(plen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                if (dec_buf) {
                    const int dec_len = miplay_safety_decrypt(
                        payload, plen, session->aes_key, session->decrypt_iv,
                        dec_buf, plen);
                    if (dec_len >= 0) {
                        payload = dec_buf;
                        plen = static_cast<uint32_t>(dec_len);
                    } else {
                        ESP_LOGW(kTag, "decrypt failed for cmd=0x%04X", cmd);
                        free(dec_buf);
                        dec_buf = nullptr;
                    }
                }
            }

            if (outer_type == 0x04) {
                // AloneMediaPlayer 命名空间：本层无媒体管线，只记日志。
                // 帧已解密（IV 已推进），不影响后续同步。
                ESP_LOGI(kTag, "alone frame sub=0x%02X len=%u (no media layer)",
                         cmd & 0xFF, static_cast<unsigned>(plen));
            } else if (outer_type == 0x14) {
                // Safety 阶段：0x1400/0x1401/0x1402/0x1403 双向认证。
                if (cmd == CMD_SAFETY_INFO) {
                    miplay_safety_handle_info(session, payload, plen, seq,
                                              &auth_challenge_sent);
                } else if (cmd == CMD_SAFETY_INFO_ACK) {
                    ESP_LOGI(kTag, "safety: phone confirmed parameter selection");
                } else if (cmd == CMD_SAFETY_AUTH) {
                    // 用统一解密的结果（dec_buf）而非再解一次——重复解密会
                    // 让 CBC 的 IV 多推进一次，之后所有帧全乱。
                    if (dec_buf) {
                        pending_ack_valid = miplay_safety_handle_auth(
                            session, dec_buf, plen, seq, pending_ack_hex,
                            &pending_ack_seq);
                    } else {
                        ESP_LOGW(kTag, "safety: 0x1402 without plaintext");
                    }
                } else if (cmd == CMD_SAFETY_AUTH_ACK) {
                    if (dec_buf) {
                        if (miplay_safety_handle_auth_ack(
                                session, dec_buf, plen, pending_ack_hex,
                                pending_ack_valid, pending_ack_seq)) {
                            ESP_LOGI(kTag, "control channel authenticated");
                            session->reverse_control_ready = true;
                            // 关键：发空 mediaInfoEx NOTIFY，让 HyperOS 安装
                            // metadata push callback。不发此帧 → 0x0012 永远不到。
                            send_empty_media_info_ex(session, "SafetyAuth");
                        } else {
                            // 认证失败必须断开：继续跑下去所有加密帧都会失败，
                            // 问题会在更晚、更难定位的地方暴露。
                            free(dec_buf);
                            dec_buf = nullptr;
                            free(buf);
                            disconnect_cleanup(session);
                            return;
                        }
                    } else {
                        ESP_LOGW(kTag, "safety: 0x1403 without plaintext");
                    }
                } else {
                    ESP_LOGI(kTag, "safety frame cmd=0x%04X len=%u (unhandled)",
                             cmd, static_cast<unsigned>(plen));
                }
            } else {
                switch (cmd) {
                case CMD_NATIVE_VERSION:
                    handle_native_version(session, payload, plen, seq);
                    break;
                case CMD_SAFETY_ACK:
                    handle_safety_ack(session, payload, plen);
                    break;
                case CMD_HEARTBEAT:
                    handle_heartbeat(session, seq);
                    break;
                case CMD_GET_DEVICE_INFO:
                    handle_get_device_info(session, seq);
                    break;
                case CMD_SET_VOLUME: {
                    // payload: 4 字节 uint32 BE，音量百分比 0-100。
                    uint32_t vol = miplay_media_get_volume();
                    if (plen >= 4) {
                        vol = ((uint32_t)payload[0] << 24) |
                              ((uint32_t)payload[1] << 16) |
                              ((uint32_t)payload[2] << 8) | payload[3];
                        if (vol > 100) vol = 100;
                        miplay_media_set_volume(vol);
                        ESP_LOGI(kTag, "SET_VOLUME -> %lu", (unsigned long)vol);
                    }
                    // ACK: 5 字节 {0x00, vol_u32_be}
                    uint8_t ack[5] = {};
                    ack[1] = (uint8_t)((vol >> 24) & 0xFF);
                    ack[2] = (uint8_t)((vol >> 16) & 0xFF);
                    ack[3] = (uint8_t)((vol >> 8) & 0xFF);
                    ack[4] = (uint8_t)(vol & 0xFF);
                    send_maybe_encrypted(session, CMD_SET_VOLUME + 1, seq,
                                         ack, sizeof(ack));
                    // NOTIFY: 通知手机音量已变更。
                    uint8_t notify_body[12] = {};
                    notify_body[0] = 0x06;                           // key_len
                    memcpy(&notify_body[1], "volume", 6);            // key
                    notify_body[7] = 0x07;                           // value_type = u32
                    notify_body[8] = (uint8_t)((vol >> 24) & 0xFF);  // value BE
                    notify_body[11] = (uint8_t)(vol & 0xFF);
                    send_maybe_encrypted(session, CMD_NOTIFY,
                                         miplay_next_notify_seq(session),
                                         notify_body, sizeof(notify_body));
                    break;
                }
                case CMD_GET_VOLUME: {
                    uint32_t vol = miplay_media_get_volume();
                    uint8_t body[5] = {};
                    body[1] = (uint8_t)((vol >> 24) & 0xFF);
                    body[2] = (uint8_t)((vol >> 16) & 0xFF);
                    body[3] = (uint8_t)((vol >> 8) & 0xFF);
                    body[4] = (uint8_t)(vol & 0xFF);
                    send_maybe_encrypted(session, CMD_GET_VOLUME + 1, seq,
                                         body, sizeof(body));
                    ESP_LOGD(kTag, "GET_VOLUME -> %lu", (unsigned long)vol);
                    break;
                }
                case CMD_OPEN_DEVICE: {
                    // 媒体层：解析 wfd:// URL，启动 RTSP 任务。
                    miplay_media_handle_open_device(session, payload, plen, seq);
                    set_connected(true);
                    // 参考 esp-miply-1.85touch: OPEN_DEVICE 后再发一次空
                    // mediaInfoEx NOTIFY，确保手机 metadata push 已激活。
                    send_empty_media_info_ex(session, "OPEN_DEVICE");
                    break;
                }
                case CMD_SET_MEDIA_INFO: {
                    // 只做 ACK，不解析元数据。本设备无屏幕，title/artist/album
                    // 纯浪费 CPU。音频流不依赖此消息。
                    ESP_LOGI(kTag, "SET_MEDIA_INFO seq=%u len=%u (ignored)",
                             seq, static_cast<unsigned>(plen));
                    send_maybe_encrypted(session, CMD_SET_MEDIA_INFO_ACK, seq,
                                         nullptr, 0);
                    break;
                }
                case CMD_GET_MEDIA_INFO: {
                    // 手机查询媒体信息。参考 esp-miply-1.85touch：始终回空
                    // mediaInfoEx NOTIFY，引导手机安装 metadata push callback。
                    ESP_LOGI(kTag, "GET_MEDIA_INFO seq=%u", seq);
                    send_empty_media_info_ex(session, "GetMediaInfo");
                    break;
                }
                case CMD_NOTIFY: {
                    // 入站 NOTIFY (0x0022)：mirror mode 2 下手机把媒体信息/播放
                    // 状态作为 0x0022 通知异步下发（而非 SET_MEDIA_INFO）。
                    // payload 可能是 JSON 或 OPack 二进制 TLV。
                    ESP_LOGI(kTag, "[NOTIFY-in] seq=%u len=%u",
                             seq, static_cast<unsigned>(plen));
                    if (plen > 0 && (payload[0] == '{' || payload[0] == '[')) {
                        // JSON 路径：直接提取元数据字段
                        char title[64] = {}, artist[64] = {}, album[64] = {};
                        int64_t duration_ms = 0, position_ms = 0;
                        const char *p = reinterpret_cast<const char *>(payload);
                        // JSON 字段查找 lambda（多别名 fallback）
                        auto find_json_str = [&](const char *keys[], char *out, size_t out_sz) {
                            for (int i = 0; keys[i]; i++) {
                                const char *f = strstr(p, keys[i]);
                                if (f) {
                                    f += strlen(keys[i]);
                                    const char *e = strchr(f, '"');
                                    if (e && (size_t)(e - f) < out_sz) {
                                        memcpy(out, f, e - f);
                                        out[e - f] = '\0';
                                        return true;
                                    }
                                }
                            }
                            return false;
                        };
                        auto find_json_num = [&](const char *keys[]) -> int64_t {
                            for (int i = 0; keys[i]; i++) {
                                const char *f = strstr(p, keys[i]);
                                if (f) return strtoll(f + strlen(keys[i]), nullptr, 10);
                            }
                            return 0;
                        };
                        { const char *ks[] = {"\"mTitle\":\"", "\"title\":\"", "\"songName\":\"", nullptr};
                          find_json_str(ks, title, sizeof(title)); }
                        { const char *ks[] = {"\"mArtist\":\"", "\"artist\":\"", "\"singer\":\"", nullptr};
                          find_json_str(ks, artist, sizeof(artist)); }
                        { const char *ks[] = {"\"mAlbum\":\"", "\"album\":\"", nullptr};
                          find_json_str(ks, album, sizeof(album)); }
                        { const char *ks[] = {"\"mDuration\":", "\"duration\":", "\"durationMs\":", nullptr};
                          duration_ms = find_json_num(ks); }
                        { const char *ks[] = {"\"mPosition\":", "\"position\":", "\"positionMs\":", nullptr};
                          position_ms = find_json_num(ks); }
                        if (title[0]) {
                            ESP_LOGI(kTag, "NOTIFY-in meta: title='%s' artist='%s' album='%s' dur=%lld pos=%lld",
                                     title, artist, album, (long long)duration_ms, (long long)position_ms);
                            miplay_media_dispatch_meta(title, artist, album, duration_ms, position_ms);
                        }
                    } else if (plen > 0) {
                        // OPack 二进制路径：尝试用已有 OPack 解析器
                        char title[64] = {}, artist[64] = {}, album[64] = {};
                        int64_t duration_ms = 0, position_ms = 0;
                        OPackField f;
                        if (opack_find_field(payload, plen, "mTitle", &f) ||
                            opack_find_field(payload, plen, "title", &f)) {
                            opack_field_to_str(f, title, sizeof(title));
                        }
                        if (opack_find_field(payload, plen, "mArtist", &f) ||
                            opack_find_field(payload, plen, "artist", &f)) {
                            opack_field_to_str(f, artist, sizeof(artist));
                        }
                        if (opack_find_field(payload, plen, "mAlbum", &f) ||
                            opack_find_field(payload, plen, "album", &f)) {
                            opack_field_to_str(f, album, sizeof(album));
                        }
                        if (opack_find_field(payload, plen, "mDuration", &f) ||
                            opack_find_field(payload, plen, "duration", &f)) {
                            char dur_buf[32];
                            opack_field_to_str(f, dur_buf, sizeof(dur_buf));
                            duration_ms = strtoll(dur_buf, nullptr, 10);
                        }
                        if (opack_find_field(payload, plen, "mPosition", &f) ||
                            opack_find_field(payload, plen, "position", &f)) {
                            char pos_buf[32];
                            opack_field_to_str(f, pos_buf, sizeof(pos_buf));
                            position_ms = strtoll(pos_buf, nullptr, 10);
                        }
                        if (title[0]) {
                            ESP_LOGI(kTag, "NOTIFY-in OPack meta: title='%s' artist='%s' dur=%lld",
                                     title, artist, (long long)duration_ms);
                            miplay_media_dispatch_meta(title, artist, album, duration_ms, position_ms);
                        } else {
                            ESP_LOGW(kTag, "[NOTIFY-in] OPack payload first=%02X %02X %02X %02X (no meta found)",
                                     payload[0], plen > 1 ? payload[1] : 0,
                                     plen > 2 ? payload[2] : 0, plen > 3 ? payload[3] : 0);
                        }
                    }
                    break;
                }
                default:
                    if (cmd == 0x006C) {
                        // SetMirrorKey：媒体流加密密钥。
                        ESP_LOGI(kTag, "SetMirrorKey seq=%u len=%u", seq,
                                 static_cast<unsigned>(plen));
                        miplay_media_set_stream_key(payload, plen);
                        uint8_t mk_body[] = {0x00};
                        send_maybe_encrypted(session, 0x006D, seq, mk_body,
                                             sizeof(mk_body));
                    } else if (cmd == CMD_SET_LOCAL_DEV_INFO) {
                        // SetLocalDeviceInfo (0x0058)：手机推送本机信息，必须 ACK。
                        ESP_LOGI(kTag, "SetLocalDevInfo seq=%u len=%u", seq,
                                 static_cast<unsigned>(plen));
                        send_maybe_encrypted(session, CMD_SET_LOCAL_DEV_ACK, seq,
                                             nullptr, 0);
                    } else if (cmd == CMD_GET_MIRROR_MODE) {
                        // GetMirrorMode (0x0034)：mode=1 = 移动音频流
                        //（HyperOS 发 SET_MEDIA_INFO + 装反向控制）。
                        ESP_LOGI(kTag, "GetMirrorMode seq=%u", seq);
                        uint8_t mode_resp[] = {0x00, 0x00, 0x00, 0x00, 0x01};
                        send_maybe_encrypted(session, CMD_GET_MIRROR_MODE_ACK, seq,
                                             mode_resp, sizeof(mode_resp));
                    } else if (cmd == CMD_SET_PLAY_SOURCE) {
                        // SetPlaySource (0x0040)：可能携带媒体信息。
                        ESP_LOGI(kTag, "SetPlaySource seq=%u len=%u", seq,
                                 static_cast<unsigned>(plen));
                        send_maybe_encrypted(session, CMD_SET_PLAY_SOURCE + 1, seq,
                                             nullptr, 0);
                    } else if (cmd == CMD_PAUSE || cmd == CMD_RESUME) {
                        // 暂停/恢复：ACK + 通知 UI 层状态变化。
                        ESP_LOGI(kTag, "media ctrl cmd=0x%04X seq=%u", cmd, seq);
                        send_maybe_encrypted(session, cmd + 1, seq, nullptr, 0);
                        miplay_media_dispatch_pause(cmd == CMD_PAUSE);
                    } else if (cmd == CMD_SET_POSITION) {
                        // SetPosition (0x0056)：前8字节大端毫秒位置。
                        int64_t pos_ms = 0;
                        if (plen >= 8) {
                            for (int i = 0; i < 8; i++)
                                pos_ms = (pos_ms << 8) | (int64_t)payload[i];
                        }
                        ESP_LOGI(kTag, "SetPosition seq=%u pos=%lldms", seq, (long long)pos_ms);
                        miplay_media_dispatch_meta(nullptr, nullptr, nullptr, 0, pos_ms);
                        send_maybe_encrypted(session, cmd + 1, seq, nullptr, 0);
                    } else if (cmd == CMD_SET_MEDIA_STATE) {
                        // SetMediaState (0x005E)：播放状态变化，ACK。
                        ESP_LOGI(kTag, "SetMediaState seq=%u", seq);
                        send_maybe_encrypted(session, cmd + 1, seq, nullptr, 0);
                    } else {
                        ESP_LOGD(kTag, "unhandled cmd=0x%04X len=%u", cmd,
                                 static_cast<unsigned>(plen));
                    }
                    break;
                }
            }

            if (dec_buf) {
                free(dec_buf);
            }
            memmove(buf, buf + total, static_cast<size_t>(buf_used - total));
            buf_used -= static_cast<int>(total);
        }
    }

    free(buf);
    disconnect_cleanup(session);
}

void client_task(void *arg)
{
    client_loop(static_cast<miplay_session_t *>(arg));
    vTaskDeleteWithCaps(nullptr);
}

}  // namespace

// ── 公共入口 ──

namespace {

// 监听任务：accept 后为每个连接起独立会话任务。
//
// 刻意不在这里处理连接——会话循环会长时间阻塞在 select 上，
// 放在 accept 循环里会让后续连接全部排队等不到服务。
void listen_task(void *arg)
{
    (void)arg;
    struct sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(MIPLAY_CONTROL_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    s_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s_listen_sock < 0) {
        ESP_LOGE(kTag, "socket failed: %d", errno);
        s_listen_task = nullptr;
        vTaskDeleteWithCaps(nullptr);
        return;
    }
    int opt = 1;
    setsockopt(s_listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (bind(s_listen_sock, reinterpret_cast<struct sockaddr *>(&server_addr),
             sizeof(server_addr)) < 0) {
        ESP_LOGE(kTag, "bind %d failed: %d", MIPLAY_CONTROL_PORT, errno);
        close(s_listen_sock);
        s_listen_sock = -1;
        s_listen_task = nullptr;
        vTaskDeleteWithCaps(nullptr);
        return;
    }
    if (listen(s_listen_sock, 8) < 0) {
        ESP_LOGE(kTag, "listen failed: %d", errno);
        close(s_listen_sock);
        s_listen_sock = -1;
        s_listen_task = nullptr;
        vTaskDeleteWithCaps(nullptr);
        return;
    }
    ESP_LOGI(kTag, "TCP %d listening for MiPlay control", MIPLAY_CONTROL_PORT);

    while (s_control_running) {
        struct sockaddr_in client_addr = {};
        socklen_t addr_len = sizeof(client_addr);
        const int client_sock =
            accept(s_listen_sock, reinterpret_cast<struct sockaddr *>(&client_addr),
                   &addr_len);
        if (client_sock < 0) {
            if (s_control_running) {
                ESP_LOGW(kTag, "accept failed: %d", errno);
                vTaskDelay(pdMS_TO_TICKS(250));
            }
            continue;
        }
        ESP_LOGI(kTag, "accepted %s:%u", inet_ntoa(client_addr.sin_addr),
                 static_cast<unsigned>(ntohs(client_addr.sin_port)));

        // TCP_NODELAY：握手是小包往返，Nagle 攒包会让手机侧等超时。
        int nodelay = 1;
        setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, &nodelay,
                   sizeof(nodelay));

        miplay_session_t *session =
            miplay_session_alloc(client_sock, client_addr.sin_addr.s_addr,
                                 ntohs(client_addr.sin_port));
        if (!session) {
            ESP_LOGW(kTag, "session slots exhausted, rejecting %s:%u",
                     inet_ntoa(client_addr.sin_addr),
                     static_cast<unsigned>(ntohs(client_addr.sin_port)));
            close(client_sock);
            continue;
        }

        // 会话任务栈放 PSRAM：256KB 级，内部 SRAM 分配必失败。
        BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
            client_task, "miplay_cli", MIPLAY_CLIENT_TASK_STACK_BYTES, session,
            5, &session->task, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ret != pdPASS) {
            ESP_LOGE(kTag, "client task create failed (stack=%u KB)",
                     static_cast<unsigned>(MIPLAY_CLIENT_TASK_STACK_BYTES / 1024));
            close(client_sock);
            miplay_session_release(session);
        }
    }

    close(s_listen_sock);
    s_listen_sock = -1;
    s_listen_task = nullptr;
    vTaskDeleteWithCaps(nullptr);
}

}  // namespace

esp_err_t miplay_control_start(void)
{
    if (s_control_running) {
        return ESP_OK;
    }
    miplay_session_init();
    s_control_running = true;
    set_connected(false);

    // 监听任务自身栈很小（只做 accept），放内部 SRAM 即可。
    const BaseType_t ret = xTaskCreatePinnedToCore(
        listen_task, "miplay_tcp", MIPLAY_TCP_TASK_STACK_BYTES, nullptr, 5,
        &s_listen_task, 1);
    if (ret != pdPASS) {
        ESP_LOGE(kTag, "listen task create failed");
        s_control_running = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

void miplay_control_stop(void)
{
    s_control_running = false;
    set_connected(false);
    if (s_listen_sock >= 0) {
        // 关掉监听 socket 让阻塞中的 accept 立即返回，任务自行退出。
        shutdown(s_listen_sock, SHUT_RDWR);
    }
}

bool miplay_control_is_connected(void)
{
    return s_connected;
}

void miplay_control_reset_connected(void)
{
    if (s_connected) {
        s_connected = false;
        ESP_LOGI(kTag, "connected state reset (stream ended)");
    }
}

void miplay_control_set_connection_callback(miplay_connection_changed_fn fn)
{
    s_conn_cb = fn;
}
