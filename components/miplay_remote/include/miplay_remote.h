// MiPlay 遥控接收端：让设备被小米「设备互联」识别为 TV，并接收遥控按键。
//
// 本组件从 esp-miply-1.85touch 的 components/miplay 移植，只保留
// 「被手机发现 + 建立控制会话」链路，剥离了 RTSP 投屏、音频管线、
// DLNA、歌词封面等无关功能。
//
// 链路全景（四层，缺一不可）：
//   ① 局域网发现：mDNS 注册(_mi-connect/_lyra-mdns) + UDP 5355 DNS-SD 应答
//      + mDNS 主动宣告
//   ② TCP 8899 控制口：手机据此建立 MiPlay 会话并获知设备 IP
//   ③ HTTP 6095 认证：airkan requestAuth/completeAuth（第二阶段）
//   ④ TCP 6091 长连：接收遥控按键帧（第二阶段）
//
// 本次实现 ①②。③④ 的代码已在源工程验证通过，待第二阶段移入。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 启动 MiPlay 遥控接收端：初始化身份、注册 mDNS、启动发现与 8899 监听。
// 需要 Wi-Fi 已连接（依赖 STA netif 的 IP）。
// 返回 ESP_OK 表示服务已启动；失败时调用方不应为其持有射频唤醒锁。
// 重复调用是安全的（只生效一次，返回 ESP_OK）。
esp_err_t miplay_remote_start(void);

// 停止所有相关任务并关闭监听 socket。
void miplay_remote_stop(void);

// 是否有手机成功建立控制会话。
bool miplay_remote_is_connected(void);

// 重置连接状态（媒体流结束后调用，允许下一次投屏重新触发连接回调）。
void miplay_remote_reset_connected(void);

// 注册音频输出回调（由 main 调用，解耦 miplay_remote 与 audio_services）。
// 签名：立体声 PCM，frame_count 是样本对数（非总样本数）。
typedef int (*miplay_write_speaker_fn)(const int16_t *stereo_samples,
                                       size_t frame_count, int sample_rate);
void miplay_media_set_speaker_callback(miplay_write_speaker_fn fn);

// 注册投屏音频流生命周期回调。
// stream_start 在媒体接收任务启动时调用（获取 codec），stream_stop 在结束时调用（释放）。
typedef bool (*miplay_stream_start_fn)(void);
typedef void (*miplay_stream_stop_fn)(void);
void miplay_media_set_stream_callback(miplay_stream_start_fn start_fn,
                                      miplay_stream_stop_fn stop_fn);

// 注册连接状态变化回调（由 main 调用，用于投屏时自动切页）。
typedef void (*miplay_connection_changed_fn)(bool connected);
void miplay_remote_set_connection_callback(miplay_connection_changed_fn fn);

// 注册音量回调（由 main 调用，统一 MiPlay 音量与设备硬件音量）。
typedef void (*miplay_volume_set_fn)(int percent);
typedef int (*miplay_volume_get_fn)(void);
void miplay_media_set_volume_callback(miplay_volume_set_fn set_fn,
                                       miplay_volume_get_fn get_fn);

// 注册媒体元数据回调（由 main 调用，用于歌词获取和 UI 更新）。
// 手机投屏时通过 SET_MEDIA_INFO(0x12) 推送歌曲信息。
typedef void (*miplay_media_meta_fn)(const char *title, const char *artist,
                                     const char *album, int64_t duration_ms,
                                     int64_t position_ms);
void miplay_media_set_meta_callback(miplay_media_meta_fn fn);

// 反控命令：向手机发送播放控制指令。
// 返回 0 表示成功，-1 表示无活跃会话或发送失败。
int miplay_remote_seek(int64_t target_ms);   // 绝对位置跳转（毫秒）
int miplay_remote_next_track(void);           // 下一首
int miplay_remote_prev_track(void);           // 上一首
int miplay_remote_volume(uint32_t percent);   // 设置绝对音量（0-100）
int miplay_remote_volume_step(int delta);     // 相对音量调整（如 +5/-5）

// 手机发送 CMD_PAUSE(0x0004)/CMD_RESUME(0x0006) 时通知 UI 层。
typedef void (*miplay_media_pause_fn)(bool paused);
void miplay_media_set_pause_callback(miplay_media_pause_fn fn);

#ifdef __cplusplus
}
#endif
