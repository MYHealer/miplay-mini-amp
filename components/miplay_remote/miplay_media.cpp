// MiPlay 媒体播放层实现：RTSP 信令 + RTP 接收 + TS 解密 + AAC 解码。
//
// 从 dlna/components/miplay/miplay.c 移植，关键差异：
//   - 音频输出：write_xiaozhi_speaker() 替代 GMF ring buffer
//   - AAC 解码：esp_aac_dec API 替代 GMF aud_dec 组件
//   - 无 DLNA 互斥：RLCD_CLOCK 不跑 DLNA，去掉 connected_cb
//   - 会话状态：复用 miplay_session_t 的 peer/local 地址做密钥派生
#include "miplay_media.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mbedtls/aes.h"
#include "mbedtls/md5.h"
#include "mbedtls/sha256.h"

#include "miplay_proto.h"
#include "miplay_remote_internal.h"
#include "miplay_safety.h"

// 音频输出回调（由 main 注册，避免组件循环依赖）。
namespace {
using WriteSpeakerFn = int (*)(const int16_t *, size_t, int);
WriteSpeakerFn s_write_speaker = nullptr;
using StreamStartFn = bool (*)(void);
using StreamStopFn = void (*)(void);
StreamStartFn s_stream_start = nullptr;
StreamStopFn s_stream_stop = nullptr;
}

void miplay_media_set_speaker_callback(WriteSpeakerFn fn)
{
    s_write_speaker = fn;
}

void miplay_media_set_stream_cb(StreamStartFn start_fn, StreamStopFn stop_fn)
{
    s_stream_start = start_fn;
    s_stream_stop = stop_fn;
}

// AAC 解码器（esp_audio_codec managed component）。
#include "decoder/impl/esp_aac_dec.h"

namespace {
constexpr char kTag[] = "miplay_media";

// ── 协议常量（对齐 miplay.c）──
constexpr int kTsPktSize = 188;
constexpr uint8_t kTsSyncByte = 0x47;
constexpr uint16_t kTargetPid = 0x1100;
constexpr int kEncryptPrefix = 256;
constexpr int kAesBlockLen = 16;

// RTSP 缓冲区。
constexpr size_t kRtspBufSize = 4096;
constexpr int kRtspRetryCount = 3;
constexpr int kRtspRetryBackoffMs = 250;

// 任务栈。
constexpr uint32_t kRtspTaskStack = 48 * 1024;

// WFD 能力声明（对齐 miplay.c 实测值）。
const char kWfdCapabilities[] =
    "wfd_audio_codecs_v2: 15 3 3\r\n"
    "wfd_video_formats: none\r\n"
    "wfd_video_enctype: none\r\n"
    "wfd_video_gamuttype: none\r\n"
    "wfd_video_bitrate: none\r\n"
    "wfd_current_video_info: none\r\n"
    "wfd_client_rtp_ports: RTP/AVP/TCP;interleaved mode=play\r\n"
    "miplay_support_image: none\r\n"
    "wfd_standby_resume_capability: supported\r\n"
    "wfd_content_SP_protection: 4 1 256 3 1 1 0 0\r\n"
    "wfd_support_secure_win:enable\r\n"
    "device_info: -1 -1 -1 -1 -1 -1 -1\r\n";

// ── 媒体会话状态（每次 OPEN 递增，旧任务自退出）──
volatile uint32_t s_media_generation = 0;

// ── SetMirrorKey 来源的流加密密钥 ──
uint8_t s_stream_key[16];
uint8_t s_stream_iv[16];
volatile bool s_has_stream_key = false;
char s_mirror_auth_key[33];
volatile bool s_has_mirror_auth_key = false;

// ── 音量回调（由 main 注册，统一设备硬件音量）──
namespace {
miplay_media_volume_set_fn s_volume_set = nullptr;
miplay_media_volume_get_fn s_volume_get = nullptr;
}

// ── RTSP 持久缓冲区（跨 rtsp_read_msg 调用保持数据）──
char *s_rtsp_buf = nullptr;
size_t s_rtsp_buf_used = 0;

// ── 媒体统计（供 keepalive 日志诊断）──
volatile uint32_t s_media_pkt_total = 0;

// ── RTSP 辅助任务句柄 ──
TaskHandle_t s_rtsp_task_handle = nullptr;

// ── 工具函数 ──

void hex_to_lower(const uint8_t *src, size_t len, char *dst)
{
    static const char kHex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        dst[i * 2] = kHex[(src[i] >> 4) & 0x0F];
        dst[i * 2 + 1] = kHex[src[i] & 0x0F];
    }
    dst[len * 2] = '\0';
}

// ── RTSP 读消息（持久缓冲 + interleaved RTP 跳过）──

void rtsp_read_msg_reset()
{
    if (s_rtsp_buf) {
        s_rtsp_buf_used = 0;
        s_rtsp_buf[0] = '\0';
    }
}

// 判断是否合法 RTSP 起始行。
int rtsp_is_message_start(const char *p, size_t n)
{
    static const char *const methods[] = {
        "OPTIONS ", "GET_PARAMETER ", "SET_PARAMETER ",
        "SETUP ",    "PLAY ",         "TEARDOWN ",
        "PAUSE ",    "TIME_OFFSET ",  "VIDEO_LATENCY ",
    };
    if (!p || n == 0) return 0;
    if (n >= 7 && memcmp(p, "RTSP/1.", 7) == 0) return 1;
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
        size_t m = strlen(methods[i]);
        if (n >= m && memcmp(p, methods[i], m) == 0) return 1;
    }
    return 0;
}

int rtsp_find_message_start(const char *buf, size_t n)
{
    if (!buf || n == 0) return -1;
    if (rtsp_is_message_start(buf, n)) return 0;
    for (size_t i = 0; i + 2 < n; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            rtsp_is_message_start(buf + i + 2, n - i - 2)) {
            return (int)(i + 2);
        }
    }
    return -1;
}

const char *rtsp_find_header_value(const char *headers, const char *name)
{
    size_t nlen = strlen(name);
    const char *p = headers;
    while (*p) {
        if (strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
            p += nlen + 1;
            while (*p == ' ') p++;
            return p;
        }
        const char *nl = strchr(p, '\n');
        if (!nl) break;
        p = nl + 1;
    }
    return nullptr;
}

// 完整读一条 RTSP 消息（headers + body），跳过 interleaved RTP。
// 返回总字节数，0=连接关闭，-1=错误。
int rtsp_read_msg(int sock, char *headers, size_t hdr_max,
                  char *body, size_t body_max, int *body_len)
{
    if (!s_rtsp_buf) {
        s_rtsp_buf = static_cast<char *>(
            heap_caps_malloc(kRtspBufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!s_rtsp_buf) return -1;
        s_rtsp_buf_used = 0;
    }
    headers[0] = '\0';
    body[0] = '\0';
    *body_len = 0;

    while (s_rtsp_buf_used < kRtspBufSize - 1) {
        s_rtsp_buf[s_rtsp_buf_used] = '\0';

        // 跳过 interleaved RTP 帧（$ + channel + len BE）。
        while (s_rtsp_buf_used >= 4 &&
               static_cast<uint8_t>(s_rtsp_buf[0]) == 0x24) {
            uint16_t rtp_len =
                (static_cast<uint16_t>(static_cast<uint8_t>(s_rtsp_buf[2])) << 8) |
                static_cast<uint8_t>(s_rtsp_buf[3]);
            if (s_rtsp_buf_used < static_cast<size_t>(4 + rtp_len)) break;
            ESP_LOGD(kTag, "[RTSP] skip interleaved len=%u", rtp_len);
            size_t skip = 4 + rtp_len;
            memmove(s_rtsp_buf, s_rtsp_buf + skip, s_rtsp_buf_used - skip);
            s_rtsp_buf_used -= skip;
            s_rtsp_buf[s_rtsp_buf_used] = '\0';
        }
        if (static_cast<uint8_t>(s_rtsp_buf[0]) == 0x24) {
            int n = recv(sock, s_rtsp_buf + s_rtsp_buf_used,
                         kRtspBufSize - 1 - s_rtsp_buf_used, 0);
            if (n <= 0) return n == 0 ? 0 : -1;
            s_rtsp_buf_used += n;
            continue;
        }

        // 跳过非 RTSP 起始的残留数据。
        if (s_rtsp_buf_used > 0 &&
            !rtsp_is_message_start(s_rtsp_buf, s_rtsp_buf_used)) {
            int next = rtsp_find_message_start(s_rtsp_buf, s_rtsp_buf_used);
            if (next > 0) {
                memmove(s_rtsp_buf, s_rtsp_buf + next,
                        s_rtsp_buf_used - static_cast<size_t>(next));
                s_rtsp_buf_used -= static_cast<size_t>(next);
                s_rtsp_buf[s_rtsp_buf_used] = '\0';
                continue;
            }
        }

        // 找 \r\n\r\n 分割 headers/body。
        char *hdr_end = strstr(s_rtsp_buf, "\r\n\r\n");
        if (!hdr_end) {
            int n = recv(sock, s_rtsp_buf + s_rtsp_buf_used,
                         kRtspBufSize - 1 - s_rtsp_buf_used, 0);
            if (n <= 0) {
                if (n == 0) ESP_LOGW(kTag, "[RTSP-recv] peer FIN (n=0)");
                else ESP_LOGW(kTag, "[RTSP-recv] ERR n=%d errno=%d", n, errno);
                return n == 0 ? 0 : -1;
            }
            s_rtsp_buf_used += n;
            continue;
        }

        size_t hdr_len = static_cast<size_t>(hdr_end - s_rtsp_buf) + 4;
        if (hdr_len >= hdr_max) {
            ESP_LOGW(kTag, "[RTSP] header too large (%u)", (unsigned)hdr_len);
            errno = EMSGSIZE;
            return -1;
        }
        memcpy(headers, s_rtsp_buf, hdr_len);
        headers[hdr_len] = '\0';

        // Content-Length。
        int content_len = 0;
        const char *cl = rtsp_find_header_value(headers, "Content-Length");
        if (cl) {
            content_len = atoi(cl);
        } else if (s_rtsp_buf_used > hdr_len) {
            size_t rest = s_rtsp_buf_used - hdr_len;
            if (!rtsp_is_message_start(s_rtsp_buf + hdr_len, rest)) {
                int next = rtsp_find_message_start(s_rtsp_buf + hdr_len, rest);
                content_len = (next > 0) ? next : (int)rest;
            }
        }
        if (content_len < 0 ||
            hdr_len + static_cast<size_t>(content_len) >= kRtspBufSize) {
            ESP_LOGW(kTag, "[RTSP] invalid Content-Length=%d", content_len);
            errno = EMSGSIZE;
            return -1;
        }

        // 等待完整 body。
        size_t body_have = s_rtsp_buf_used - hdr_len;
        while (static_cast<int>(body_have) < content_len &&
               s_rtsp_buf_used < kRtspBufSize - 1) {
            size_t need = static_cast<size_t>(content_len) - body_have;
            size_t space = kRtspBufSize - 1 - s_rtsp_buf_used;
            if (need > space) need = space;
            int n2 = recv(sock, s_rtsp_buf + s_rtsp_buf_used, need, 0);
            if (n2 <= 0) break;
            s_rtsp_buf_used += n2;
            s_rtsp_buf[s_rtsp_buf_used] = '\0';
            body_have = s_rtsp_buf_used - hdr_len;
        }

        size_t copy = static_cast<size_t>(content_len) < body_max - 1
                          ? static_cast<size_t>(content_len)
                          : body_max - 1;
        body_have = s_rtsp_buf_used - hdr_len;
        if (copy > body_have) copy = body_have;
        memcpy(body, s_rtsp_buf + hdr_len, copy);
        body[copy] = '\0';
        *body_len = static_cast<int>(copy);

        // 从持久缓冲移除已消费的 bytes。
        size_t consumed = hdr_len + static_cast<size_t>(content_len);
        if (consumed > s_rtsp_buf_used) consumed = s_rtsp_buf_used;
        memmove(s_rtsp_buf, s_rtsp_buf + consumed, s_rtsp_buf_used - consumed);
        s_rtsp_buf_used -= consumed;

        return static_cast<int>(hdr_len + copy);
    }
    return -1;
}

// ── RTSP 发送工具 ──

int rtsp_send_all(int sock, const void *data, size_t len)
{
    const uint8_t *buf = static_cast<const uint8_t *>(data);
    size_t sent = 0;
    while (sent < len) {
        int n = send(sock, buf + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return static_cast<int>(sent);
}

void rtsp_send_req(int sock, const char *method, const char *url,
                   const char *extra, int cseq)
{
    char req[384];
    int len = snprintf(req, sizeof(req),
                       "%s %s RTSP/1.0\r\n"
                       "User-Agent: stagefright/1.1 (Linux;Android 4.1)\r\n"
                       "CSeq: %d\r\n"
                       "%s"
                       "\r\n",
                       method, url, cseq, extra ? extra : "");
    send(sock, req, len, 0);
}

void rtsp_send_resp(int sock, int cseq, const char *body)
{
    char resp[640];
    int len;
    if (body) {
        len = snprintf(resp, sizeof(resp),
                       "RTSP/1.0 200 OK\r\n"
                       "User-Agent: stagefright/1.1 (Linux;Android 4.1)\r\n"
                       "CSeq: %d\r\n"
                       "Content-Length: %d\r\n"
                       "\r\n%s",
                       cseq, (int)strlen(body), body);
    } else {
        len = snprintf(resp, sizeof(resp),
                       "RTSP/1.0 200 OK\r\n"
                       "User-Agent: stagefright/1.1 (Linux;Android 4.1)\r\n"
                       "CSeq: %d\r\n"
                       "\r\n",
                       cseq);
    }
    send(sock, resp, len, 0);
}

int rtsp_get_cseq(const char *headers)
{
    const char *p = strstr(headers, "CSeq:");
    if (!p) p = strstr(headers, "cseq:");
    if (!p) return 0;
    return atoi(p + 5);
}

void rtsp_get_session(const char *headers, char *out, size_t max)
{
    out[0] = '\0';
    const char *p = strstr(headers, "Session:");
    if (!p) p = strstr(headers, "session:");
    if (!p) return;
    p += 8;
    while (*p == ' ') p++;
    size_t i = 0;
    while (*p && *p != '\r' && *p != ';' && i < max - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';
}

// ── HMAC-SHA256（复用 miplay_safety 的实现）──
// miplay_safety.h 已声明 miplay_hmac_sha256()，直接调用。

// ── TS 解密 ──

// PES 私有数据中提取 16 字节 AES IV。
bool pes_extract_iv(const uint8_t *pes, size_t pes_len, uint8_t iv[16])
{
    if (pes_len < 9) return false;
    uint8_t flags = pes[7];
    if (!(flags & 0x01)) return false;  // PES_extension_flag 未设置
    size_t cursor = 9 + pes[8];
    if (cursor + 17 > pes_len) return false;
    if (!(pes[cursor] & 0x80)) return false;  // PES_private_data_flag
    memcpy(iv, pes + cursor + 1, 16);
    return true;
}

// 就地解密 TS 流中 PID=0x1100 的 PES 音频前 256 字节。
void decrypt_ts_media(uint8_t *ts_buf, size_t ts_len)
{
    struct PesDecState {
        uint8_t iv[16];
        int remaining;
        int pes_pay_off;
    };
    PesDecState st = {};
    for (size_t off = 0; off + kTsPktSize <= ts_len; off += kTsPktSize) {
        uint8_t *pkt = ts_buf + off;
        if (pkt[0] != kTsSyncByte) continue;
        uint16_t pid = (static_cast<uint16_t>(pkt[1] & 0x1F) << 8) | pkt[2];
        if (pid != kTargetPid) continue;
        uint8_t afc = (pkt[3] >> 4) & 0x03;
        if (afc == 0 || afc == 2) continue;
        bool pusi = (pkt[1] & 0x40) != 0;
        int pay_off = 4;
        if (afc == 3) {
            int alen = pkt[4];
            pay_off = 5 + alen;
        }
        int pay_len = kTsPktSize - pay_off;
        if (pay_len <= 0) continue;
        if (pusi) {
            uint8_t *pes = pkt + pay_off;
            if (pay_len < 9 || pes[0] != 0 || pes[1] != 0 || pes[2] != 1 ||
                pes[3] != 0xC0) {
                st.remaining = 0;
                continue;
            }
            uint16_t pes_len = (static_cast<uint16_t>(pes[4]) << 8) | pes[5];
            int hdr_len = 9 + pes[8];
            if (hdr_len > pay_len) hdr_len = pay_len;
            if (pes_extract_iv(pes, pay_len, st.iv)) {
                st.remaining = (pes_len > 0) ? (pes_len - hdr_len + 6)
                                             : kEncryptPrefix;
                if (st.remaining > kEncryptPrefix)
                    st.remaining = kEncryptPrefix;
            } else {
                st.remaining = 0;
            }
            st.pes_pay_off = hdr_len;
        }
        if (st.remaining > 0) {
            int start = pusi ? st.pes_pay_off : 0;
            int avail = pay_len - start;
            if (avail <= 0) {
                st.remaining = 0;
                continue;
            }
            int n = (avail < st.remaining) ? avail : st.remaining;
            if (n % kAesBlockLen != 0) n -= n % kAesBlockLen;
            if (n > 0) {
                mbedtls_aes_context actx;
                mbedtls_aes_init(&actx);
                mbedtls_aes_setkey_dec(&actx, s_stream_key, 128);
                mbedtls_aes_crypt_cbc(&actx, MBEDTLS_AES_DECRYPT, n, st.iv,
                                      pkt + pay_off + start,
                                      pkt + pay_off + start);
                mbedtls_aes_free(&actx);
                st.remaining -= n;
            }
            if (st.remaining <= 0) st.remaining = 0;
        }
    }
}

// ── AAC 解码器包装 ──
// MiPlay 的 TS 流包含 ADTS 封装的 AAC-LC 音频。
// 解码流程：TS demux → PES 解密 → ADTS 帧提取 → AAC 解码 → PCM 输出。

struct AacDecoderCtx {
    void *handle = nullptr;
    bool opened = false;
    int sample_rate = 48000;
    int channels = 2;
    // PCM 输出缓冲区（decoder 生命周期内复用，避免每帧 malloc/free 破坏 PSRAM 堆）
    static constexpr size_t kPcmBufSize = 2048 * 2 * 2;  // AAC-LC max: 1024 samples * 2ch * 2bytes
    uint8_t *pcm_buf = nullptr;

    bool open()
    {
        if (opened) return true;
        pcm_buf = static_cast<uint8_t *>(
            heap_caps_malloc(kPcmBufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!pcm_buf) {
            ESP_LOGE(kTag, "PCM buf alloc failed");
            return false;
        }
        esp_aac_dec_cfg_t cfg = {};
        cfg.sample_rate = ESP_AUDIO_SAMPLE_RATE_48K;
        cfg.channel = ESP_AUDIO_DUAL;
        cfg.bits_per_sample = ESP_AUDIO_BIT16;
        cfg.no_adts_header = false;  // ADTS 帧带头
        cfg.aac_plus_enable = false;
        esp_audio_err_t err = esp_aac_dec_open(&cfg, sizeof(cfg), &handle);
        if (err != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(kTag, "AAC open failed: %d", err);
            heap_caps_free(pcm_buf);
            pcm_buf = nullptr;
            return false;
        }
        opened = true;
        return true;
    }

    // 解码一个 ADTS 帧，输出 PCM 到 audio_services。
    // 返回解码的输入字节数，0=跳过，-1=错误。
    int decode_and_play(const uint8_t *adts, size_t adts_len)
    {
        if (!opened || !adts || adts_len < 7) return -1;

        // ADTS 帧长度。
        uint16_t frame_len =
            ((uint16_t)(adts[3] & 0x03) << 11) |
            ((uint16_t)adts[4] << 3) |
            ((uint16_t)(adts[5] >> 5));
        if (frame_len < 7 || frame_len > adts_len) return 0;

        // 解码。
        esp_audio_dec_in_raw_t raw = {};
        raw.buffer = const_cast<uint8_t *>(adts);
        raw.len = frame_len;

        esp_audio_dec_out_frame_t frame = {};
        frame.buffer = pcm_buf;
        frame.len = kPcmBufSize;

        esp_audio_dec_info_t info = {};
        esp_audio_err_t err =
            esp_aac_dec_decode(handle, &raw, &frame, &info);

        // 一次性诊断：首次解码时打印结果。
        static bool s_diag_logged = false;
        if (!s_diag_logged) {
            s_diag_logged = true;
            ESP_LOGW(kTag, "[DIAG] decode_and_play: err=%d decoded=%u "
                           "info_rate=%d info_ch=%d",
                     (int)err, (unsigned)frame.decoded_size,
                     (int)info.sample_rate, (int)info.channel);
        }

        if (err == ESP_AUDIO_ERR_OK && frame.decoded_size > 0) {
            // 更新采样率（首帧可能改变）。
            if (info.sample_rate > 0 && info.sample_rate != sample_rate) {
                sample_rate = info.sample_rate;
                channels = info.channel;
                ESP_LOGI(kTag, "AAC: rate=%d ch=%d", sample_rate, channels);
            }
            // 直接输出立体声 PCM（frame_count = 样本对数）。
            int16_t *pcm = reinterpret_cast<int16_t *>(pcm_buf);
            size_t total_samples = frame.decoded_size / sizeof(int16_t);
            size_t frame_count = (channels >= 2) ? total_samples / channels
                                                 : total_samples;
            if (s_write_speaker) {
                s_write_speaker(pcm, frame_count, sample_rate);
            }
        }
        return static_cast<int>(frame_len);
    }

    void close()
    {
        if (opened && handle) {
            esp_aac_dec_close(handle);
            handle = nullptr;
            opened = false;
        }
        if (pcm_buf) {
            heap_caps_free(pcm_buf);
            pcm_buf = nullptr;
        }
    }
};

// ── 媒体接收任务参数 ──
struct MediaTaskArg {
    int media_sock;
    int rtsp_sock;
    uint32_t generation;
};

// ── 媒体接收任务 ──
// 读 interleaved RTP → 剥 RTP 头 → TS 解密 → AAC 解码 → audio_services。
void media_receive_task(void *arg)
{
    MediaTaskArg *marg = static_cast<MediaTaskArg *>(arg);
    int media_sock = marg->media_sock;
    int rtsp_sock_to_close = marg->rtsp_sock;
    uint32_t generation = marg->generation;
    free(marg);

    ESP_LOGI(kTag, "[MEDIA] Task started (sock=%d gen=%lu)",
             media_sock, (unsigned long)generation);

    // 获取音频 codec（立体声）。
    if (s_stream_start && !s_stream_start()) {
        ESP_LOGW(kTag, "[MEDIA] audio session acquire failed");
    }

    // AAC 解码器。
    AacDecoderCtx aac;
    if (!aac.open()) {
        ESP_LOGE(kTag, "[MEDIA] AAC decoder open failed");
        if (media_sock != rtsp_sock_to_close) close(media_sock);
        vTaskDeleteWithCaps(nullptr);
        return;
    }

    uint8_t *rtp_buf = static_cast<uint8_t *>(
        heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!rtp_buf) {
        ESP_LOGE(kTag, "[MEDIA] alloc failed");
        if (media_sock != rtsp_sock_to_close) close(media_sock);
        aac.close();
        vTaskDeleteWithCaps(nullptr);
        return;
    }

    // SO_RCVBUF + SO_RCVTIMEO。
    int rcvbuf = 32 * 1024;
    setsockopt(media_sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};  // 100ms
    setsockopt(media_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint32_t pkt_count = 0, rtp_total = 0, ts_total = 0;
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t last_log = start_tick;
    uint32_t idle_ticks = 0;

    // ADTS 帧跨 TS 包边界时的缓冲（8KB 足够容纳多个 AAC-LC 帧）。
    constexpr size_t kAdtsBufSize = 8192;
    uint8_t *adts_buf = static_cast<uint8_t *>(
        heap_caps_malloc(kAdtsBufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    size_t adts_buf_used = 0;
    if (!adts_buf) {
        ESP_LOGE(kTag, "[MEDIA] adts_buf alloc failed");
        free(rtp_buf);
        if (media_sock != rtsp_sock_to_close) close(media_sock);
        aac.close();
        vTaskDeleteWithCaps(nullptr);
        return;
    }

    while (s_media_generation == generation) {
        // 每秒日志。
        if (xTaskGetTickCount() - last_log >= pdMS_TO_TICKS(1000)) {
            ESP_LOGI(kTag,
                     "[MEDIA] alive %lus pkts=%lu rtp=%luB idle=%lu",
                     (unsigned long)((xTaskGetTickCount() - start_tick) /
                                     configTICK_RATE_HZ),
                     (unsigned long)pkt_count, (unsigned long)rtp_total,
                     (unsigned long)idle_ticks);
            last_log = xTaskGetTickCount();
        }

        // 读 4 字节 interleaved 帧头：$ + channel + len(u16 BE)。
        uint8_t hdr[4];
        int got = 0;
        while (got < 4) {
            int n = recv(media_sock, hdr + got, 4 - got, 0);
            if (n <= 0) {
                if (n == 0) {
                    ESP_LOGW(kTag, "[MEDIA] sock closed (n=0) pkts=%lu",
                             (unsigned long)pkt_count);
                    goto m_cleanup;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (s_media_generation != generation) goto m_cleanup;
                    idle_ticks++;
                    continue;
                }
                ESP_LOGW(kTag, "[MEDIA] recv err errno=%d pkts=%lu", errno,
                         (unsigned long)pkt_count);
                goto m_cleanup;
            }
            got += n;
        }
        if (s_media_generation != generation) break;
        if (hdr[0] != 0x24) {
            ESP_LOGW(kTag, "[MEDIA] bad marker 0x%02X", hdr[0]);
            memmove(hdr, hdr + 1, 3);
            int n = recv(media_sock, hdr + 3, 1, 0);
            if (n <= 0) goto m_cleanup;
            if (hdr[0] != 0x24) continue;
        }

        uint16_t rtp_len = (static_cast<uint16_t>(hdr[2]) << 8) | hdr[3];
        if (rtp_len < 12 || rtp_len > 4096) {
            ESP_LOGW(kTag, "[MEDIA] bad RTP len: %u", rtp_len);
            continue;
        }

        got = 0;
        while (got < rtp_len) {
            int n = recv(media_sock, rtp_buf + got, rtp_len - got, 0);
            if (n > 0) {
                got += n;
                continue;
            }
            if (n == 0) {
                ESP_LOGW(kTag, "[MEDIA] body FIN pkts=%lu",
                         (unsigned long)pkt_count);
                goto m_cleanup;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (s_media_generation != generation) goto m_cleanup;
                idle_ticks++;
                continue;
            }
            ESP_LOGW(kTag, "[MEDIA] body recv err errno=%d", errno);
            goto m_cleanup;
        }
        rtp_total += rtp_len;
        pkt_count++;
        s_media_pkt_total = pkt_count;
        if (pkt_count <= 3) {
            ESP_LOGI(kTag,
                     "[MEDIA] RTP#%lu len=%u first=[%02X %02X %02X %02X]",
                     (unsigned long)pkt_count, rtp_len, rtp_buf[0], rtp_buf[1],
                     rtp_buf[2], rtp_buf[3]);
        }

        // 剥 RTP 头，提取 TS 载荷。
        uint8_t *ts_data = rtp_buf;
        uint32_t ts_len = rtp_len;
        if ((rtp_buf[0] & 0xC0) == 0x80) {
            int hdr_len = 12 + (rtp_buf[0] & 0x0F) * 4;
            if (rtp_buf[0] & 0x10) {
                if (hdr_len + 4 <= static_cast<int>(rtp_len)) {
                    int ext_words =
                        (static_cast<uint16_t>(rtp_buf[hdr_len + 2]) << 8) |
                        rtp_buf[hdr_len + 3];
                    hdr_len += 4 + ext_words * 4;
                }
            }
            int end = rtp_len;
            if (rtp_buf[0] & 0x20) {
                end -= rtp_buf[rtp_len - 1];
            }
            if (hdr_len < end) {
                ts_data = rtp_buf + hdr_len;
                ts_len = end - hdr_len;
            }
        }

        // PES 解密（streamKey 已交换时）。
        if (s_has_stream_key) {
            decrypt_ts_media(ts_data, ts_len);
        }

        // TS demux + AAC 解码。
        // 策略：扫描 TS 包，提取 PID=0x1100 的 PES 音频数据，
        // 追加到 adts_buf，扫描完整 ADTS 帧解码。
        // adts_buf 在循环外分配，跨 TS 包保持未消费数据。
        for (size_t off = 0; off + kTsPktSize <= ts_len; off += kTsPktSize) {
            uint8_t *pkt = ts_data + off;
            if (pkt[0] != kTsSyncByte) continue;
            uint16_t pid =
                (static_cast<uint16_t>(pkt[1] & 0x1F) << 8) | pkt[2];
            if (pid != kTargetPid) continue;
            uint8_t afc = (pkt[3] >> 4) & 0x03;
            if (afc == 0 || afc == 2) continue;
            bool pusi = (pkt[1] & 0x40) != 0;
            int pay_off = 4;
            if (afc == 3) {
                pay_off = 5 + pkt[4];
            }
            int pay_len = kTsPktSize - pay_off;
            if (pay_len <= 0) continue;

            // 提取 PES 音频数据。
            const uint8_t *audio_data = pkt + pay_off;
            int audio_len = pay_len;
            if (pusi && pay_len >= 9 && pkt[pay_off] == 0 &&
                pkt[pay_off + 1] == 0 && pkt[pay_off + 2] == 1 &&
                pkt[pay_off + 3] == 0xC0) {
                // 新 PES 包：跳过 PES 头。
                int hdr_len = 9 + pkt[pay_off + 8];
                if (hdr_len >= pay_len) {
                    adts_buf_used = 0;  // 整个 TS 包都是 PES 头，重置
                    continue;
                }
                audio_data = pkt + pay_off + hdr_len;
                audio_len = pay_len - hdr_len;
                // 新 PES 开始时不清空 adts_buf（可能有上一帧尾部）。
            }

            // 追加到 ADTS 缓冲。
            if (audio_len > 0 &&
                adts_buf_used + audio_len <= kAdtsBufSize) {
                memcpy(adts_buf + adts_buf_used, audio_data, audio_len);
                adts_buf_used += audio_len;
            }

            // 扫描并解码完整 ADTS 帧。
            int scan_off = 0;
            while (scan_off + 7 <= adts_buf_used) {
                // 找 ADTS 同步字。
                if (adts_buf[scan_off] != 0xFF ||
                    (adts_buf[scan_off + 1] & 0xF6) != 0xF0) {
                    scan_off++;
                    continue;
                }
                uint16_t frame_len =
                    (static_cast<uint16_t>(adts_buf[scan_off + 3] & 0x03) << 11) |
                    (static_cast<uint16_t>(adts_buf[scan_off + 4]) << 3) |
                    (static_cast<uint16_t>(adts_buf[scan_off + 5] >> 5));
                if (frame_len < 7) { scan_off++; continue; }
                if (scan_off + frame_len > adts_buf_used) break;  // 不完整
                aac.decode_and_play(adts_buf + scan_off, frame_len);
                scan_off += frame_len;
            }
            // 移除已消费数据。
            if (scan_off > 0 && scan_off < adts_buf_used) {
                memmove(adts_buf, adts_buf + scan_off,
                        adts_buf_used - scan_off);
                adts_buf_used -= scan_off;
            } else if (scan_off >= adts_buf_used) {
                adts_buf_used = 0;
            }
        }

        ts_total += ts_len;
        if (pkt_count % 100 == 1) {
            ESP_LOGI(kTag, "[MEDIA] pkts=%lu rtp=%luKB ts=%luKB",
                     (unsigned long)pkt_count,
                     (unsigned long)(rtp_total / 1024),
                     (unsigned long)(ts_total / 1024));
        }
    }

m_cleanup:
    if (s_media_generation != generation) {
        ESP_LOGI(kTag, "[MEDIA] Stale session cleanup, skip stop callback");
    }
    aac.close();
    if (s_stream_stop) {
        s_stream_stop();
    }
    free(adts_buf);
    free(rtp_buf);
    close(media_sock);
    // 避免双重关闭同一个 fd。
    if (rtsp_sock_to_close >= 0 && rtsp_sock_to_close != media_sock) {
        ESP_LOGW(kTag, "[MEDIA] closing rtsp_sock=%d", rtsp_sock_to_close);
        close(rtsp_sock_to_close);
    }
    ESP_LOGI(kTag, "[MEDIA] Task ended, %lu pkts, %luKB rtp, %luKB ts",
             (unsigned long)pkt_count, (unsigned long)(rtp_total / 1024),
             (unsigned long)(ts_total / 1024));
    vTaskDeleteWithCaps(nullptr);
}

// ── Image drain 任务 ──
// 手机在数据 socket 上发的 image 数据（缩略图/封面），RLCD 不需要，直接丢弃。
//
// ★ socket/任务泄漏修复（2026-09-17）。
//
// 起因：投屏第二次起必失败，socket() 报 errno=23(ENFILE)，随后
//   multi_sock=-1 → 回落用 rtsp_sock 收 RTP → errno=109(ENOTCONN) → 秒退。
// 判据（串口实测）：
//   [RTSP] data socket() failed: image=63 multi=-1 errno=23
//     internal_free=15883 internal_largest=7680   ← 内存还剩 15KB，不是 OOM
//   CONFIG_LWIP_MAX_SOCKETS=10                    ← 全局仅 10 个 socket
//
// 根因：本任务原先只靠 recv()<=0 退出。会话结束（rtsp_cleanup 关的是
//   rtsp_sock/multi_sock）时，image_sock 这条 TCP 连接手机侧可能仍开着，
//   recv() 会【永久阻塞】→ 永远走不到 close(sock) → 每投屏一次泄漏
//   1 个 socket fd + 1 个任务（12KB PSRAM 栈 + 4KB buf）。
//   投屏几次即打满 10 个 socket → 后续全部 ENFILE。
//   连带：miplay_create_task 的 s_task_caps 表仅 8 槽（kMaxTrackedTasks），
//   被泄漏任务占满后新任务登记不上 → 删除时走错 API → PSRAM 栈也不再释放。
//
// 修法：与 miplay_rtsp_run 同一套【换代守卫】范式（见本文件 while
//   (s_media_generation == generation)）：recv 加 SO_RCVTIMEO，超时后
//   比对 s_media_generation，不匹配即认定本会话已结束 → 退出并 close。
//   不用 poll：本任务无需区分可读事件类型，超时轮询足够且改动最小。
namespace {
struct ImageDrainArg {
    int sock;
    uint32_t generation;
};
}  // namespace

void image_drain_task(void *arg)
{
    ImageDrainArg *ia = static_cast<ImageDrainArg *>(arg);
    const int sock = ia->sock;
    const uint32_t generation = ia->generation;
    heap_caps_free(ia);

    uint8_t *buf = static_cast<uint8_t *>(
        heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf) {
        close(sock);
        miplay_delete_current_task();
        return;
    }

    // 1 秒接收超时：既避免永久阻塞，又不影响正常封面数据的接收
    // （封面数据到达是突发性的，1 秒内无数据即视为空闲）。
    struct timeval rtv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));

    while (s_media_generation == generation) {
        int n = recv(sock, buf, 4096, 0);
        if (n > 0) {
            continue;  // 收到封面数据，继续丢
        }
        if (n == 0) {
            break;  // 对端正常关闭
        }
        // n < 0：区分超时与真错误。超时（EAGAIN/EWOULDBLOCK）回到循环
        // 顶部复查 generation；真错误（ECONNRESET 等）退出。
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            continue;
        }
        break;
    }

    heap_caps_free(buf);
    close(sock);
    ESP_LOGI(kTag, "[img_drain] exit gen=%lu (socket released)",
             (unsigned long)generation);
    miplay_delete_current_task();
}

// ── RTSP 客户端 ──
// TCP 连接 → OPTIONS 握手 → rtp_ports → GET_PARAMETER → SETUP → PLAY
// → 媒体任务 + keepalive 循环。
void miplay_rtsp_run(const char *host, int port, int client_sock,
                     uint32_t generation, const miplay_session_t *session)
{
    char *headers = nullptr;
    char *body = nullptr;
    int rtsp_sock = -1;
    char rtsp_host[64];
    int rtsp_port = 0;

    rtsp_read_msg_reset();

    char session_id[32] = {0};
    char pres_url[160] = "rtsp://localhost/wfd1.0/streamid=0";
    int cseq = 1;
    int sent_options = 0;
    int rtsp_state = 0;  // 0=handshake, 1=setup_sent, 2=playing
    int cseq_of_options = 0;
    int cseq_of_getparam = 0;
    int cseq_of_setup = 0;
    int cseq_of_play = 0;
    // 数据 socket：参考实现要求手机发 GET_PARAMETER 响应前建好，否则手机不发 RTP。
    int image_sock = -1;
    int multi_sock = -1;
    uint16_t image_port = 0;
    uint16_t multi_port = 0;
    struct timeval tv_recv = {.tv_sec = 2};

    // RTSP 连接重试。
    for (int attempt = 0; attempt < kRtspRetryCount; attempt++) {
        if (attempt > 0) {
            ESP_LOGW(kTag, "[RTSP] Retry attempt %d...", attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(kRtspRetryBackoffMs));
        }
        ESP_LOGI(kTag, "[RTSP] Connecting to %s:%d ...", host, port);

        rtsp_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (rtsp_sock < 0) {
            ESP_LOGE(kTag, "[RTSP] socket failed");
            goto rtsp_cleanup;
        }
        int flag = 1;
        setsockopt(rtsp_sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        struct timeval tv2 = {.tv_sec = 2};
        setsockopt(rtsp_sock, SOL_SOCKET, SO_RCVTIMEO, &tv2, sizeof(tv2));

        struct sockaddr_in dest = {};
        dest.sin_family = AF_INET;
        dest.sin_port = htons(port);
        dest.sin_addr.s_addr = inet_addr(host);
        if (connect(rtsp_sock, reinterpret_cast<struct sockaddr *>(&dest),
                    sizeof(dest)) < 0) {
            ESP_LOGW(kTag, "[RTSP] connect failed: %d", errno);
            close(rtsp_sock);
            rtsp_sock = -1;
            continue;
        }
        ESP_LOGI(kTag, "[RTSP] Connected!");
        rtsp_read_msg_reset();

        // 试读第一条消息，如果立刻失败则重连。
        char test_hdr[16];
        int tn = recv(rtsp_sock, test_hdr, 1, MSG_PEEK);
        if (tn <= 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGW(kTag, "[RTSP] First peek failed: n=%d errno=%d", tn, errno);
            close(rtsp_sock);
            rtsp_sock = -1;
            continue;
        }
        break;
    }
    if (rtsp_sock < 0) {
        ESP_LOGE(kTag, "[RTSP] All connection attempts failed");
        goto rtsp_cleanup;
    }

    strncpy(rtsp_host, host, sizeof(rtsp_host) - 1);
    rtsp_host[sizeof(rtsp_host) - 1] = '\0';
    rtsp_port = port;

    headers = static_cast<char *>(
        heap_caps_malloc(kRtspBufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    body = static_cast<char *>(
        heap_caps_malloc(kRtspBufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!headers || !body) {
        heap_caps_free(headers);
        heap_caps_free(body);
        close(rtsp_sock);
        return;
    }
    int body_len;

    setsockopt(rtsp_sock, SOL_SOCKET, SO_RCVTIMEO, &tv_recv, sizeof(tv_recv));
    rtsp_read_msg_reset();

    while (s_media_generation == generation && rtsp_state < 2) {
        ESP_LOGI(kTag, "[RTSP] Waiting for msg (state=%d)...", rtsp_state);
        int ret = rtsp_read_msg(rtsp_sock, headers, kRtspBufSize,
                                body, kRtspBufSize, &body_len);
        if (ret <= 0) {
            if (ret == 0) ESP_LOGI(kTag, "[RTSP] Disconnected");
            else ESP_LOGW(kTag, "[RTSP] Read error (errno=%d)", errno);
            break;
        }
        ESP_LOGI(kTag, "[RTSP] Got msg (%d bytes)", ret);

        int peer_cseq = rtsp_get_cseq(headers);
        int is_resp = (strncmp(headers, "RTSP/1.0 ", 9) == 0);

        if (is_resp) {
            ESP_LOGI(kTag, "[RTSP] <- resp cseq=%d: %.120s",
                     peer_cseq, headers);
            if (peer_cseq == cseq_of_options && strstr(headers, "200")) {
                ESP_LOGI(kTag, "[RTSP] <- OPTIONS resp 200");
            } else if (peer_cseq == cseq_of_getparam &&
                       strstr(headers, "200")) {
                ESP_LOGI(kTag, "[RTSP] <- GET_PARAMETER resp (body=%d bytes)",
                         body_len);
            } else if (peer_cseq == cseq_of_setup && strstr(headers, "200")) {
                rtsp_get_session(headers, session_id, sizeof(session_id));
                ESP_LOGI(kTag, "[RTSP] <- SETUP resp 200, session=%s",
                         session_id);
                char play_extra[64];
                snprintf(play_extra, sizeof(play_extra), "Session: %s\r\n",
                         session_id);
                cseq_of_setup = 0;
                rtsp_send_req(rtsp_sock, "PLAY", pres_url, play_extra, cseq);
                ESP_LOGI(kTag, "[RTSP] -> PLAY (session=%s, cseq=%d)",
                         session_id, cseq);
                cseq_of_play = cseq;
                cseq++;
            } else if (peer_cseq == cseq_of_play && strstr(headers, "200")) {
                ESP_LOGI(kTag,
                         "[RTSP] <- PLAY resp 200 === STREAM STARTED ===");
                rtsp_state = 2;
            } else if (strstr(headers, "200")) {
                ESP_LOGI(kTag, "[RTSP] <- resp 200 (cseq=%d, ignored)",
                         peer_cseq);
            }
            continue;
        }

        // 处理手机发来的 RTSP 请求。
        char method[32] = {0};
        sscanf(headers, "%31s", method);
        ESP_LOGI(kTag, "[RTSP] <- %s (cseq=%d)", method, peer_cseq);

        if (strcmp(method, "OPTIONS") == 0) {
            ESP_LOGI(kTag, "[RTSP] OPTIONS headers(%d): %.200s",
                     (int)strlen(headers), headers);
            // OPTIONS 响应：有 auth 时带 HMAC。
            {
                char auth_ack_hex[65] = {0};
                char *phone_auth = strstr(headers, "authMsg:");
                if (phone_auth && session && session->has_session_key) {
                    phone_auth += 8;
                    while (*phone_auth == ' ' || *phone_auth == '\t' ||
                           *phone_auth == '\r' || *phone_auth == '\n')
                        phone_auth++;
                    int chal_len = strlen(phone_auth);
                    while (chal_len > 0 &&
                           (phone_auth[chal_len - 1] == '\r' ||
                            phone_auth[chal_len - 1] == '\n' ||
                            phone_auth[chal_len - 1] == ' '))
                        chal_len--;
                    uint8_t hash[32];
                    miplay_hmac_sha256(
                        reinterpret_cast<const uint8_t *>(session->auth_key),
                        32, reinterpret_cast<const uint8_t *>(phone_auth),
                        chal_len, hash);
                    hex_to_lower(hash, 32, auth_ack_hex);
                    ESP_LOGI(kTag, "[RTSP] authMsg ack=%.16s...",
                             auth_ack_hex);
                }
                char resp[512];
                int rlen;
                if (auth_ack_hex[0]) {
                    rlen = snprintf(
                        resp, sizeof(resp),
                        "RTSP/1.0 200 OK\r\n"
                        "User-Agent: stagefright/1.1 (Linux;Android 4.1)\r\n"
                        "CSeq: %d\r\n"
                        "Public: org.wfa.wfd1.0, GET_PARAMETER, "
                        "SET_PARAMETER\r\n"
                        "authKeyType:2\r\n"
                        "authAlgorithmVal:4\r\n"
                        "authMsgAck:%s\r\n"
                        "\r\n",
                        peer_cseq, auth_ack_hex);
                } else {
                    rlen = snprintf(
                        resp, sizeof(resp),
                        "RTSP/1.0 200 OK\r\n"
                        "User-Agent: stagefright/1.1 (Linux;Android 4.1)\r\n"
                        "CSeq: %d\r\n"
                        "Public: org.wfa.wfd1.0, GET_PARAMETER, "
                        "SET_PARAMETER\r\n"
                        "\r\n",
                        peer_cseq);
                }
                send(rtsp_sock, resp, rlen, 0);
            }
            ESP_LOGI(kTag, "[RTSP] -> OPTIONS resp (cseq=%d)", peer_cseq);
            // 发我们自己的 OPTIONS（带随机 challenge）。
            if (!sent_options) {
                char rtsp_chal[33] = {0};
                uint8_t rb[16];
                for (int i = 0; i < 16; i += 4) {
                    uint32_t r = esp_random();
                    memcpy(rb + i, &r, 4);
                }
                hex_to_lower(rb, 16, rtsp_chal);
                char extra[128];
                snprintf(extra, sizeof(extra),
                         "Require: org.wfa.wfd1.0\r\n"
                         "lib_version: audio-display-release2.1 "
                         "2.1.5071614\r\n"
                         "authMsg:%s\r\n",
                         rtsp_chal);
                rtsp_send_req(rtsp_sock, "OPTIONS", "*", extra, cseq);
                ESP_LOGI(kTag, "[RTSP] -> OPTIONS req (cseq=%d)", cseq);
                cseq_of_options = cseq;
                cseq++;
                sent_options = 1;
            }
        } else if (strcmp(method, "rtp_ports") == 0) {
            if (peer_cseq == cseq_of_getparam) {
                ESP_LOGI(kTag, "[RTSP] rtp_ports is resp to GET_PARAMETER");
                cseq_of_getparam = 0;
            } else {
                rtsp_send_resp(rtsp_sock, peer_cseq, kWfdCapabilities);
                ESP_LOGI(kTag, "[RTSP] -> rtp_ports resp (caps, cseq=%d)",
                         peer_cseq);
            }
        } else if (strcmp(method, "GET_PARAMETER") == 0) {
            ESP_LOGI(kTag, "[RTSP] GET_PARAMETER body[%d]: %.80s", body_len,
                     body);
            // 参考实现：手机期望数据连接在收到 GET_PARAMETER 响应前就建立。
            // 先建两个 TCP 数据 socket 连回手机 RTSP 端口，再发能力声明。
            if (image_sock < 0 && rtsp_port > 0) {
                image_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
                multi_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
                // ★ 诊断（2026-09-17）：此前失败分支【无任何日志】，导致
                //   "Data sockets: image=.. multi=.." 那行没出现时无法区分
                //   「socket() 失败」与「connect() 失败」。
                //   实测 12 次投屏全部 media_sock==rtsp_sock 且 errno=109(ENOTCONN)，
                //   怀疑 socket() 因内部 RAM 耗尽返回 -1，故在此暴露真实原因。
                //   注意 image_sock/multi_sock 失败时停在 -1，下一轮 GET_PARAMETER
                //   会重试，故这里只打日志不改变控制流。
                if (image_sock < 0 || multi_sock < 0) {
                    ESP_LOGW(kTag,
                             "[RTSP] data socket() failed: image=%d multi=%d "
                             "errno=%d internal_free=%u internal_largest=%u",
                             image_sock, multi_sock, errno,
                             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                             (unsigned)heap_caps_get_largest_free_block(
                                 MALLOC_CAP_INTERNAL));
                }
                if (image_sock >= 0 && multi_sock >= 0) {
                    struct timeval ctv = {.tv_sec = 3};
                    setsockopt(image_sock, SOL_SOCKET, SO_SNDTIMEO,
                               &ctv, sizeof(ctv));
                    setsockopt(multi_sock, SOL_SOCKET, SO_SNDTIMEO,
                               &ctv, sizeof(ctv));
                    struct sockaddr_in daddr = {};
                    daddr.sin_family = AF_INET;
                    daddr.sin_port = htons(rtsp_port);
                    daddr.sin_addr.s_addr = inet_addr(rtsp_host);
                    if (connect(image_sock,
                                reinterpret_cast<struct sockaddr *>(&daddr),
                                sizeof(daddr)) == 0 &&
                        connect(multi_sock,
                                reinterpret_cast<struct sockaddr *>(&daddr),
                                sizeof(daddr)) == 0) {
                        struct sockaddr_in local_addr;
                        socklen_t addrlen = sizeof(local_addr);
                        getsockname(image_sock,
                                    reinterpret_cast<struct sockaddr *>(
                                        &local_addr),
                                    &addrlen);
                        image_port = ntohs(local_addr.sin_port);
                        getsockname(multi_sock,
                                    reinterpret_cast<struct sockaddr *>(
                                        &local_addr),
                                    &addrlen);
                        multi_port = ntohs(local_addr.sin_port);
                        ESP_LOGI(kTag,
                                 "[RTSP] Data sockets: image=%u multi=%u",
                                 (unsigned)image_port,
                                 (unsigned)multi_port);
                        // ★ 传 generation 给 drain 任务（2026-09-17，修 socket 泄漏）：
                        //   任务据此在会话换代后主动退出，避免永久阻塞在 recv()
                        //   而泄漏 socket 与任务。参数结构用 PSRAM 分配，失败则
                        //   退化为"直接关闭 image_sock"（放弃丢包，不泄漏）。
                        ImageDrainArg *ia = static_cast<ImageDrainArg *>(
                            heap_caps_malloc(sizeof(ImageDrainArg),
                                             MALLOC_CAP_SPIRAM |
                                                 MALLOC_CAP_8BIT));
                        if (ia) {
                            ia->sock = image_sock;
                            ia->generation = generation;
                            if (miplay_create_task(image_drain_task, "img_drain",
                                                   12 * 1024, ia, 3,
                                                   nullptr, 1) != pdPASS) {
                                heap_caps_free(ia);
                                close(image_sock);
                            }
                        } else {
                            close(image_sock);
                        }
                        image_sock = -1;  // 所有权已移交（任务或就地释放）
                    } else {
                        ESP_LOGW(kTag,
                                 "[RTSP] data socket connect failed "
                                 "(using interleaved)");
                        close(image_sock);
                        close(multi_sock);
                        image_sock = multi_sock = -1;
                    }
                }
            }
            rtsp_send_resp(rtsp_sock, peer_cseq, kWfdCapabilities);
            ESP_LOGI(kTag, "[RTSP] -> GET_PARAMETER resp (caps)");
        } else if (strcmp(method, "SET_PARAMETER") == 0) {
            // 解析 presentation_URL。
            char *purl = strstr(body, "wfd_presentation_URL:");
            if (purl) {
                purl += 21;
                while (*purl == ' ') purl++;
                int i = 0;
                while (purl[i] && purl[i] != ' ' && purl[i] != '\r' &&
                       purl[i] != '\n' && i < static_cast<int>(sizeof(pres_url)) - 1) {
                    pres_url[i] = purl[i];
                    i++;
                }
                pres_url[i] = '\0';
                ESP_LOGI(kTag, "[RTSP] presentation_URL: %s", pres_url);
            }
            rtsp_send_resp(rtsp_sock, peer_cseq, nullptr);

            // SETUP trigger。
            if (strstr(body, "wfd_trigger_method: SETUP")) {
                ESP_LOGI(kTag, "[RTSP] Got SETUP trigger");
                char extra[160];
                if (image_port > 0 && multi_port > 0) {
                    snprintf(extra, sizeof(extra),
                             "Transport: RTP/AVP/TCP;interleaved=0-1\r\n"
                             "MultiPort: image_port=%u;multi_port=%u\r\n",
                             (unsigned)image_port, (unsigned)multi_port);
                } else {
                    snprintf(extra, sizeof(extra),
                             "Transport: RTP/AVP/TCP;interleaved=0-1\r\n");
                }
                rtsp_send_req(rtsp_sock, "SETUP", pres_url, extra, cseq);
                ESP_LOGI(kTag, "[RTSP] -> SETUP (cseq=%d)", cseq);
                cseq_of_setup = cseq;
                cseq++;
                rtsp_state = 1;
            }
        } else if (strcmp(method, "PLAY") == 0) {
            rtsp_send_resp(rtsp_sock, peer_cseq, nullptr);
            ESP_LOGI(kTag, "[RTSP] -> PLAY resp (STREAM STARTED!)");
            rtsp_state = 2;
        } else if (strcmp(method, "TEARDOWN") == 0) {
            rtsp_send_resp(rtsp_sock, peer_cseq, nullptr);
            ESP_LOGI(kTag, "[RTSP] <- TEARDOWN, closing");
            break;
        } else {
            ESP_LOGW(kTag, "[RTSP] unhandled: %s (cseq=%d, body_len=%d)",
                     method, peer_cseq, body_len);
            rtsp_send_resp(rtsp_sock, peer_cseq, nullptr);
        }
    }

    // ── PLAY 后进入统一循环：RTSP keepalive + RTP 媒体 ──
    // 关键：不能用两个 task 分别读同一个 socket（竞态条件：media task
    // 会吞掉 RTSP keepalive，手机收不到响应 → 断联）。
    // 对齐参考实现：单循环处理两种数据，靠首字节 '$' 区分 RTP 和 RTSP。
    if (rtsp_state >= 2) {
        // 初始化 AAC 解码器。
        AacDecoderCtx aac;
        if (!aac.open()) {
            ESP_LOGE(kTag, "[PLAY] AAC decoder open failed");
            goto rtsp_cleanup;
        }

        // 获取音频 codec（立体声）。
        if (s_stream_start && !s_stream_start()) {
            ESP_LOGW(kTag, "[PLAY] audio session acquire failed");
        }

        // RTP/ADTS 缓冲区（PSRAM）。
        uint8_t *rtp_buf = static_cast<uint8_t *>(
            heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        constexpr size_t kAdtsBufSize = 8192;
        uint8_t *adts_buf = static_cast<uint8_t *>(
            heap_caps_malloc(kAdtsBufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!rtp_buf || !adts_buf) {
            ESP_LOGE(kTag, "[PLAY] media buf alloc failed");
            heap_caps_free(rtp_buf);
            heap_caps_free(adts_buf);
            aac.close();
            goto rtsp_cleanup;
        }
        size_t adts_buf_used = 0;

        // SO_RCVBUF（大缓冲减少丢包）。
        int rcvbuf = 32 * 1024;
        setsockopt(rtsp_sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

        // 数据 socket 选择：有 multi_sock 时 RTP 走数据 socket，RTSP 走原 socket。
        int media_sock = multi_sock >= 0 ? multi_sock : rtsp_sock;
        ESP_LOGI(kTag, "[PLAY] media_sock=%d (multi=%d rtsp=%d)",
                 media_sock, multi_sock, rtsp_sock);

        ESP_LOGI(kTag, "[PLAY] Entering unified RTSP+RTP loop...");
        TickType_t loop_start = xTaskGetTickCount();
        uint32_t pkt_count = 0, rtp_total = 0, keepalive_count = 0;
        TickType_t last_log = loop_start;
        TickType_t last_keepalive_log = loop_start;

        while (s_media_generation == generation) {
            // poll 100ms 超时，兼顾 RTP 实时性和 keepalive 响应。
            // 用 poll() 替代 select() 避免 lwIP 栈上 scb 竞态崩溃。
            struct pollfd pfds[2] = {};
            int nfds = 0;
            pfds[0].fd = media_sock;
            pfds[0].events = POLLIN;
            nfds = 1;
            if (multi_sock >= 0) {
                pfds[1].fd = rtsp_sock;
                pfds[1].events = POLLIN;
                nfds = 2;
            }
            int sr = poll(pfds, nfds, 100);
            if (sr < 0) {
                ESP_LOGW(kTag, "[PLAY] poll err: %d", errno);
                break;
            }
            if (sr == 0) {
                // 超时，继续（generation 检查）。
                continue;
            }

            // RTSP socket 有数据 → 处理 keepalive/TEARDOWN。
            if (multi_sock >= 0 && nfds >= 2 && (pfds[1].revents & POLLIN)) {
                uint8_t ka_first;
                int ka_pn = recv(rtsp_sock, &ka_first, 1, MSG_PEEK);
                if (ka_pn <= 0) {
                    ESP_LOGW(kTag, "[PLAY] rtsp sock closed");
                    break;
                }
                // 读完整 RTSP 消息并响应。
                char ka_hdr[512];
                char ka_body[256];
                int ka_body_len;
                int ka_ret = rtsp_read_msg(rtsp_sock, ka_hdr, sizeof(ka_hdr),
                                           ka_body, sizeof(ka_body),
                                           &ka_body_len);
                if (ka_ret > 0) {
                    char ka_method[32] = {0};
                    sscanf(ka_hdr, "%31s", ka_method);
                    int ka_cseq = rtsp_get_cseq(ka_hdr);
                    if (strcmp(ka_method, "GET_PARAMETER") == 0) {
                        rtsp_send_resp(rtsp_sock, ka_cseq, nullptr);
                        keepalive_count++;
                    } else if (strcmp(ka_method, "TEARDOWN") == 0) {
                        rtsp_send_resp(rtsp_sock, ka_cseq, nullptr);
                        ESP_LOGI(kTag, "[RTSP] TEARDOWN, exit");
                        goto play_cleanup;
                    }
                }
                if (sr == 1) continue;  // 只有 RTSP 数据，继续
            }

            // 窥视首字节区分 RTP vs RTSP。
            uint8_t first;
            int pn = recv(media_sock, &first, 1, MSG_PEEK);
            if (pn <= 0) {
                if (pn == 0) {
                    ESP_LOGW(kTag, "[PLAY] sock closed, pkts=%lu ka=%lu",
                             (unsigned long)pkt_count,
                             (unsigned long)keepalive_count);
                } else {
                    ESP_LOGW(kTag, "[PLAY] peek err errno=%d", errno);
                }
                break;
            }

            if (first == 0x24) {
                // ── RTP interleaved 帧 ──
                // 读 4 字节帧头：$ + channel + len(u16 BE)。
                uint8_t hdr[4];
                int got = 0;
                while (got < 4) {
                    int n = recv(media_sock, hdr + got, 4 - got, 0);
                    if (n <= 0) {
                        if (n == 0) goto play_cleanup;
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            if (s_media_generation != generation)
                                goto play_cleanup;
                            continue;
                        }
                        goto play_cleanup;
                    }
                    got += n;
                }
                if (s_media_generation != generation) break;

                uint16_t rtp_len =
                    (static_cast<uint16_t>(hdr[2]) << 8) | hdr[3];
                if (rtp_len < 12 || rtp_len > 4096) {
                    ESP_LOGW(kTag, "[PLAY] bad RTP len: %u", rtp_len);
                    continue;
                }

                got = 0;
                while (got < rtp_len) {
                    int n =
                        recv(media_sock, rtp_buf + got, rtp_len - got, 0);
                    if (n > 0) {
                        got += n;
                        continue;
                    }
                    if (n == 0) goto play_cleanup;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        if (s_media_generation != generation)
                            goto play_cleanup;
                        continue;
                    }
                    goto play_cleanup;
                }
                rtp_total += rtp_len;
                pkt_count++;
                s_media_pkt_total = pkt_count;
                if (pkt_count <= 3) {
                    ESP_LOGI(
                        kTag,
                        "[PLAY] RTP#%lu len=%u first=[%02X %02X %02X %02X]",
                        (unsigned long)pkt_count, rtp_len, rtp_buf[0],
                        rtp_buf[1], rtp_buf[2], rtp_buf[3]);
                }

                // 剥 RTP 头。
                uint8_t *ts_data = rtp_buf;
                uint32_t ts_len = rtp_len;
                if ((rtp_buf[0] & 0xC0) == 0x80) {
                    int hdr_len = 12 + (rtp_buf[0] & 0x0F) * 4;
                    if (rtp_buf[0] & 0x10) {
                        if (hdr_len + 4 <= static_cast<int>(rtp_len)) {
                            int ext_words =
                                (static_cast<uint16_t>(rtp_buf[hdr_len + 2])
                                 << 8) |
                                rtp_buf[hdr_len + 3];
                            hdr_len += 4 + ext_words * 4;
                        }
                    }
                    int end = rtp_len;
                    if (rtp_buf[0] & 0x20) {
                        end -= rtp_buf[rtp_len - 1];
                    }
                    if (hdr_len < end) {
                        ts_data = rtp_buf + hdr_len;
                        ts_len = end - hdr_len;
                    }
                }

                // PES 解密。
                if (s_has_stream_key) {
                    decrypt_ts_media(ts_data, ts_len);
                }

                // 一次性诊断：打印剥头后前 16 字节与 TS 匹配统计。
                if (pkt_count == 4) {
                    ESP_LOGW(kTag, "[DIAG] ts_len=%u has_key=%d first16="
                                   "%02X %02X %02X %02X %02X %02X %02X %02X "
                                   "%02X %02X %02X %02X %02X %02X %02X %02X",
                             (unsigned)ts_len, (int)s_has_stream_key,
                             ts_data[0], ts_data[1], ts_data[2], ts_data[3],
                             ts_data[4], ts_data[5], ts_data[6], ts_data[7],
                             ts_data[8], ts_data[9], ts_data[10], ts_data[11],
                             ts_data[12], ts_data[13], ts_data[14], ts_data[15]);
                    int ts_ok = 0, pid_ok = 0;
                    for (size_t o = 0; o + kTsPktSize <= ts_len; o += kTsPktSize) {
                        if (ts_data[o] == kTsSyncByte) {
                            ts_ok++;
                            uint16_t p = ((uint16_t)(ts_data[o+1] & 0x1F) << 8) | ts_data[o+2];
                            if (p == kTargetPid) pid_ok++;
                        }
                    }
                    ESP_LOGW(kTag, "[DIAG] ts_pkts=%d pid_1100=%d adts_used=%u",
                             ts_ok, pid_ok, (unsigned)adts_buf_used);
                }

                // TS demux → ADTS → AAC 解码。
                for (size_t off = 0; off + kTsPktSize <= ts_len;
                     off += kTsPktSize) {
                    uint8_t *pkt = ts_data + off;
                    if (pkt[0] != kTsSyncByte) continue;
                    uint16_t pid =
                        (static_cast<uint16_t>(pkt[1] & 0x1F) << 8) |
                        pkt[2];
                    if (pid != kTargetPid) continue;
                    uint8_t afc = (pkt[3] >> 4) & 0x03;
                    if (afc == 0 || afc == 2) continue;
                    bool pusi = (pkt[1] & 0x40) != 0;
                    int pay_off = 4;
                    if (afc == 3) pay_off = 5 + pkt[4];
                    int pay_len = kTsPktSize - pay_off;
                    if (pay_len <= 0) continue;

                    const uint8_t *audio_data = pkt + pay_off;
                    int audio_len = pay_len;
                    if (pusi && pay_len >= 9 && pkt[pay_off] == 0 &&
                        pkt[pay_off + 1] == 0 && pkt[pay_off + 2] == 1 &&
                        pkt[pay_off + 3] == 0xC0) {
                        int pes_hdr = 9 + pkt[pay_off + 8];
                        if (pes_hdr >= pay_len) {
                            adts_buf_used = 0;
                            continue;
                        }
                        audio_data = pkt + pay_off + pes_hdr;
                        audio_len = pay_len - pes_hdr;
                    }

                    if (audio_len > 0 &&
                        adts_buf_used + audio_len <= kAdtsBufSize) {
                        memcpy(adts_buf + adts_buf_used, audio_data,
                               audio_len);
                        adts_buf_used += audio_len;
                    }

                    int scan_off = 0;
                    int frames_found = 0;
                    while (scan_off + 7 <= adts_buf_used) {
                        if (adts_buf[scan_off] != 0xFF ||
                            (adts_buf[scan_off + 1] & 0xF6) != 0xF0) {
                            scan_off++;
                            continue;
                        }
                        uint16_t frame_len =
                            (static_cast<uint16_t>(
                                 adts_buf[scan_off + 3] & 0x03)
                             << 11) |
                            (static_cast<uint16_t>(adts_buf[scan_off + 4])
                             << 3) |
                            (static_cast<uint16_t>(
                                 adts_buf[scan_off + 5] >>
                                 5));
                        if (frame_len < 7) {
                            scan_off++;
                            continue;
                        }
                        if (scan_off + frame_len > adts_buf_used) break;
                        {
                            static int s_adts_diag = 0;
                            if (s_adts_diag < 3) {
                                s_adts_diag++;
                                ESP_LOGW(kTag, "[DIAG] ADTS frame #%d: "
                                               "off=%d len=%u total=%u",
                                         s_adts_diag, scan_off,
                                         (unsigned)frame_len,
                                         (unsigned)adts_buf_used);
                            }
                        }
                        aac.decode_and_play(adts_buf + scan_off, frame_len);
                        frames_found++;
                        scan_off += frame_len;
                    }
                    if (pkt_count == 4 && frames_found == 0 && adts_buf_used > 0) {
                        ESP_LOGW(kTag, "[DIAG] adts_buf head: "
                                       "%02X %02X %02X %02X %02X %02X %02X %02X "
                                       "(used=%u, no ADTS sync)",
                                 adts_buf[0], adts_buf[1], adts_buf[2],
                                 adts_buf[3], adts_buf[4], adts_buf[5],
                                 adts_buf[6], adts_buf[7],
                                 (unsigned)adts_buf_used);
                    }
                    if (scan_off > 0 && scan_off < adts_buf_used) {
                        memmove(adts_buf, adts_buf + scan_off,
                                adts_buf_used - scan_off);
                        adts_buf_used -= scan_off;
                    } else if (scan_off >= adts_buf_used) {
                        adts_buf_used = 0;
                    }
                }

            } else {
                // ── RTSP 信令消息（keepalive / TEARDOWN 等）──
                // 设置短超时，避免阻塞 RTP 接收。
                struct timeval rtsp_tv = {.tv_sec = 0,
                                          .tv_usec = 500000};
                setsockopt(rtsp_sock, SOL_SOCKET, SO_RCVTIMEO, &rtsp_tv,
                           sizeof(rtsp_tv));

                int ret = rtsp_read_msg(rtsp_sock, headers, kRtspBufSize,
                                        body, kRtspBufSize, &body_len);

                // 恢复无超时（select 管理等待）。
                struct timeval no_tv = {.tv_sec = 0, .tv_usec = 0};
                setsockopt(rtsp_sock, SOL_SOCKET, SO_RCVTIMEO, &no_tv,
                           sizeof(no_tv));

                if (ret <= 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                    ESP_LOGW(kTag, "[PLAY] RTSP read err ret=%d errno=%d",
                             ret, errno);
                    break;
                }
                int peer_cseq = rtsp_get_cseq(headers);
                int is_resp =
                    (strncmp(headers, "RTSP/1.0 ", 9) == 0);
                if (is_resp) {
                    ESP_LOGD(kTag,
                             "[PLAY] <- RTSP resp cseq=%d (keepalive)",
                             peer_cseq);
                    continue;
                }
                char method[32] = {0};
                sscanf(headers, "%31s", method);
                if (strcmp(method, "GET_PARAMETER") == 0) {
                    rtsp_send_resp(rtsp_sock, peer_cseq, nullptr);
                    keepalive_count++;
                } else if (strcmp(method, "TEARDOWN") == 0) {
                    rtsp_send_resp(rtsp_sock, peer_cseq, nullptr);
                    ESP_LOGI(kTag, "[PLAY] <- TEARDOWN");
                    break;
                } else {
                    rtsp_send_resp(rtsp_sock, peer_cseq, nullptr);
                    keepalive_count++;
                }
            }

            // 每 5 秒日志。
            if (xTaskGetTickCount() - last_log >= pdMS_TO_TICKS(5000)) {
                ESP_LOGI(kTag,
                         "[PLAY] %lus pkts=%lu rtp=%luB ka=%lu",
                         (unsigned long)((xTaskGetTickCount() - loop_start) /
                                         configTICK_RATE_HZ),
                         (unsigned long)pkt_count,
                         (unsigned long)rtp_total,
                         (unsigned long)keepalive_count);
                last_log = xTaskGetTickCount();
            }
        }

    play_cleanup:
        ESP_LOGI(kTag,
                 "[PLAY] Loop exit: pkts=%lu rtp=%luB ka=%lu",
                 (unsigned long)pkt_count, (unsigned long)rtp_total,
                 (unsigned long)keepalive_count);
        heap_caps_free(rtp_buf);
        heap_caps_free(adts_buf);
        aac.close();
        if (s_stream_stop) s_stream_stop();
    }

rtsp_cleanup:
    heap_caps_free(headers);
    heap_caps_free(body);
    if (multi_sock >= 0) close(multi_sock);
    if (rtsp_sock >= 0) close(rtsp_sock);
    ESP_LOGI(kTag, "[RTSP] Cleanup done");
    vTaskDelay(pdMS_TO_TICKS(kRtspRetryBackoffMs));
}

// ── RTSP 任务包装器 ──
struct RtspTaskArg {
    char host[64];
    int port;
    int client_sock;
    uint32_t generation;
    const miplay_session_t *session;  // 指向控制会话，生命周期由 control 管理
};

void miplay_rtsp_task_wrapper(void *arg)
{
    RtspTaskArg *a = static_cast<RtspTaskArg *>(arg);
    ESP_LOGI(kTag, "[RTSP-task] Starting: %s:%d sock=%d gen=%lu", a->host,
             a->port, a->client_sock, (unsigned long)a->generation);
    miplay_rtsp_run(a->host, a->port, a->client_sock, a->generation,
                    a->session);
    free(a);
    if (s_rtsp_task_handle == xTaskGetCurrentTaskHandle())
        s_rtsp_task_handle = nullptr;
    ESP_LOGI(kTag, "[RTSP-task] Done");
    // 必须走 miplay_delete_current_task()：本任务由 miplay_create_task 创建，
    // 在 s_task_caps 里占了一个槽。直接 vTaskDeleteWithCaps() 会绕过清槽，
    // 槽位永久泄漏；累积后 miplay_delete_current_task 找不到自己，退化为
    // 普通 vTaskDelete()，PSRAM 栈就再也释放不掉。
    miplay_delete_current_task();
}

}  // namespace

// ══════════════════════════════════════════════════════════════════════
// 公共 API
// ══════════════════════════════════════════════════════════════════════

extern "C" {

void miplay_media_handle_open_device(miplay_session_t *session,
                                     const uint8_t *payload, uint32_t plen,
                                     uint16_t seq)
{
    if (!session || !payload || plen == 0) {
        ESP_LOGW(kTag, "OPEN: null/empty payload");
        // 仍回 ACK 保活。
        const uint8_t ack_body[] = {0, 0, 0, 0, 0};
        miplay_send_encrypted(session, CMD_OPEN_DEVICE + 1, seq, ack_body,
                              sizeof(ack_body));
        session->media_session_opened = true;
        return;
    }

    // 解析 wfd://host:port?mirrorMode=1（前 16 字节可能异常，用 memmem）。
    const char *wfd_start =
        static_cast<const char *>(memmem(payload, plen, "wfd://", 6));
    if (!wfd_start) {
        ESP_LOGW(kTag, "OPEN: wfd:// not found in %u bytes",
                 static_cast<unsigned>(plen));
        const uint8_t ack_body[] = {0, 0, 0, 0, 0};
        miplay_send_encrypted(session, CMD_OPEN_DEVICE + 1, seq, ack_body,
                              sizeof(ack_body));
        session->media_session_opened = true;
        return;
    }

    char wfd_url[160] = {0};
    int url_len = static_cast<int>(plen) -
                  static_cast<int>(wfd_start - reinterpret_cast<const char *>(payload));
    if (url_len > 159) url_len = 159;
    memcpy(wfd_url, wfd_start, url_len);
    wfd_url[url_len] = '\0';
    ESP_LOGI(kTag, "OPEN body: %s", wfd_url);

    // 解析 host:port。
    char rtsp_host[64] = {0};
    int rtsp_port = 0;
    const char *u = strstr(wfd_url, "wfd://");
    if (u) {
        u += 6;
        const char *colon = strchr(u, ':');
        const char *q = strchr(u, '?');
        if (colon && (!q || colon < q)) {
            int hl = colon - u;
            if (hl > 0 && hl < static_cast<int>(sizeof(rtsp_host))) {
                memcpy(rtsp_host, u, hl);
                rtsp_port = atoi(colon + 1);
            }
        } else if (q) {
            int hl = q - u;
            if (hl > 0 && hl < static_cast<int>(sizeof(rtsp_host))) {
                memcpy(rtsp_host, u, hl);
                rtsp_port = 8554;  // WFD 默认端口
            }
        }
    }
    ESP_LOGI(kTag, "RTSP target: %s:%d", rtsp_host, rtsp_port);

    // 回复 OPEN_ACK。
    uint8_t ack_body[] = {0, 0, 0, 0, 0};
    miplay_send_encrypted(session, CMD_OPEN_DEVICE + 1, seq, ack_body,
                          sizeof(ack_body));
    ESP_LOGI(kTag, "-> OPEN_ACK");
    session->media_session_opened = true;

    // 启动 RTSP 任务。
    if (rtsp_port > 0) {
        uint32_t gen = ++s_media_generation;
        ESP_LOGI(kTag, "[RTSP] media_generation -> %lu",
                 (unsigned long)gen);

        RtspTaskArg *rtsp_arg = static_cast<RtspTaskArg *>(
            heap_caps_malloc(sizeof(RtspTaskArg),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (rtsp_arg) {
            strncpy(rtsp_arg->host, rtsp_host, sizeof(rtsp_arg->host) - 1);
            rtsp_arg->host[sizeof(rtsp_arg->host) - 1] = '\0';
            rtsp_arg->port = rtsp_port;
            rtsp_arg->client_sock = session->sock;
            rtsp_arg->generation = gen;
            rtsp_arg->session = session;

            // 等旧任务退出。
            TaskHandle_t old = s_rtsp_task_handle;
            s_rtsp_task_handle = nullptr;
            if (old) {
                for (int wt = 0; wt < 50 && eTaskGetState(old) != eDeleted;
                     wt++) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            }
            if (miplay_create_task(miplay_rtsp_task_wrapper, "miplay_rtsp",
                                   kRtspTaskStack, rtsp_arg, 4,
                                   reinterpret_cast<void **>(
                                       &s_rtsp_task_handle),
                                   1) == pdPASS) {
                ESP_LOGI(kTag, "[RTSP] Task launched");
                return;
            }
            ESP_LOGW(kTag, "[RTSP] Task creation failed");
            free(rtsp_arg);
        }

        // 回退：内联运行（仅在任务创建失败时）。
        ESP_LOGW(kTag, "[RTSP] Inline fallback");
        miplay_rtsp_run(rtsp_host, rtsp_port, session->sock, gen, session);
    } else {
        ESP_LOGW(kTag, "OPEN: no valid host:port parsed");
    }
}

void miplay_media_set_stream_key(const uint8_t *payload, uint32_t plen)
{
    if (!payload || plen == 0) return;

    // 提取 streamKey, streamIV, authKey。
    const char *sk = strstr(reinterpret_cast<const char *>(payload),
                            "\"streamKey\"");
    const char *siv = strstr(reinterpret_cast<const char *>(payload),
                             "\"streamIV\"");
    const char *ak = strstr(reinterpret_cast<const char *>(payload),
                            "\"authKey\"");
    if (sk && siv) {
        sk = strchr(sk, ':');
        siv = strchr(siv, ':');
        if (sk && siv) {
            sk++;
            siv++;
            while (*sk == ' ' || *sk == '"') sk++;
            while (*siv == ' ' || *siv == '"') siv++;
            memcpy(s_stream_key, sk, 16);
            memcpy(s_stream_iv, siv, 16);
            s_has_stream_key = true;
            ESP_LOGI(kTag, "SetMirrorKey: streamKey=%.16s streamIV=%.16s",
                     s_stream_key, s_stream_iv);
        }
    }
    if (ak) {
        ak = strchr(ak, ':');
        if (ak) {
            ak++;
            while (*ak == ' ' || *ak == '"') ak++;
            memcpy(s_mirror_auth_key, ak, 16);
            s_mirror_auth_key[16] = '\0';
            s_has_mirror_auth_key = true;
            ESP_LOGI(kTag, "SetMirrorKey: authKey=%.16s", s_mirror_auth_key);
        }
    }
    if (!sk || !siv) {
        ESP_LOGW(kTag,
                 "SetMirrorKey: streamKey/streamIV not found in JSON");
    }
}

bool miplay_media_is_streaming()
{
    return s_rtsp_task_handle != nullptr;
}

void miplay_media_set_volume(uint32_t percent)
{
    if (percent > 100) percent = 100;
    if (s_volume_set) s_volume_set(static_cast<int>(percent));
}

uint32_t miplay_media_get_volume()
{
    if (s_volume_get) return static_cast<uint32_t>(s_volume_get());
    return 50;
}

void miplay_media_set_volume_cb(miplay_media_volume_set_fn set_fn,
                                 miplay_media_volume_get_fn get_fn)
{
    s_volume_set = set_fn;
    s_volume_get = get_fn;
}

static miplay_media_meta_fn s_meta_cb = nullptr;

void miplay_media_set_meta_cb(miplay_media_meta_fn fn)
{
    s_meta_cb = fn;
}

void miplay_media_dispatch_meta(const char *title, const char *artist,
                                const char *album, int64_t duration_ms,
                                int64_t position_ms)
{
    if (s_meta_cb) {
        s_meta_cb(title, artist, album, duration_ms, position_ms);
    }
}

static miplay_media_pause_fn s_pause_cb = nullptr;

void miplay_media_set_pause_cb(miplay_media_pause_fn fn)
{
    s_pause_cb = fn;
}

void miplay_media_dispatch_pause(bool paused)
{
    if (s_pause_cb) {
        s_pause_cb(paused);
    }
}

void miplay_media_stop()
{
    s_media_generation++;
    s_has_stream_key = false;
    s_has_mirror_auth_key = false;
    s_media_pkt_total = 0;
    rtsp_read_msg_reset();
    ESP_LOGI(kTag, "[MEDIA] Stopped (gen -> %lu)",
             (unsigned long)s_media_generation);

    // 等待 RTSP 任务真正退出，避免 disconnect_cleanup 关 socket 时任务仍在用。
    // 这是 lwip err_tcp use-after-free 崩溃的根因修复。
    for (int i = 0; i < 200 && s_rtsp_task_handle; i++) {
        eTaskState st = eTaskGetState(s_rtsp_task_handle);
        if (st == eDeleted || st == eInvalid) {
            s_rtsp_task_handle = nullptr;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_rtsp_task_handle) {
        ESP_LOGW(kTag, "[MEDIA] RTSP task did not exit in time");
        s_rtsp_task_handle = nullptr;
    }
}

}  // extern "C"
