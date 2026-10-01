// MiPlay 控制通道：8899 TCP 监听与握手状态机。
//
// 职责边界刻意收窄——本文件只负责：
//   ① 监听 8899 并 accept
//   ② 每个连接一个会话任务，跑帧解析循环
//   ③ 握手：0x28 DEVICE_ID → 0x36/0x37 版本 → 0x29 AUTH_ACK → 0x22 NOTIFY(5,6,7)
//      → 0x1400 SafetyInfo → 0x1402 SafetyAuth → 安全通道建立
//   ④ 心跳（0x1A/0x1B）与设备信息（0x1E/0x1F）
//
// 刻意不含媒体管线（OPEN/RTSP/WFD）。OPEN 帧只记日志并回 ACK，不建立媒体
// 会话——这样手机能完成识别与控制，而 RTP 端口、TS 解复用、PSRAM 音频缓冲
// 这些重资产不会污染控制路径。媒体层后续作为独立文件接入。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 启动 8899 监听。需在拿到 STA IP 之后调用。
esp_err_t miplay_control_start(void);

// 停止监听并关闭所有会话。幂等。
void miplay_control_stop(void);

// 是否已建立会话（手机已连上并完成握手）。
bool miplay_control_is_connected(void);

// 重置连接状态为 false（媒体流结束后调用，允许下一次 CMD_OPEN_DEVICE
// 重新触发连接回调）。不会断开控制通道。
void miplay_control_reset_connected(void);

// 注册连接状态变化回调。
typedef void (*miplay_connection_changed_fn)(bool connected);
void miplay_control_set_connection_callback(miplay_connection_changed_fn fn);

#ifdef __cplusplus
}
#endif
