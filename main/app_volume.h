// 软件音量。
//
// PCM5102A 是纯 DAC，没有 I2C 控制口，音量只能在 int16 PCM 上做数字缩放。
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 初始化，设定开机默认音量（0-100）。
void app_volume_init(int default_percent);

// 设置音量百分比（0-100），越界自动钳位。
void app_volume_set(int percent);

// 读取当前音量百分比。
int app_volume_get(void);

// 对交错立体声样本做原地缩放。sample_count 是**样本总数**（帧数 × 声道数），
// 不是帧数。音量 >=100 时直接返回，不做无谓的乘法。
void app_volume_apply(int16_t *samples, size_t sample_count);

#ifdef __cplusplus
}
#endif
