// Wi-Fi 凭据的 NVS 持久化。
//
// 独立成模块的理由：app_main 的状态机只关心"能不能连上"，不关心凭据
// 存在哪、叫什么键。日后要加"长按清除配置"之类的入口，改这一处即可。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 读凭据。成功返回 true 并填入调用方缓冲（含 NUL）。
//
// ssid_len / pass_len 是缓冲容量。二者都必须显式传，不能用 sizeof——
// 本模块不假设调用方的缓冲尺寸。
bool wifi_creds_load(char *ssid, size_t ssid_len, char *pass, size_t pass_len);

// 写凭据。覆盖旧值。
bool wifi_creds_save(const char *ssid, const char *pass);

// 清除凭据（下次启动直接进配网态）。
bool wifi_creds_clear(void);

#ifdef __cplusplus
}
#endif