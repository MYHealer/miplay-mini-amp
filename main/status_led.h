// 状态指示：板载 WS2812（ESP32-S3 SuperMini 在 GPIO48）。
//
// 无屏幕设备唯一的反馈通道。若板子没有灯，把 MIPLAY_LED_ENABLE 关掉，
// 本模块所有函数退化为空实现，不影响音频功能。
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// 配色遵循一条主线：**橙色 = 设备还没上互联网，蓝色 = 已经上了**。
// 这是无屏幕设备唯一能表达"我能不能用"的通道，所以这两种状态必须一眼可辨。
//
// STATUS_BOOT 刻意用白而非蓝：蓝已经专属"已联网"，同色系会让"启动中"
// 被误读成"已经好了"。
typedef enum {
    STATUS_BOOT = 0,      // 启动中：白色呼吸
    STATUS_WIFI,          // STA 连接中：橙色闪烁（还没连上）
    STATUS_AP,            // 配网热点已开：橙色常亮（还没连上，等用户填密码）
    STATUS_IDLE,          // 已联网待机：蓝色低亮常亮
    STATUS_CONNECTED,     // 手机已连接：青色常亮
    STATUS_PLAYING,       // 播放中：紫色常亮
    STATUS_ERROR,         // 错误：红色闪烁
} status_led_state_t;

// 初始化驱动并启动刷新任务。失败只打日志，不阻断启动——
// 一颗状态灯不值得让整个耳放起不来。
void status_led_init(void);

// 切换状态。可从任意任务调用。
void status_led_set(status_led_state_t state);

#ifdef __cplusplus
}
#endif
