// MiPlay 控制会话状态实现。
#include "miplay_session.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/semphr.h"

namespace {
constexpr char kTag[] = "miplay_session";

// 控制会话槽。只在任务上下文访问（无 ISR/DMA 路径），可安全放 PSRAM。
// 静态分配避免每次握手都做堆分配——并发握手时的堆竞争会让 IV 初始化
// 与 socket 绑定错序。
EXT_RAM_BSS_ATTR miplay_session_t s_sessions[MIPLAY_MAX_CONTROL_SESSIONS];

SemaphoreHandle_t s_session_mux = nullptr;
SemaphoreHandle_t s_send_mux = nullptr;
uint32_t s_session_count = 0;

// 加密发送缓冲。256KB 级，静态放 PSRAM 以免反复 malloc/free 造成内部 SRAM
// 碎片化。注意：这是单份的，多会话并发发送需靠 s_send_mux 串行化。
EXT_RAM_BSS_ATTR uint8_t s_encrypt_buf[MIPLAY_MAX_FRAME_PAYLOAD +
                                       MIPLAY_FRAME_HDR_LEN + 32];
}  // namespace

void miplay_session_init(void)
{
    if (!s_session_mux) {
        s_session_mux = xSemaphoreCreateMutex();
    }
    if (!s_send_mux) {
        s_send_mux = xSemaphoreCreateMutex();
    }
    memset(s_sessions, 0, sizeof(s_sessions));
    s_session_count = 0;
    ESP_LOGI(kTag, "session table ready (max %d, stack %u KB)",
             MIPLAY_MAX_CONTROL_SESSIONS,
             static_cast<unsigned>(MIPLAY_CLIENT_TASK_STACK_BYTES / 1024));
}

bool miplay_session_lock(uint32_t timeout_ms)
{
    if (!s_session_mux) {
        return false;
    }
    return xSemaphoreTake(s_session_mux,
                          pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void miplay_session_unlock(void)
{
    if (s_session_mux) {
        xSemaphoreGive(s_session_mux);
    }
}

bool miplay_send_lock(uint32_t timeout_ms)
{
    if (!s_send_mux) {
        return false;
    }
    return xSemaphoreTake(s_send_mux, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void miplay_send_unlock(void)
{
    if (s_send_mux) {
        xSemaphoreGive(s_send_mux);
    }
}

miplay_session_t *miplay_session_alloc(int sock, uint32_t peer_ip,
                                       uint16_t peer_port)
{
    if (!miplay_session_lock(1000)) {
        return nullptr;
    }
    miplay_session_t *slot = nullptr;
    for (size_t i = 0; i < MIPLAY_MAX_CONTROL_SESSIONS; i++) {
        if (!s_sessions[i].in_use) {
            slot = &s_sessions[i];
            memset(slot, 0, sizeof(*slot));
            slot->in_use = true;
            slot->sock = sock;
            slot->peer_ip = peer_ip;
            slot->peer_port = peer_port;
            slot->notify_seq = 8;  // 协议不接受 0 号序号
            s_session_count++;
            break;
        }
    }
    miplay_session_unlock();
    return slot;
}

void miplay_session_release(miplay_session_t *session)
{
    if (!session) {
        return;
    }
    if (!miplay_session_lock(1000)) {
        return;
    }
    if (session->in_use) {
        session->in_use = false;
        session->task = nullptr;
        if (s_session_count > 0) {
            s_session_count--;
        }
    }
    miplay_session_unlock();
}

miplay_session_t *miplay_session_find_locked(int sock)
{
    for (size_t i = 0; i < MIPLAY_MAX_CONTROL_SESSIONS; i++) {
        if (s_sessions[i].in_use && s_sessions[i].sock == sock) {
            return &s_sessions[i];
        }
    }
    return nullptr;
}

miplay_session_t *miplay_session_get_active_locked(void)
{
    for (size_t i = 0; i < MIPLAY_MAX_CONTROL_SESSIONS; i++) {
        if (s_sessions[i].in_use && s_sessions[i].reverse_control_ready) {
            return &s_sessions[i];
        }
    }
    return nullptr;
}

uint16_t miplay_next_notify_seq(miplay_session_t *session)
{
    if (!session) {
        return 8;
    }
    int seq = static_cast<int>(session->notify_seq);
    if (seq < 8 || seq > UINT16_MAX) {
        seq = 8;
    }
    session->notify_seq = (seq == UINT16_MAX) ? 8U
                                              : static_cast<uint32_t>(seq + 1);
    return static_cast<uint16_t>(seq);
}

int miplay_send_encrypted(miplay_session_t *session, uint16_t cmd, uint16_t seq,
                          const uint8_t *payload, uint32_t payload_len)
{
    if (!session || !session->in_use || !session->has_session_key) {
        return -1;
    }
    if (payload_len > MIPLAY_MAX_FRAME_PAYLOAD) {
        return -1;
    }
    if (!miplay_send_lock(2000)) {
        return -1;
    }
    // 用会话自己的 key/iv 加密。iv 会被 CBC 就地演进，因此这一步必须在
    // 发送锁内完成，否则并发发送会取到中间态 IV。
    const int sd_len = miplay_safety_encrypt(payload, payload_len,
                                             session->aes_key,
                                             session->encrypt_iv,
                                             s_encrypt_buf,
                                             sizeof(s_encrypt_buf));
    int ret = -1;
    if (sd_len > 0) {
        ret = miplay_send_frame(session->sock, cmd, seq, s_encrypt_buf,
                                static_cast<uint32_t>(sd_len));
    } else {
        ESP_LOGW(kTag, "safety_encrypt failed (cmd=0x%04X len=%lu)", cmd,
                 static_cast<unsigned long>(payload_len));
    }
    miplay_send_unlock();
    return ret;
}

int miplay_send_encrypted_auto_seq(miplay_session_t *session, uint16_t cmd,
                                   const uint8_t *payload, uint32_t payload_len,
                                   uint16_t *seq_out)
{
    if (seq_out) {
        *seq_out = 0;
    }
    if (!session || !session->in_use || !session->has_session_key) {
        return -1;
    }
    if (payload_len > MIPLAY_MAX_FRAME_PAYLOAD) {
        return -1;
    }
    if (!miplay_send_lock(2000)) {
        return -1;
    }
    // 序号分配与加密发送同处一个临界区：分开做会让并发帧的发送顺序
    // 与序号顺序不一致，手机按序号重排时就会乱序丢弃。
    const uint16_t seq = miplay_next_notify_seq(session);
    const int sd_len = miplay_safety_encrypt(payload, payload_len,
                                             session->aes_key,
                                             session->encrypt_iv,
                                             s_encrypt_buf,
                                             sizeof(s_encrypt_buf));
    int ret = -1;
    if (sd_len > 0) {
        ret = miplay_send_frame(session->sock, cmd, seq, s_encrypt_buf,
                                static_cast<uint32_t>(sd_len));
    }
    miplay_send_unlock();
    if (seq_out) {
        *seq_out = seq;
    }
    return ret;
}
