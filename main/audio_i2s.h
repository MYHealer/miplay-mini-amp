// PCM5102A 输出层：I2S 标准模式 + 采样率自适应 + 软件音量。
//
// 只依赖 driver/i2s_std.h。刻意不用 esp_codec_dev——那个组件是给
// 「带 I2C 控制口的 codec」做抽象用的，PCM5102A 没有控制口，多引一层
// 抽象只是负担。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 创建 I2S 通道。由 miplay 的 stream_start 回调调用。
// 返回 false 表示创建失败——协议层会据此中止本次流（手机端表现为
// 投屏立即结束），所以失败原因必须打日志。
bool audio_i2s_stream_start(void);

// 销毁 I2S 通道。由 stream_stop 回调调用。
//
// 为什么必须在这里删掉通道而不是常驻：AAC 流的采样率在流开始前未知，
// 通道常驻则首次必须猜采样率。删掉后每次流都按实测值重建，无需猜测。
void audio_i2s_stream_stop(void);

// 写入一帧立体声 PCM。签名与 miplay_write_speaker_fn 完全一致。
//
// frame_count 是**帧数**（每帧含左右两个样本），不是样本总数。
// 返回值语义由协议层决定：返回 0 表示成功。
int audio_i2s_write(const int16_t *stereo_samples, size_t frame_count,
                    int sample_rate);

#ifdef __cplusplus
}
#endif
