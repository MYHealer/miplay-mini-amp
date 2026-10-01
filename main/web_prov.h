// 配网 Web 后台 + 强制门户。
//
// 只在配网热点开着的时候跑。它做四件事：
//   1. 提供 /、/api/scan、/api/connect、/api/info 四个端点（页面靠它们工作）
//   2. 应答各平台的"联网探测 URL"并 302 回配网页——这就是强制门户
//   3. /api/* 兜底，避免页面 JS 打到未注册路径拿到 404 空响应
//   4. 连接成功后回调上层，由 app_main 决定关热点、起 MiPlay
//
// 本模块不碰 Wi-Fi 状态机：连上了该做什么是 app_main 的判断，
// 这里只负责"把用户填的密码交给 wifi_sta 试一次，结果如实回报"。
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 连接成功的通知。由 app_main 注册。
// 注意：回调在 HTTP 服务器任务的上下文里执行，实现方必须尽快返回——
// 页面正等着这个响应，在这里做长耗时的事会让手机看到超时。
typedef void (*web_prov_connected_fn)(const char *ssid);

// 启动服务器。需在 wifi_ap_start() 之后调用（监听地址取决于 AP netif 是否就绪）。
// 重复调用返回 ESP_OK。
esp_err_t web_prov_start(web_prov_connected_fn on_connected);

// 停止并释放。可反复调用。
void web_prov_stop(void);

#ifdef __cplusplus
}
#endif