// MiPlay 控制协议：帧编解码与 SafetyData 密码学容器（接口）。
//
// 这些函数是纯函数、无全局状态，可脱离会话单独测试。会话级状态
// （AES key/IV、序号）由 miplay_session 层持有。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lwip/sockets.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── 协议常量 ──

// 帧头：magic(1) + cmd(2 BE) + seq(2 BE) + payload_len(4 BE)
#define MIPLAY_FRAME_MAGIC   0x24
#define MIPLAY_FRAME_HDR_LEN 9

// 控制命令字（手机 ↔ 接收端）。
#define CMD_OPEN_DEVICE          0x0000
#define CMD_PAUSE                0x0004
#define CMD_RESUME               0x0006
#define CMD_SET_VOLUME           0x000C
#define CMD_GET_VOLUME           0x000E
#define CMD_GET_MEDIA_INFO       0x0014
#define CMD_GET_MEDIA_INFO_ACK   0x0015
#define CMD_GET_STATE            0x001C
#define CMD_HEARTBEAT            0x001A
#define CMD_HEARTBEAT_ACK        0x001B
#define CMD_GET_DEVICE_INFO      0x001E
#define CMD_GET_DEVICE_INFO_ACK  0x001F
#define CMD_SAFETY_CHALLENGE     0x0028  // 设备下发 14 位挑战号
#define CMD_SAFETY_ACK           0x0029  // 手机回 32 hex
#define CMD_NOTIFY               0x0022
#define CMD_GET_MIRROR_MODE      0x0034
#define CMD_GET_MIRROR_MODE_ACK  0x0035
#define CMD_NATIVE_VERSION       0x0036
#define CMD_NATIVE_VERSION_ACK   0x0037
#define CMD_SET_PLAY_SOURCE      0x0040
#define CMD_SET_POSITION         0x0056
#define CMD_SET_MEDIA_INFO       0x0012
#define CMD_SET_MEDIA_INFO_ACK   0x0013
#define CMD_SET_LOCAL_DEV_INFO   0x0058
#define CMD_SET_LOCAL_DEV_ACK    0x0059
#define CMD_SET_MEDIA_STATE      0x005E
#define CMD_SET_MEDIA_STATE_ACK  0x005F
#define CMD_SAFETY_INFO          0x1400
#define CMD_SAFETY_INFO_ACK      0x1401
#define CMD_SAFETY_AUTH          0x1402
#define CMD_SAFETY_AUTH_ACK      0x1403

// SafetyData valueType 常量。
#define MIPLAY_SAFETY_VALUE_TYPE 30

// 单帧 payload 上限。本工程**刻意保留上游原值 256KB，不做下调**。
//
// 曾计划砍到 64KB，核查后被否决，理由如下：
//
//  ① 上游在同样的代码处留了警告：「缩小它会在收到大帧时截断并失步」。
//     该注释写在接收缓冲分配点，是本值唯一的直接经验依据。
//
//  ② 本值同时是安全校验上界：frame_hdr_plausible() 用它拒绝超限帧，
//     而 dec_buf 按 wire_plen 分配（miplay_control.cpp:705）。两者必须
//     同源——改小之后，合法大帧会在 recv 侧被截断成半帧，帧头错位，
//     后续所有帧永久失步。这是一个「改小即损坏协议」的强耦合点。
//
//  ③ 上游日志里 MiPlay 控制通道实测最大帧只有 7257 字节（且是握手阶段
//     的小帧），看上去 64KB 余量充足。但那是握手期日志，没有覆盖
//     播放期的封面推送路径；现有证据不足以证明封面帧 < 64KB。
//     证据不足时不下调，是这里唯一安全的选择。
//
// 结论：省这 192KB 换来的失步风险不划算（本机 PSRAM 2MB，256KB 占 12.5%，
// 余量仍充裕）。本工程无屏幕，收到封面帧会直接跳过不解码，但缓冲
// 仍需容得下整帧才能正确跳过。
#define MIPLAY_MAX_FRAME_PAYLOAD (256U * 1024U)
#define MIPLAY_RX_BUF_LEN        (MIPLAY_MAX_FRAME_PAYLOAD + MIPLAY_FRAME_HDR_LEN)

// ── ① 帧层 ──

// 循环写满 len 字节，处理短写。返回已发送字节数，失败返回负值。
int miplay_socket_send_all(int sock, const void *data, size_t len);

// 组装并发送一帧。不加锁——调用者需自行串行化（见会话层的发送互斥）。
int miplay_send_frame(int sock, uint16_t cmd, uint16_t seq,
                      const uint8_t *payload, uint32_t payload_len);

// ── ② 完整性层 ──

// CRC-32/MPEG-2 变体，无最终 XOR。与手机侧 miplay_integrity() 对齐。
uint32_t miplay_safety_integrity(const uint8_t *data, size_t len);

// ── ③ 加密层 ──
//
// CBC 模式下 iv 会被原地更新为最后一个密文块，供下一帧续用。
// 因此 iv 必须按会话隔离，跨会话共享会导致解密失败。

bool miplay_aes_cbc_encrypt(const uint8_t *key, uint8_t *iv,
                            const uint8_t *input, uint8_t *output, size_t len);

bool miplay_aes_cbc_decrypt(const uint8_t *key, uint8_t *iv,
                            const uint8_t *input, uint8_t *output, size_t len);

// ── ④ 容器层 ──

// 明文 → SafetyData v1 容器。返回写入 out 的字节数，失败返回 0。
int miplay_safety_encrypt(const uint8_t *plaintext, size_t pt_len,
                          const uint8_t *key, uint8_t *iv,
                          uint8_t *out, size_t out_max);

// SafetyData v1 容器 → 明文。返回明文长度，失败返回 -1。
int miplay_safety_decrypt(const uint8_t *data, size_t data_len,
                          const uint8_t *key, uint8_t *iv,
                          uint8_t *out, size_t out_max);

// ── SafetyEnvelope（仅 0x14 阶段）──

int miplay_safety_envelope_encode(bool is_ack, uint8_t value_type,
                                  const uint8_t *payload, uint32_t payload_len,
                                  uint8_t *out, size_t out_max);

#ifdef __cplusplus
}
#endif
