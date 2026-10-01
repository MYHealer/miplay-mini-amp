// MiPlay 控制会话状态。
//
// 每个 TCP 控制连接占一个静态 slot。加密 IV、认证材料和序号必须按连接
// 隔离——CBC 链是连续演进的，两个连接共用 IV 会互相破坏解密。
//
// 设计取舍：参考实现把会话字段镜像到一组全局变量，旧代码直接读写全局，
// 靠 miplay_session_load/save 在临界区进出时同步。那是为了少改 6000 行旧
// 代码的妥协，代价是每个临界区都必须严格 load→处理→save，漏一处就串会话。
// 本工程从零写，直接以 session* 为参数传递，不做全局镜像。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "miplay_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

// 控制连接上限。手机可能同时开控制 + companion 两条连接。
#define MIPLAY_MAX_CONTROL_SESSIONS 3

// 会话任务栈。上游参考实现开 256KB——那是为 8MB PSRAM 的 RLCD 定的，
// 单会话占用是本机可用 PSRAM（2MB）的 12.5%，三条并发就吃掉 37.5%。
//
// 本工程按实测收敛到 64KB。依据：对整个组件做过栈上对象普查，该任务
// 及其调用链（client_task → miplay_safety_handle_* → mbedtls HMAC）
// 最大的栈对象是 miplay_control.cpp 的 `body[512]`，其次 safety 层的
// `jbuf[256]`；调用链总深度估算 < 8KB。64KB 留 8 倍余量。
//
// 若日后出现栈溢出（症状是随机 crash 或内存损坏），先调
// uxTaskGetStackHighWaterMark(session->task) 看真实水位再调大，
// 不要凭猜测改。MIPLAY_TCP_TASK_STACK_BYTES 不在该链路上，维持原值。
#define MIPLAY_CLIENT_TASK_STACK_BYTES (64U * 1024U)
#define MIPLAY_TCP_TASK_STACK_BYTES    (16U * 1024U)

typedef struct {
    bool in_use;
    int sock;
    uint32_t peer_ip;
    uint16_t peer_port;

    // 会话密钥材料。auth_key 是完整 32 hex（HMAC 用），aes_key/iv 取前 16 字节。
    uint8_t aes_key[16];
    uint8_t encrypt_iv[16];
    uint8_t decrypt_iv[16];
    char auth_key[33];

    // 设备下发的 32 hex 挑战应答，回填到 SafetyAuthAck。
    uint8_t auth_msg[33];

    bool has_session_key;
    uint32_t notify_seq;

    // 握手阶段标志。reverse_control_ready 置位后才允许下发反控帧，
    // 否则手机会丢弃——比"发了没反应"更难排查。
    bool reverse_control_ready;
    bool play_source_registered;
    bool media_session_opened;

    TaskHandle_t task;
} miplay_session_t;

// 会话槽表初始化。必须在任何 alloc 之前调用一次。
void miplay_session_init(void);

// 取一个空闲 slot 并填充连接信息；无空闲 slot 返回 NULL。
miplay_session_t *miplay_session_alloc(int sock, uint32_t peer_ip, uint16_t peer_port);

// 归还 slot。只清 in_use，不清密钥材料——清理由 disconnect 路径负责。
void miplay_session_release(miplay_session_t *session);

// 按 socket 查找会话。调用者需持有会话互斥锁（见 miplay_session_lock）。
miplay_session_t *miplay_session_find_locked(int sock);

// 获取第一个活跃且反控就绪的会话。调用者需持有会话互斥锁。
// 返回 NULL 表示无可用会话。
miplay_session_t *miplay_session_get_active_locked(void);

// 会话表互斥。所有对 s_sessions 的读写都必须在其保护下进行。
bool miplay_session_lock(uint32_t timeout_ms);
void miplay_session_unlock(void);

// 发送互斥：串行化"分配序号 + 更新 CBC IV + 写 socket"这一整段，
// 防止心跳任务与按键任务并发时序号顺序与网络顺序不一致。
bool miplay_send_lock(uint32_t timeout_ms);
void miplay_send_unlock(void);

// 在当前活跃会话上分配下一个 NOTIFY 序号。调用者必须已持有发送锁。
// 序号从 8 起，到 0xFFFF 回卷到 8——协议不接受 0 号。
uint16_t miplay_next_notify_seq(miplay_session_t *session);

// 用会话密钥加密并发送一帧。内部取发送锁。
int miplay_send_encrypted(miplay_session_t *session, uint16_t cmd, uint16_t seq,
                          const uint8_t *payload, uint32_t payload_len);

// 分配序号后加密发送（心跳/按键这类自发帧用）。seq_out 可为 NULL。
int miplay_send_encrypted_auto_seq(miplay_session_t *session, uint16_t cmd,
                                   const uint8_t *payload, uint32_t payload_len,
                                   uint16_t *seq_out);

#ifdef __cplusplus
}
#endif
