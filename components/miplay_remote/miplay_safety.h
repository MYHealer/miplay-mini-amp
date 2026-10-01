// MiPlay 安全通道：0x1400 SafetyInfo → 0x1402 SafetyAuth 双向认证。
//
// 这一阶段建立"可信通道"——之后所有控制帧都以 SafetyData 容器加密。
// 握手顺序（顺序错会被手机直接断开）：
//   [手机→设备] 0x1400 SafetyInfo         手机提议参数
//   [设备→手机] 0x1401 SafetyInfoAck      **明文 envelope**，回选定的参数
//   [设备→手机] 0x1402 SafetyAuth         **加密** envelope，下发本机 authMsg
//   [手机→设备] 0x1402 SafetyAuth         手机回它的 authMsg
//   [手机→设备] 0x1403 SafetyAuthAck      手机对本机 authMsg 的 HMAC
//   [设备→手机] 0x1403 SafetyAuthAck      **加密** envelope，本机对手机 authMsg 的 HMAC
//
// 注意 0x1401 必须明文：此刻双方还没就加密参数达成一致，加密发过去手机会
// 解不开并直接断开——这个失败点表现为"发完 1401 连接立刻断"。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "mbedtls/sha256.h"
#include "miplay_session.h"

#ifdef __cplusplus
extern "C" {
#endif

// HMAC-SHA256。key 超过 64 字节时先做一次 SHA256 压缩。
void miplay_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg,
                        size_t msg_len, uint8_t out[32]);

// 发一个明文 envelope 帧（0x1401 用）。包装层不加密。
int miplay_send_plain_envelope(miplay_session_t *session, uint16_t cmd,
                               uint16_t seq, const uint8_t *payload,
                               uint32_t payload_len);

// 发一个加密 envelope 帧（0x1402/0x1403 用）。
// is_ack 决定 envelope 的 tag 是 "ack" 还是 "cmd"。
int miplay_send_encrypted_envelope(miplay_session_t *session, uint16_t cmd,
                                   uint16_t seq, bool is_ack,
                                   const uint8_t *payload, uint32_t payload_len);

// 处理 0x1400 SafetyInfo。
// 返回后 out_auth_challenge_sent 指示是否已下发本机 challenge。
void miplay_safety_handle_info(miplay_session_t *session, const uint8_t *payload,
                               uint32_t plen, uint16_t seq,
                               bool *auth_challenge_sent);

// 处理手机回的 0x1402 SafetyAuth（索取其 authMsg，算 HMAC 暂存）。
// 返回该 HMAC 的 hex（65 字节缓冲）与其 seq，供后续 0x1403 补发。
bool miplay_safety_handle_auth(miplay_session_t *session,
                               const uint8_t *plaintext, uint32_t pt_len,
                               uint16_t seq, char out_pending_hex[65],
                               uint16_t *out_pending_seq);

// 处理手机回的 0x1403 SafetyAuthAck（校验其 HMAC，通过则补发本机 ack）。
// 返回 true 表示双向认证完成。
bool miplay_safety_handle_auth_ack(miplay_session_t *session,
                                   const uint8_t *plaintext, uint32_t pt_len,
                                   const char *pending_hex, bool pending_valid,
                                   uint16_t pending_seq);

#ifdef __cplusplus
}
#endif
