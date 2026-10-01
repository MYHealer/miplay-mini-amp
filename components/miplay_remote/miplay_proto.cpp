// MiPlay 控制协议：帧编解码与 SafetyData 密码学容器。
//
// 分层：
//   ① 帧层      —— 9 字节大端头（magic 0x24 + cmd + seq + len）
//   ② 完整性层  —— CRC-32/MPEG-2 变体（无最终 XOR，字节交换查找表）
//   ③ 加密层    —— AES-128-CBC，无内置填充，调用方负责对齐
//   ④ 容器层    —— SafetyData v1：9 字节头 + 密文（零填充 1~16 字节）
//
// 所有大缓冲走 PSRAM。本文件不含会话状态，纯函数，可独立单测。
#include "miplay_proto.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mbedtls/aes.h"

namespace {
constexpr char kTag[] = "miplay_proto";

// SafetyData v1 容器常量。
constexpr size_t kSafetyHdrLen = 9;
constexpr uint8_t kSafetyVersion = 1;
constexpr uint8_t kSafetyFlags = 0xE0;  // encryption | padding | integrity
constexpr size_t kAesBlock = 16;
}  // namespace

// ── ② 完整性层 ──

// CRC-32/MPEG-2 变体：逐字节生成查找表项，最后 bswap。
// 无最终 XOR —— 与手机侧 miplay_integrity() 一致，加 XOR 会导致校验恒失败。
static uint32_t crc32_table_entry(uint8_t index)
{
    uint32_t value = static_cast<uint32_t>(index) << 24;
    for (int i = 0; i < 8; i++) {
        value = (value & 0x80000000u) ? (value << 1) ^ 0x04C11DB7u : (value << 1);
    }
    return __builtin_bswap32(value);
}

uint32_t miplay_safety_integrity(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        const uint8_t idx = static_cast<uint8_t>(crc & 0xFF) ^ data[i];
        crc = crc32_table_entry(idx) ^ (crc >> 8);
    }
    return crc;  // 刻意不做最终 XOR
}

// ── ③ 加密层 ──

bool miplay_aes_cbc_encrypt(const uint8_t *key, uint8_t *iv,
                            const uint8_t *input, uint8_t *output, size_t len)
{
    if (len == 0 || len % kAesBlock != 0) {
        return false;
    }
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    if (mbedtls_aes_setkey_enc(&ctx, key, 128) != 0) {
        mbedtls_aes_free(&ctx);
        return false;
    }
    uint8_t iv_copy[kAesBlock];
    memcpy(iv_copy, iv, kAesBlock);
    const int ret = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_ENCRYPT, len,
                                          iv_copy, input, output);
    mbedtls_aes_free(&ctx);
    if (ret != 0) {
        return false;
    }
    // 回写演进后的 IV：协议要求 CBC 链跨帧连续，否则后续帧解不开。
    memcpy(iv, iv_copy, kAesBlock);
    return true;
}

bool miplay_aes_cbc_decrypt(const uint8_t *key, uint8_t *iv,
                            const uint8_t *input, uint8_t *output, size_t len)
{
    if (len == 0 || len % kAesBlock != 0) {
        return false;
    }
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    if (mbedtls_aes_setkey_dec(&ctx, key, 128) != 0) {
        mbedtls_aes_free(&ctx);
        return false;
    }
    uint8_t iv_copy[kAesBlock];
    memcpy(iv_copy, iv, kAesBlock);
    const int ret = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, len,
                                          iv_copy, input, output);
    mbedtls_aes_free(&ctx);
    if (ret != 0) {
        return false;
    }
    memcpy(iv, iv_copy, kAesBlock);
    return true;
}

// ── ④ 容器层 ──

int miplay_safety_encrypt(const uint8_t *plaintext, size_t pt_len,
                          const uint8_t *key, uint8_t *iv,
                          uint8_t *out, size_t out_max)
{
    if (!key || !iv || !out || (pt_len > 0 && !plaintext)) {
        return 0;
    }
    // 零填充 1~16 字节：即使明文已是块整数倍也要补一整块，
    // 否则解密端无法区分"填充长度"。pad_len 写入头部第 5 字节。
    const size_t pad_len = kAesBlock - (pt_len % kAesBlock);
    const size_t padded_len = pt_len + pad_len;
    if (kSafetyHdrLen + padded_len > out_max) {
        return 0;
    }

    uint8_t *padded = static_cast<uint8_t *>(
        heap_caps_calloc(1, padded_len, MALLOC_CAP_SPIRAM));
    if (!padded) {
        return 0;
    }
    if (pt_len > 0) {
        memcpy(padded, plaintext, pt_len);
    }

    uint8_t *ciphertext = out + kSafetyHdrLen;
    if (!miplay_aes_cbc_encrypt(key, iv, padded, ciphertext, padded_len)) {
        free(padded);
        return 0;
    }
    free(padded);

    // 9 字节头：[0x00, 0x07, version, flags, padLen, CRC(4 BE)]
    // 前两字节是 headerLenMinusTwo，解密端按此校验，写错直接拒收。
    const uint32_t crc = miplay_safety_integrity(ciphertext, padded_len);
    out[0] = 0x00;
    out[1] = 0x07;
    out[2] = kSafetyVersion;
    out[3] = kSafetyFlags;
    out[4] = static_cast<uint8_t>(pad_len);
    out[5] = static_cast<uint8_t>((crc >> 24) & 0xFF);
    out[6] = static_cast<uint8_t>((crc >> 16) & 0xFF);
    out[7] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    out[8] = static_cast<uint8_t>(crc & 0xFF);

    return static_cast<int>(kSafetyHdrLen + padded_len);
}

int miplay_safety_decrypt(const uint8_t *data, size_t data_len,
                          const uint8_t *key, uint8_t *iv,
                          uint8_t *out, size_t out_max)
{
    if (data_len < kSafetyHdrLen) {
        return -1;
    }
    if (data[0] != 0x00 || data[1] != 0x07) {
        ESP_LOGW(kTag, "safety_decrypt: bad header %02X %02X", data[0], data[1]);
        return -1;
    }
    if (data[2] != kSafetyVersion) {
        ESP_LOGW(kTag, "safety_decrypt: version %u unsupported", data[2]);
        return -1;
    }
    const uint8_t flags = data[3];
    if (!(flags & 0x80)) {
        ESP_LOGW(kTag, "safety_decrypt: not encrypted (flags=0x%02X)", flags);
        return -1;
    }

    const size_t pad_len = (flags & 0x40) ? data[4] : 0;
    const uint32_t expected_crc = (static_cast<uint32_t>(data[5]) << 24) |
                                  (static_cast<uint32_t>(data[6]) << 16) |
                                  (static_cast<uint32_t>(data[7]) << 8) |
                                  static_cast<uint32_t>(data[8]);
    const size_t ct_len = data_len - kSafetyHdrLen;
    if (ct_len == 0 || ct_len % kAesBlock != 0) {
        return -1;
    }
    // 先验完整性再解密：省掉一次无效 AES 运算，也让篡改帧留下明确日志。
    if (miplay_safety_integrity(data + kSafetyHdrLen, ct_len) != expected_crc) {
        ESP_LOGW(kTag, "safety_decrypt: crc mismatch");
        return -1;
    }
    if (pad_len > ct_len) {
        ESP_LOGW(kTag, "safety_decrypt: pad_len %u > ct_len %u",
                 static_cast<unsigned>(pad_len), static_cast<unsigned>(ct_len));
        return -1;
    }

    const size_t pt_len = ct_len - pad_len;
    if (pt_len > out_max) {
        ESP_LOGW(kTag, "safety_decrypt: pt_len %u > out_max %u",
                 static_cast<unsigned>(pt_len), static_cast<unsigned>(out_max));
        return -1;
    }

    uint8_t *decrypted = static_cast<uint8_t *>(
        heap_caps_malloc(ct_len, MALLOC_CAP_SPIRAM));
    if (!decrypted) {
        return -1;
    }
    if (!miplay_aes_cbc_decrypt(key, iv, data + kSafetyHdrLen, decrypted, ct_len)) {
        ESP_LOGW(kTag, "safety_decrypt: aes failed");
        free(decrypted);
        return -1;
    }

    // 校验零填充：填充区非零说明密钥或 IV 不对（CBC 会把错误扩散到全块），
    // 比 CRC 更早暴露"能解但解错"的情况。
    for (size_t i = pt_len; i < ct_len; i++) {
        if (decrypted[i] != 0) {
            ESP_LOGW(kTag, "safety_decrypt: bad pad at %u=0x%02X",
                     static_cast<unsigned>(i), decrypted[i]);
            free(decrypted);
            return -1;
        }
    }
    memcpy(out, decrypted, pt_len);
    free(decrypted);
    return static_cast<int>(pt_len);
}

// ── ① 帧层 ──

int miplay_socket_send_all(int sock, const void *data, size_t len)
{
    const uint8_t *p = static_cast<const uint8_t *>(data);
    size_t sent = 0;
    while (sent < len) {
        const int n = send(sock, p + sent, len - sent, 0);
        if (n <= 0) {
            return n < 0 ? n : -1;
        }
        sent += static_cast<size_t>(n);
    }
    return static_cast<int>(sent);
}

int miplay_send_frame(int sock, uint16_t cmd, uint16_t seq,
                      const uint8_t *payload, uint32_t payload_len)
{
    uint8_t hdr[MIPLAY_FRAME_HDR_LEN];
    hdr[0] = MIPLAY_FRAME_MAGIC;
    hdr[1] = static_cast<uint8_t>((cmd >> 8) & 0xFF);
    hdr[2] = static_cast<uint8_t>(cmd & 0xFF);
    hdr[3] = static_cast<uint8_t>((seq >> 8) & 0xFF);
    hdr[4] = static_cast<uint8_t>(seq & 0xFF);
    hdr[5] = static_cast<uint8_t>((payload_len >> 24) & 0xFF);
    hdr[6] = static_cast<uint8_t>((payload_len >> 16) & 0xFF);
    hdr[7] = static_cast<uint8_t>((payload_len >> 8) & 0xFF);
    hdr[8] = static_cast<uint8_t>(payload_len & 0xFF);

    int ret = miplay_socket_send_all(sock, hdr, MIPLAY_FRAME_HDR_LEN);
    if (ret < 0) {
        return ret;
    }
    if (payload_len > 0 && payload) {
        ret = miplay_socket_send_all(sock, payload, payload_len);
        if (ret < 0) {
            return ret;
        }
    }
    ESP_LOGD(kTag, "TX cmd=0x%04X seq=%u len=%lu", cmd, seq,
             static_cast<unsigned long>(payload_len));
    return MIPLAY_FRAME_HDR_LEN + static_cast<int>(payload_len);
}

// ── SafetyEnvelope（OPack 子集，仅 0x14 Safety 阶段使用）──
//
// tagLen(1) + tag("cmd"/"ack") + valueType(4 LE) + dataLen(1) + data
// 注意 valueType 占 4 字节小端（常量 30 = 0x1E），dataLen 只占 1 字节——
// 因此 payload 上限 255，超过会静默截断，调用方需自行保证。
int miplay_safety_envelope_encode(bool is_ack, uint8_t value_type,
                                  const uint8_t *payload, uint32_t payload_len,
                                  uint8_t *out, size_t out_max)
{
    const uint8_t *tag = reinterpret_cast<const uint8_t *>(is_ack ? "ack" : "cmd");
    constexpr size_t kTagLen = 3;
    const size_t total = 1 + kTagLen + 4 + 1 + payload_len;
    if (total > out_max || payload_len > 255) {
        return 0;
    }
    size_t off = 0;
    out[off++] = static_cast<uint8_t>(kTagLen);
    memcpy(out + off, tag, kTagLen);
    off += kTagLen;
    out[off++] = value_type;
    out[off++] = 0x00;
    out[off++] = 0x00;
    out[off++] = 0x00;
    out[off++] = static_cast<uint8_t>(payload_len);
    memcpy(out + off, payload, payload_len);
    return static_cast<int>(off + payload_len);
}
