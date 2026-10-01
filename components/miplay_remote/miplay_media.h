// MiPlay 媒体播放层：RTSP 信令 + RTP 接收 + TS 解密 + AAC 解码。
//
// 从 dlna/components/miplay/miplay.c 移植，适配 RLCD_CLOCK 的音频输出
// （write_xiaozhi_speaker 替代 GMF pipeline）。
//
// 调用链：OPEN_DEVICE → handle_open_device() → miplay_rtsp_run() 任务
//         → RTSP 握手 → media_receive_task() → AAC 解码 → audio_services
//
// SetMirrorKey (0x006C) 由 miplay_control.cpp 调用 miplay_media_set_stream_key()。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "miplay_session.h"

#ifdef __cplusplus
extern "C" {
#endif

// ── OPEN_DEVICE 处理入口 ──
// 解析 wfd://host:port?mirrorMode=1，发送 ACK，启动 RTSP 任务。
// 由 miplay_control.cpp 的 CMD_OPEN_DEVICE case 调用。
// session: 当前控制会话（用于发 ACK 和取 peer/local 地址）。
// payload/plen: 解密后的 OPEN 帧 payload。
// seq: 帧序号（ACK 需用同一 seq）。
void miplay_media_handle_open_device(miplay_session_t *session,
                                     const uint8_t *payload, uint32_t plen,
                                     uint16_t seq);

// ── SetMirrorKey 处理 ──
// 解析 JSON 中的 streamKey/streamIV/authKey，存入媒体层静态变量。
// 由 miplay_control.cpp 的 CMD 0x006C case 调用。
void miplay_media_set_stream_key(const uint8_t *payload, uint32_t plen);

// ── 媒体状态查询 ──
bool miplay_media_is_streaming(void);

// ── 音量控制（外部写入，media_receive_task 读取）──
void miplay_media_set_volume(uint32_t percent);
uint32_t miplay_media_get_volume(void);

// ── 音量回调注册（由 miplay_remote.cpp 委托调用）──
typedef void (*miplay_media_volume_set_fn)(int percent);
typedef int (*miplay_media_volume_get_fn)(void);
void miplay_media_set_volume_cb(miplay_media_volume_set_fn set_fn,
                                 miplay_media_volume_get_fn get_fn);

// ── 停止媒体流 ──
// 递增 generation 使旧任务自动退出。
void miplay_media_stop(void);

// ── 音频输出回调注册（由 main 调用，注册 write_stereo_speaker 函数指针）──
typedef int (*miplay_write_speaker_fn)(const int16_t *stereo_samples,
                                       size_t frame_count, int sample_rate);
void miplay_media_set_speaker_callback(miplay_write_speaker_fn fn);

// ── 投屏音频流生命周期回调注册 ──
typedef bool (*miplay_stream_start_fn)(void);
typedef void (*miplay_stream_stop_fn)(void);
void miplay_media_set_stream_cb(miplay_stream_start_fn start_fn,
                                miplay_stream_stop_fn stop_fn);

// ── 媒体元数据回调注册 ──
// 由 miplay_remote.cpp 委托调用，存储回调指针。
// miplay_control.cpp 收到 SET_MEDIA_INFO 时调用 miplay_media_dispatch_meta()。
typedef void (*miplay_media_meta_fn)(const char *title, const char *artist,
                                     const char *album, int64_t duration_ms,
                                     int64_t position_ms);
void miplay_media_set_meta_cb(miplay_media_meta_fn fn);
void miplay_media_dispatch_meta(const char *title, const char *artist,
                                const char *album, int64_t duration_ms,
                                int64_t position_ms);

// ── 暂停/恢复状态回调注册 ──
// miplay_control.cpp 收到 CMD_PAUSE/CMD_RESUME 时调用 miplay_media_dispatch_pause()。
typedef void (*miplay_media_pause_fn)(bool paused);
void miplay_media_set_pause_cb(miplay_media_pause_fn fn);
void miplay_media_dispatch_pause(bool paused);

#ifdef __cplusplus
}
#endif
