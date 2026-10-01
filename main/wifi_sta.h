// Wi-Fi 三态管理：STA 连接、AP 配网热点、扫描。
//
// 本模块只管"怎么连"，不管"什么时候连"——重试策略、何时开热点、
// 何时关热点，全部由 app_main 的状态机决定。这样超时与重试次数
// 都能被上层控制，而不是埋在一个无限重连循环里出不来。
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// 一次性初始化：NVS / netif(STA) / 事件循环 / esp_wifi_init，并以 STA 模式启动。
// 幂等，重复调用只生效一次。必须在其他函数之前调用。
esp_err_t wifi_init_once(void);

// 扫描可见 AP，结果按信号强度降序且同名 SSID 已去重。
// 返回实际条数；失败返回 -1。
//
// 注意：内部会确保 Wi-Fi 处于 STA 或 APSTA 模式——esp_wifi_scan_start()
// 在纯 AP 模式下不可用。
int wifi_scan(wifi_ap_record_t *out, int max);

// 连接一个 AP，最多阻塞 timeout_ms。
//
// 认证类失败（密码错、找不到 AP）会立即返回 false 而不是耗满超时——
// 同一个密码重试不会成功，让用户早一点看到"密码错了"更有用。
bool wifi_sta_connect(const char *ssid, const char *pass, int timeout_ms);

// 断开 STA。不改变模式。
//
// ⚠ 命名不是 wifi_sta_disconnect：那个名字在 libnet80211.a(ieee80211_ioctl.o)
// 里是 IDF 自己的全局符号（同名出现 9 次），链接期必报 multiple definition。
// 我们用 C 链接的短通用名，撞进了 Wi-Fi 驱动的保留名域——改名是唯一干净的解法。
void wifi_sta_drop(void);

// 开配网热点。必须在 wifi_init_once() 之后调用。
//
// 以 APSTA 起（而非纯 AP）是因为扫描需要 STA 侧在场；STA 侧不连接，
// 只用于扫描。热点为开放网络，SSID 形如 "MiPlay-Amp-A1B2"。
esp_err_t wifi_ap_start(void);

// 关热点并销毁 AP netif，回到纯 STA。
void wifi_ap_stop(void);

// 当前是否已拿到 IP。供状态机判断"能不能启 MiPlay"。
bool wifi_is_connected(void);

#ifdef __cplusplus
}
#endif