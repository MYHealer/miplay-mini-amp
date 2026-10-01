// MiPlay 安全通道实现。详见 miplay_safety.h 的握手顺序说明。
#include "miplay_safety.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"

#include "miplay_proto.h"

namespace {
constexpr char kTag[] = "miplay_safety";

// envelope payload 上限。envelope 的 dataLen 字段只占 1 字节，
// 超长会被截断成一个错的小长度，手机按错长度解析后丢弃整帧。
constexpr uint32_t kEnvelopePayloadMax = 2048;

// 手机提议后本机选定的安全参数。aesIvType=2 / aesKeyType=1 是实测值：
// IV 取 authKey 前 16 字节的 ASCII。改成别的组合会导致首帧解不开。
constexpr char kSelectionJson[] =
    "{\n\t\"aesIvType\": \"2\",\n\t\"aesKeyType\": \"1\",\n"
    "\t\"authAlgorithmType\": \"4\",\n\t\"authKeyType\": \"1\",\n"
    "\t\"integrityType\": \"1\",\n\t\"result\": \"0\" \n} \n";

// 从 JSON 文本里取出某个 key 的字符串值。搜索式解析而非完整 JSON 解析——
// 这段明文首块在高版本手机上偶有异常字节，完整解析会失败，但 authMsg
// 总在偏移 16 之后稳定出现，搜索式能容错。
bool json_extract_string(const uint8_t *text, size_t text_len,
                         const char *key_with_quotes, char *out,
                         size_t out_max)
{
    const char *hay = reinterpret_cast<const char *>(text);
    const size_t key_len = strlen(key_with_quotes);
    if (text_len < key_len) {
        return false;
    }
    const char *p = nullptr;
    for (size_t i = 0; i + key_len <= text_len; i++) {
        if (memcmp(hay + i, key_with_quotes, key_len) == 0) {
            p = hay + i + key_len;
            break;
        }
    }
    if (!p) {
        return false;
    }
    while (*p == ':' || *p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;
    const char *end = strchr(p, '"');
    if (!end || static_cast<size_t>(end - p) >= out_max) {
        return false;
    }
    memcpy(out, p, static_cast<size_t>(end - p));
    out[end - p] = '\0';
    return true;
}

}  // namespace

void miplay_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg,
                        size_t msg_len, uint8_t out[32])
{
    uint8_t k_pad[64];
    uint8_t k_hash[32];

    // 超长 key 先压缩到 32 字节，这是 HMAC 规范要求。
    if (key_len > 64) {
        mbedtls_sha256(key, key_len, k_hash, 0);
        key = k_hash;
        key_len = 32;
    }
    memset(k_pad, 0, sizeof(k_pad));
    if (key_len > 0) {
        memcpy(k_pad, key, key_len);
    }

    uint8_t o_key_pad[64];
    uint8_t i_key_pad[64];
    for (int i = 0; i < 64; i++) {
        o_key_pad[i] = static_cast<uint8_t>(k_pad[i] ^ 0x5C);
        i_key_pad[i] = static_cast<uint8_t>(k_pad[i] ^ 0x36);
    }

    mbedtls_sha256_context ctx;
    uint8_t inner[32];
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, i_key_pad, 64);
    mbedtls_sha256_update(&ctx, msg, msg_len);
    mbedtls_sha256_finish(&ctx, inner);
    mbedtls_sha256_free(&ctx);

    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, o_key_pad, 64);
    mbedtls_sha256_update(&ctx, inner, 32);
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

int miplay_send_plain_envelope(miplay_session_t *session, uint16_t cmd,
                               uint16_t seq, const uint8_t *payload,
                               uint32_t payload_len)
{
    if (!session || payload_len > kEnvelopePayloadMax) {
        return -1;
    }
    const size_t cap = 1 + 3 + 4 + 1 + payload_len;
    uint8_t *envelope = static_cast<uint8_t *>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM));
    if (!envelope) {
        return -1;
    }
    const int elen = miplay_safety_envelope_encode(
        true, MIPLAY_SAFETY_VALUE_TYPE, payload, payload_len, envelope, cap);
    int ret = -1;
    if (elen > 0) {
        ret = miplay_send_frame(session->sock, cmd, seq, envelope,
                                static_cast<uint32_t>(elen));
    }
    free(envelope);
    return ret;
}

int miplay_send_encrypted_envelope(miplay_session_t *session, uint16_t cmd,
                                   uint16_t seq, bool is_ack,
                                   const uint8_t *payload, uint32_t payload_len)
{
    if (!session || !session->has_session_key ||
        payload_len > kEnvelopePayloadMax) {
        return -1;
    }
    // 复用会话层提供的加密发送：它内部持有发送锁并串行化序号/IV。
    // 这里先编 envelope，再由 miplay_send_encrypted 加密外层。
    const size_t cap = 1 + 3 + 4 + 1 + payload_len;
    uint8_t *envelope = static_cast<uint8_t *>(heap_caps_malloc(cap, MALLOC_CAP_SPIRAM));
    if (!envelope) {
        return -1;
    }
    const int elen = miplay_safety_envelope_encode(
        is_ack, MIPLAY_SAFETY_VALUE_TYPE, payload, payload_len, envelope, cap);
    int ret = -1;
    if (elen > 0) {
        ret = miplay_send_encrypted(session, cmd, seq, envelope,
                                    static_cast<uint32_t>(elen));
    }
    free(envelope);
    return ret;
}

void miplay_safety_handle_info(miplay_session_t *session,
                               const uint8_t *payload, uint32_t plen,
                               uint16_t seq, bool *auth_challenge_sent)
{
    (void)payload;
    (void)plen;
    ESP_LOGI(kTag, "SafetyInfo offer received");

    // 先回参数选择（明文）。这一步必须在 challenge 之前——手机要先用
    // 选定参数初始化它那侧的 cipher，否则收到加密的 challenge 也解不开。
    if (miplay_send_plain_envelope(session, CMD_SAFETY_INFO_ACK, seq,
                                   reinterpret_cast<const uint8_t *>(kSelectionJson),
                                   sizeof(kSelectionJson) - 1) > 0) {
        ESP_LOGI(kTag, "-> SafetyInfoAck");
    } else {
        ESP_LOGW(kTag, "SafetyInfoAck send failed");
    }

    // 下发本机 challenge（加密）。authMsg 在连接建立时就已生成。
    if (auth_challenge_sent && !*auth_challenge_sent) {
        char challenge_json[128];
        const int cj_len = snprintf(
            challenge_json, sizeof(challenge_json),
            "{\n\t\"authMsg\": \"%s\" \n} \n",
            reinterpret_cast<const char *>(session->auth_msg));
        if (miplay_send_encrypted_envelope(
                session, CMD_SAFETY_AUTH, 0, false,
                reinterpret_cast<const uint8_t *>(challenge_json),
                static_cast<uint32_t>(cj_len)) > 0) {
            *auth_challenge_sent = true;
            ESP_LOGI(kTag, "-> SafetyAuth challenge sent authMsg=%s",
                     reinterpret_cast<const char *>(session->auth_msg));
        } else {
            ESP_LOGW(kTag, "SafetyAuth challenge send failed");
        }
    }
}

bool miplay_safety_handle_auth(miplay_session_t *session,
                               const uint8_t *plaintext, uint32_t pt_len,
                               uint16_t seq, char out_pending_hex[65],
                               uint16_t *out_pending_seq)
{
    (void)session;
    ESP_LOGI(kTag, "SafetyAuth challenge from phone (pt=%lu bytes)",
             static_cast<unsigned long>(pt_len));
    if (!plaintext || pt_len == 0) {
        ESP_LOGW(kTag, "SafetyAuth: empty plaintext");
        return false;
    }

    char peer_auth_msg[33] = {0};
    if (!json_extract_string(plaintext, pt_len, "\"authMsg\"", peer_auth_msg,
                             sizeof(peer_auth_msg))) {
        ESP_LOGW(kTag, "SafetyAuth: authMsg not found");
        return false;
    }
    ESP_LOGI(kTag, "phone authMsg: %s", peer_auth_msg);

    // 对手机的 authMsg 做 HMAC，稍后在 0x1403 里回给它。
    // 用完整 32 字节 authKey（不是前 16 字节）——手机侧同样用完整值。
    uint8_t ack_hash[32];
    miplay_hmac_sha256(reinterpret_cast<const uint8_t *>(session->auth_key), 32,
                       reinterpret_cast<const uint8_t *>(peer_auth_msg),
                       strlen(peer_auth_msg), ack_hash);
    static const char kHex[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out_pending_hex[i * 2] = kHex[(ack_hash[i] >> 4) & 0x0F];
        out_pending_hex[i * 2 + 1] = kHex[ack_hash[i] & 0x0F];
    }
    out_pending_hex[64] = '\0';
    if (out_pending_seq) {
        *out_pending_seq = seq;
    }
    return true;
}

bool miplay_safety_handle_auth_ack(miplay_session_t *session,
                                   const uint8_t *plaintext, uint32_t pt_len,
                                   const char *pending_hex, bool pending_valid,
                                   uint16_t pending_seq)
{
    if (!plaintext || pt_len < 6) {
        return false;
    }
    // 解开 envelope：tagLen(1) + tag(3) + valueType(4 LE) + dataLen(1) + json
    const uint8_t tag_len = plaintext[0];
    if (tag_len != 3 || pt_len < static_cast<uint32_t>(1 + tag_len + 4 + 1)) {
        ESP_LOGW(kTag, "SafetyAuthAck: bad envelope (tagLen=%u len=%lu)", tag_len,
                 static_cast<unsigned long>(pt_len));
        return false;
    }
    const uint32_t json_len = plaintext[5 + tag_len];
    if (json_len > pt_len - 6 - tag_len) {
        ESP_LOGW(kTag, "SafetyAuthAck: json_len %lu overruns",
                 static_cast<unsigned long>(json_len));
        return false;
    }
    const uint8_t *json = plaintext + 6 + tag_len;

    // 打印 json 段（只打这一小段，不打整包）：手机字段名/取值是逐版本演进
    // 的，解析失败时必须能看到它到底发了什么，否则只能盲猜。
    {
        char jbuf[256];
        const uint32_t n = json_len < sizeof(jbuf) - 1 ? json_len : (uint32_t)sizeof(jbuf) - 1;
        uint32_t o = 0;
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t c = json[i];
            jbuf[o++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
        }
        jbuf[o] = '\0';
        ESP_LOGI(kTag, "SafetyAuthAck json[%lu]: %s",
                 static_cast<unsigned long>(json_len), jbuf);
    }

    char peer_ack[65] = {0};
    if (!json_extract_string(json, json_len, "\"authMsgAck\"", peer_ack,
                             sizeof(peer_ack))) {
        ESP_LOGW(kTag, "SafetyAuthAck: authMsgAck not found");
    }

    // 校验手机对本机 authMsg 的 HMAC。不匹配说明密钥或算法约定不一致，
    // 必须断开——继续下去所有加密帧都会失败，问题会更晚才暴露。
    uint8_t expected_hash[32];
    miplay_hmac_sha256(reinterpret_cast<const uint8_t *>(session->auth_key), 32,
                       session->auth_msg, strlen(reinterpret_cast<const char *>(session->auth_msg)),
                       expected_hash);
    static const char kHex[] = "0123456789abcdef";
    char expected_hex[65];
    for (int i = 0; i < 32; i++) {
        expected_hex[i * 2] = kHex[(expected_hash[i] >> 4) & 0x0F];
        expected_hex[i * 2 + 1] = kHex[expected_hash[i] & 0x0F];
    }
    expected_hex[64] = '\0';

    if (strcmp(peer_ack, expected_hex) != 0) {
        ESP_LOGE(kTag, "SafetyAuthAck HMAC mismatch -> rejecting");
        ESP_LOGE(kTag, "  peer=%s", peer_ack);
        ESP_LOGE(kTag, "  exp =%s", expected_hex);
        return false;
    }
    ESP_LOGI(kTag, "mutual SafetyAuth verified");

    // 补发对手机 authMsg 的 ack。seq 用手机 0x1402 那帧的 seq——
    // 手机按 seq 匹配应答，用错 seq 会被丢弃。
    if (pending_valid) {
        char ack_json[160];
        const int aj_len = snprintf(
            ack_json, sizeof(ack_json),
            "{\n\t\"authMsgAck\": \"%s\",\n\t\"result\": \"0\" \n} \n",
            pending_hex);
        const int sr = miplay_send_encrypted_envelope(
            session, CMD_SAFETY_AUTH_ACK, pending_seq, true,
            reinterpret_cast<const uint8_t *>(ack_json),
            static_cast<uint32_t>(aj_len));
        ESP_LOGI(kTag, "-> SafetyAuthAck seq=%u ret=%d", pending_seq, sr);
    }
    return true;
}
