// 局域网发现层：让小米「设备互联」能发现本设备并知道它是 TV。
//
// 三层机制并存，缺一层手机可能就发现不了：
//   ① mDNS 服务注册（5353）：_mi-connect._udp + _lyra-mdns._udp
//   ② UDP 5355 legacy DNS-SD 应答：手机 SystemUI 走这条，查 _miplay_lan._tcp
//   ③ mDNS 主动宣告：启动 3 轮 + 每 30s 刷新，应对手机侧冷缓存
//
// 与参考实现的差异：应答包改为每包动态构建。参考实现预构建后只改 TX ID，
// 导致 Wi-Fi 重连换 IP 后 A 记录仍是旧地址；动态构建消除这个隐患。
#include "miplay_remote_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

// ── DNS 编码原语 ──

void dns_push_u16(uint8_t *p, size_t *o, uint16_t v)
{
    p[(*o)++] = (uint8_t)(v >> 8);
    p[(*o)++] = (uint8_t)(v & 0xFF);
}

void dns_push_u32(uint8_t *p, size_t *o, uint32_t v)
{
    p[(*o)++] = (uint8_t)((v >> 24) & 0xFF);
    p[(*o)++] = (uint8_t)((v >> 16) & 0xFF);
    p[(*o)++] = (uint8_t)((v >> 8) & 0xFF);
    p[(*o)++] = (uint8_t)(v & 0xFF);
}

void dns_push_label(uint8_t *p, size_t *o, const char *label)
{
    size_t len = strlen(label);
    if (len > 63) len = 63;
    p[(*o)++] = (uint8_t)len;
    memcpy(p + *o, label, len);
    *o += len;
}

void dns_push_ptr(uint8_t *p, size_t *o, uint16_t offset)
{
    uint16_t v = (uint16_t)(0xC000 | (offset & 0x3FFF));
    p[(*o)++] = (uint8_t)(v >> 8);
    p[(*o)++] = (uint8_t)(v & 0xFF);
}

// ── UDP 5355 legacy DNS-SD ──

namespace {

constexpr char kTag[] = "miplay_remote";

// _miplay_lan._tcp.local 的 wire 格式 QNAME。
const uint8_t kServiceQname[] = {
    0x0b, '_', 'm', 'i', 'p', 'l', 'a', 'y', '_', 'l', 'a', 'n',
    0x04, '_', 't', 'c', 'p',
    0x05, 'l', 'o', 'c', 'a', 'l',
    0x00,
};
constexpr size_t kServiceQnameLen = sizeof(kServiceQname);

char s_lan_hostname[32];
int s_lan_sock = -1;
TaskHandle_t s_lan_task = nullptr;
TaskHandle_t s_announce_task = nullptr;
TaskHandle_t s_scan_task = nullptr;

// 构建 _miplay_lan._tcp 的 DNS-SD 响应。
//
// 两个刻意的设计：
//   - qdcount=0：DNS-SD 响应不回显 question section，否则手机按错误偏移
//     解析并丢弃整包。
//   - PTR 的 NAME 从服务 QNAME 开始，SRV/TXT/A 通过压缩指针引用它，
//     所以构建顺序不能调整（指针偏移是硬耦合的）。
size_t build_lan_packet(uint8_t *p, bool is_unicast)
{
    size_t o = 0;
    const uint32_t ip = miplay_get_my_ipv4();
    const uint16_t record_class = 0x8001;  // 唯一的 SRV/TXT/A 带 cache-flush 位
    const uint32_t ttl = is_unicast ? 10 : 60;

    dns_push_u16(p, &o, 0x0000);  // TX ID，由调用方覆盖
    dns_push_u16(p, &o, 0x8400);  // 权威应答
    dns_push_u16(p, &o, 0);       // QDCOUNT
    dns_push_u16(p, &o, 1);       // ANCOUNT = PTR
    dns_push_u16(p, &o, 0);       // NSCOUNT
    dns_push_u16(p, &o, 3);       // ARCOUNT = SRV + TXT + A

    // PTR answer
    const uint16_t service_off = (uint16_t)o;
    memcpy(p + o, kServiceQname, kServiceQnameLen);
    o += kServiceQnameLen;
    dns_push_u16(p, &o, 12);  // TYPE = PTR
    dns_push_u16(p, &o, 1);   // PTR 属于共享数据，不带 cache-flush 位
    dns_push_u32(p, &o, ttl);
    const uint16_t instance_len = (uint16_t)strlen(s_inst_name);
    dns_push_u16(p, &o, (uint16_t)(instance_len + 3));
    const uint16_t instance_off = (uint16_t)o;
    dns_push_label(p, &o, s_inst_name);
    dns_push_ptr(p, &o, service_off);

    // SRV：端口必须是 8899，手机据此连控制口。
    dns_push_ptr(p, &o, instance_off);
    dns_push_u16(p, &o, 33);
    dns_push_u16(p, &o, record_class);
    dns_push_u32(p, &o, ttl);
    size_t srv_len_pos = o;
    dns_push_u16(p, &o, 0);
    const size_t srv_rdata = o;
    dns_push_u16(p, &o, 0);  // Priority
    dns_push_u16(p, &o, 0);  // Weight
    dns_push_u16(p, &o, MIPLAY_CONTROL_PORT);
    const uint16_t host_off = (uint16_t)o;
    dns_push_label(p, &o, s_lan_hostname);
    p[o++] = 0x05;
    memcpy(p + o, "local", 5);
    o += 5;
    p[o++] = 0x00;
    const uint16_t srv_len = (uint16_t)(o - srv_rdata);
    p[srv_len_pos] = (uint8_t)(srv_len >> 8);
    p[srv_len_pos + 1] = (uint8_t)(srv_len & 0xFF);

    // TXT：appdata JSON，其中 "type":3 是申报 TV 的关键字段。
    dns_push_ptr(p, &o, instance_off);
    dns_push_u16(p, &o, 16);
    dns_push_u16(p, &o, record_class);
    dns_push_u32(p, &o, ttl);

    // 5355 的 idhash 必须与 mDNS _mi-connect 的 idHash **逐字节相同**：
    // 4 字符标准 base64（base64(short_id) 的必然长度）。
    // 手机侧的 BonjourGovernor 会比对两条发现路径上的身份字段，任何
    // 不一致都会让整条记录被拒——症状就是"设备扫不到"。
    // 旧实现在此截断成 3 字符，与 mDNS 的 4 字符冲突，已修正。
    const char *idhash_lan = s_idhash;
    // stable_id 取 MAC 后 4 字节，清掉最高位避免负数。
    const int stable_id = (int)((((uint32_t)s_mac[2] << 24) |
                                 ((uint32_t)s_mac[3] << 16) |
                                 ((uint32_t)s_mac[4] << 8) |
                                 s_mac[5]) & 0x7FFFFFFF);

    char appdata_json[256];
    snprintf(appdata_json, sizeof(appdata_json),
             "{\"supportLyra\":true,"
             "\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
             "\"ID\":%d,"
             "\"type\":3,"
             "\"idhash\":\"%s\","
             "\"extraAbility\":0}",
             s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5],
             stable_id, idhash_lan);

    char txt_rec[320];
    int tl = snprintf(txt_rec, sizeof(txt_rec), "appdata=%s", appdata_json);
    if (tl > 255) tl = 255;
    dns_push_u16(p, &o, (uint16_t)(tl + 1));
    p[o++] = (uint8_t)tl;
    memcpy(p + o, txt_rec, (size_t)tl);
    o += (size_t)tl;

    // A：本机 IP，四段小端序写入。
    dns_push_ptr(p, &o, host_off);
    dns_push_u16(p, &o, 1);
    dns_push_u16(p, &o, record_class);
    dns_push_u32(p, &o, ttl);
    dns_push_u16(p, &o, 4);
    p[o++] = (uint8_t)(ip & 0xFF);
    p[o++] = (uint8_t)((ip >> 8) & 0xFF);
    p[o++] = (uint8_t)((ip >> 16) & 0xFF);
    p[o++] = (uint8_t)((ip >> 24) & 0xFF);

    return o;
}

void lan_reply_to_query(int sock, const uint8_t *query, size_t qlen,
                        const struct sockaddr_in *src, socklen_t srclen)
{
    uint8_t *reply = (uint8_t *)heap_caps_malloc(512, MALLOC_CAP_SPIRAM);
    if (!reply) {
        return;
    }
    const size_t len = build_lan_packet(reply, true);
    reply[0] = query[0];  // 回显查询的 TX ID
    reply[1] = query[1];
    sendto(sock, reply, len, 0, (const struct sockaddr *)src, srclen);
    free(reply);
}

void lan_task(void *)
{
    s_lan_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_lan_sock < 0) {
        ESP_LOGE(kTag, "LAN UDP socket failed: %d", errno);
        s_lan_task = nullptr;
        miplay_delete_current_task();
        return;
    }
    int reuse = 1;
    setsockopt(s_lan_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in bind_addr = {};
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(MIPLAY_LAN_DISCOVERY_PORT);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s_lan_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(kTag, "LAN bind failed: %d", errno);
        close(s_lan_sock);
        s_lan_sock = -1;
        s_lan_task = nullptr;
        miplay_delete_current_task();
        return;
    }

    struct ip_mreq mreq = {};
    mreq.imr_multiaddr.s_addr = inet_addr(MIPLAY_MDNS_GROUP);
    mreq.imr_interface.s_addr = miplay_get_my_ipv4();
    // 必须检查返回值：join 失败时 5355 收不到任何组播查询，设备只剩手工
    // announce 这一条路径；手机在 announce 间隙发的 query 得不到回应就会
    // 跳过本设备。静默失败会让这个症状看起来像"协议不对"。
    if (setsockopt(s_lan_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq,
                   sizeof(mreq)) != 0) {
        ESP_LOGE(kTag,
                 "IP_ADD_MEMBERSHIP failed: errno=%d iface=0x%08X "
                 "(LAN discovery degraded to announce-only)",
                 errno, (unsigned)ntohl(mreq.imr_interface.s_addr));
    }

    // 关闭组播回环：否则自己发的宣告会被当成查询，回发到本机 5355
    // 形成无限应答循环。
    unsigned char mcast_loop = 0;
    setsockopt(s_lan_sock, IPPROTO_IP, IP_MULTICAST_LOOP, &mcast_loop,
               sizeof(mcast_loop));

    struct timeval tv = {};
    tv.tv_sec = 0;
    tv.tv_usec = 100000;  // 100ms，便于快速响应退出
    setsockopt(s_lan_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(kTag, "LAN discovery UDP %d listening", MIPLAY_LAN_DISCOVERY_PORT);

    // 启动宣告 3 轮，让已开机等待的手机立刻感知到。
    struct sockaddr_in mcast_dest = {};
    mcast_dest.sin_family = AF_INET;
    mcast_dest.sin_port = htons(MIPLAY_LAN_DISCOVERY_PORT);
    mcast_dest.sin_addr.s_addr = inet_addr(MIPLAY_MDNS_GROUP);
    for (int round = 1; round <= 3 && s_running; round++) {
        uint8_t buf[512];
        const size_t len = build_lan_packet(buf, false);
        sendto(s_lan_sock, buf, len, 0, (struct sockaddr *)&mcast_dest,
               sizeof(mcast_dest));
        ESP_LOGI(kTag, "LAN announcement %d/3 sent (%u bytes)", round,
                 (unsigned)len);
        vTaskDelay(pdMS_TO_TICKS(180));
    }

    while (s_running) {
        uint8_t rx[512];
        struct sockaddr_in src = {};
        socklen_t srclen = sizeof(src);
        const int n = recvfrom(s_lan_sock, rx, sizeof(rx), 0,
                               (struct sockaddr *)&src, &srclen);
        if (n < 12) {
            continue;
        }
        // 只应答查询：QR=1（响应）或 QDCOUNT=0（宣告）直接忽略。
        // 这是一层额外防护，即使组播回环被底层打开也不会自激。
        const uint16_t dns_flags = ((uint16_t)rx[2] << 8) | rx[3];
        const uint16_t question_count = ((uint16_t)rx[4] << 8) | rx[5];
        if ((dns_flags & 0x8000u) || question_count == 0) {
            continue;
        }

        // 只回应询问 _miplay_lan._tcp 的报文。
        bool found = false;
        for (int i = 0; i + (int)kServiceQnameLen <= n; i++) {
            if (memcmp(rx + i, kServiceQname, kServiceQnameLen) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            continue;
        }

        lan_reply_to_query(s_lan_sock, rx, (size_t)n, &src, srclen);
        ESP_LOGI(kTag, "LAN -> _miplay_lan reply %u.%u.%u.%u",
                 (unsigned)(src.sin_addr.s_addr & 0xFF),
                 (unsigned)((src.sin_addr.s_addr >> 8) & 0xFF),
                 (unsigned)((src.sin_addr.s_addr >> 16) & 0xFF),
                 (unsigned)((src.sin_addr.s_addr >> 24) & 0xFF));
    }

    close(s_lan_sock);
    s_lan_sock = -1;
    s_lan_task = nullptr;
    miplay_delete_current_task();
}

// ── mDNS 主动宣告 ──

// 把 IP 的中间两段做"数字 → '#'+数字"编码，得到 Xiaomi 的 DebugInfo 格式。
// 例：192.168.110.38 → 192.$)+.$$.38
void encode_debug_ip(uint32_t ip, char *out, size_t out_sz)
{
    const uint8_t a[4] = {(uint8_t)(ip & 0xFF), (uint8_t)((ip >> 8) & 0xFF),
                          (uint8_t)((ip >> 16) & 0xFF), (uint8_t)((ip >> 24) & 0xFF)};
    char enc[2][8] = {};
    for (int seg = 0; seg < 2; seg++) {
        const uint8_t val = seg == 0 ? a[1] : a[2];
        char tmp[4];
        snprintf(tmp, sizeof(tmp), "%u", (unsigned)val);
        int oi = 0;
        for (int ci = 0; tmp[ci] && oi < 7; ci++) {
            const char c = tmp[ci];
            enc[seg][oi++] = (c >= '0' && c <= '9') ? (char)('#' + (c - '0')) : c;
        }
        enc[seg][oi] = '\0';
    }
    snprintf(out, out_sz, "%u.%s.%s.%u", (unsigned)a[0], enc[0], enc[1],
             (unsigned)a[3]);
}

// 构建单个 mDNS 服务的主动宣告包（ANCOUNT=1 PTR + ARCOUNT=3 SRV+TXT+A）。
size_t build_mdns_announce(uint8_t *p, size_t p_size,
                           const uint8_t *svc_qname, size_t svc_qname_len,
                           const char *instance, uint16_t port,
                           const mdns_txt_item_t *txt, size_t txt_count,
                           uint32_t ip)
{
    size_t o = 0;
    dns_push_u16(p, &o, 0x0000);
    dns_push_u16(p, &o, 0x8400);
    dns_push_u16(p, &o, 0);  // QDCOUNT
    dns_push_u16(p, &o, 1);  // ANCOUNT
    dns_push_u16(p, &o, 0);
    dns_push_u16(p, &o, 3);  // ARCOUNT

    const uint16_t svc_off = (uint16_t)o;
    memcpy(p + o, svc_qname, svc_qname_len);
    o += svc_qname_len;
    dns_push_u16(p, &o, 12);
    dns_push_u16(p, &o, 0x8001);
    dns_push_u32(p, &o, 120);
    const size_t inst_len = strlen(instance);
    dns_push_u16(p, &o, (uint16_t)(inst_len + 3));
    const uint16_t inst_off = (uint16_t)o;
    dns_push_label(p, &o, instance);
    dns_push_ptr(p, &o, svc_off);

    dns_push_ptr(p, &o, inst_off);
    dns_push_u16(p, &o, 33);
    dns_push_u16(p, &o, 0x8001);
    dns_push_u32(p, &o, 120);
    size_t srv_len_pos = o;
    dns_push_u16(p, &o, 0);
    const size_t srv_start = o;
    dns_push_u16(p, &o, 0);
    dns_push_u16(p, &o, 0);
    dns_push_u16(p, &o, port);
    const uint16_t host_off = (uint16_t)o;
    dns_push_label(p, &o, s_device_id);
    p[o++] = 0x05;
    memcpy(p + o, "local", 5);
    o += 5;
    p[o++] = 0x00;
    const uint16_t srv_len = (uint16_t)(o - srv_start);
    p[srv_len_pos] = (uint8_t)(srv_len >> 8);
    p[srv_len_pos + 1] = (uint8_t)(srv_len & 0xFF);

    // TXT：每条 key=value 是独立的 length-prefixed 字符串。
    dns_push_ptr(p, &o, inst_off);
    dns_push_u16(p, &o, 16);
    dns_push_u16(p, &o, 0x8001);
    dns_push_u32(p, &o, 120);
    size_t txt_rdlen_pos = o;
    dns_push_u16(p, &o, 0);
    const size_t txt_rdlen_start = o;
    for (size_t i = 0; i < txt_count; i++) {
        const size_t kl = strlen(txt[i].key);
        const char *val = txt[i].value ? txt[i].value : "";
        const size_t vl = strlen(val);
        const uint8_t slen = (uint8_t)(kl + 1 + vl);
        if (o + 1 + slen > p_size - 64) {
            break;
        }
        p[o++] = slen;
        memcpy(p + o, txt[i].key, kl);
        o += kl;
        p[o++] = '=';
        memcpy(p + o, val, vl);
        o += vl;
    }
    const uint16_t txt_rdlen = (uint16_t)(o - txt_rdlen_start);
    p[txt_rdlen_pos] = (uint8_t)(txt_rdlen >> 8);
    p[txt_rdlen_pos + 1] = (uint8_t)(txt_rdlen & 0xFF);

    dns_push_ptr(p, &o, host_off);
    dns_push_u16(p, &o, 1);
    dns_push_u16(p, &o, 0x8001);
    dns_push_u32(p, &o, 120);
    dns_push_u16(p, &o, 4);
    p[o++] = (uint8_t)(ip & 0xFF);
    p[o++] = (uint8_t)((ip >> 8) & 0xFF);
    p[o++] = (uint8_t)((ip >> 16) & 0xFF);
    p[o++] = (uint8_t)((ip >> 24) & 0xFF);
    return o;
}

// 组装两类服务的 TXT 记录集合。返回的字符串内容指向调用方提供的缓冲，
// 必须在其生命周期内保持有效。
struct AnnounceTxt {
    mdns_txt_item_t micon[9];
    mdns_txt_item_t lyra[5];
    char ts_str[24];
    char debug_info[128];
};

void fill_announce_txt(AnnounceTxt &t, const char *debug_prefix)
{
    t.micon[0] = {"version", MIPLAY_VERSION};
    t.micon[1] = {"apps", "[5]"};
    t.micon[2] = {"flags", "CgE="};
    t.micon[3] = {"name", MIPLAY_DEVICE_DISPLAY_NAME};
    t.micon[4] = {"idHash", s_idhash};
    t.micon[5] = {"dev", MIPLAY_DEV};
    t.micon[6] = {"sec", MIPLAY_SEC};
    t.micon[7] = {"appsData", s_appsdata};
    t.micon[8] = {"mac", s_mac_b64};

    // TS 用 Unix epoch 毫秒，手机据此判断宣告新鲜度。
    struct timeval tv_now;
    gettimeofday(&tv_now, nullptr);
    const long long ts_ms =
        (long long)tv_now.tv_sec * 1000LL + tv_now.tv_usec / 1000;
    snprintf(t.ts_str, sizeof(t.ts_str), "%lld", ts_ms);

    char encoded_ip[32];
    encode_debug_ip(miplay_get_my_ipv4(), encoded_ip, sizeof(encoded_ip));
    snprintf(t.debug_info, sizeof(t.debug_info), "{msg:%s, ifname:STA, v4:%s}",
             debug_prefix, encoded_ip);

    t.lyra[0] = {"AppData", s_lyra_appdata};
    t.lyra[1] = {"MediumType", "8192"};
    t.lyra[2] = {"CH", "0"};
    t.lyra[3] = {"DebugInfo", t.debug_info};
    t.lyra[4] = {"TS", t.ts_str};
}

const uint8_t kLyraQname[] = {
    0x0a, '_', 'l', 'y', 'r', 'a', '-', 'm', 'd', 'n', 's',
    0x04, '_', 'u', 'd', 'p',
    0x05, 'l', 'o', 'c', 'a', 'l', 0x00,
};
const uint8_t kMiconQname[] = {
    0x0b, '_', 'm', 'i', '-', 'c', 'o', 'n', 'n', 'e', 'c', 't',
    0x04, '_', 'u', 'd', 'p',
    0x05, 'l', 'o', 'c', 'a', 'l', 0x00,
};

void send_announce_pair(int sock, const struct sockaddr_in *dest,
                        const char *debug_prefix)
{
    AnnounceTxt txt;
    fill_announce_txt(txt, debug_prefix);
    const uint32_t ip = miplay_get_my_ipv4();
    uint8_t pkt[768];

    const size_t len1 =
        build_mdns_announce(pkt, sizeof(pkt), kLyraQname, sizeof(kLyraQname),
                            s_device_id, MIPLAY_MDNS_PORT, txt.lyra, 5, ip);
    sendto(sock, pkt, len1, 0, (const struct sockaddr *)dest, sizeof(*dest));

    const size_t len2 =
        build_mdns_announce(pkt, sizeof(pkt), kMiconQname, sizeof(kMiconQname),
                            s_inst_name, MIPLAY_COAP_PORT, txt.micon, 9, ip);
    sendto(sock, pkt, len2, 0, (const struct sockaddr *)dest, sizeof(*dest));
}

void mdns_announce_task(void *)
{
    // 等 mDNS 组件就绪，避免注册未完成就宣告。
    vTaskDelay(pdMS_TO_TICKS(2000));

    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(kTag, "announce socket failed: %d", errno);
        s_announce_task = nullptr;
        miplay_delete_current_task();
        return;
    }
    int ttl = 255;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    struct sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(MIPLAY_MDNS_PORT);
    dest.sin_addr.s_addr = inet_addr(MIPLAY_MDNS_GROUP);

    // 启动突发 3 轮，让冷缓存的手机立刻发现。
    for (int round = 1; round <= 3 && s_running; round++) {
        send_announce_pair(sock, &dest, "announcement");
        ESP_LOGI(kTag, "mDNS announce burst %d/3", round);
        if (round < 3) {
            vTaskDelay(pdMS_TO_TICKS(180));
        }
    }

    // 加入 5353 组播组并监听查询。为什么 esp-mdns 之外还要这个应答器：
    // 手机(设备互联)对两个服务的 browse 是 QU(单播期望)查询，esp-mdns 对
    // QU 只发单播应答。实测(2026-09-12 组播监听)同网段其他能被发现的设备
    // 对同一查询全部回组播应答，HyperOS 的组播 browse socket 因此总能收到
    // 它们；对单播应答是否采纳取决于其 socket 生命周期，实测 36 次 QU 查询
    // 后手机仍未发现本设备。对齐参考实现(收到查询→应答)这里补发组播应答，
    // 与 esp-mdns 的单播应答并存——两者内容一致，手机按 RFC 6762 去重。
    int listen_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (listen_sock >= 0) {
        int reuse = 1;
        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        struct sockaddr_in bind_addr = {};
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_port = htons(MIPLAY_MDNS_PORT);
        bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(listen_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
            ESP_LOGW(kTag, "5353 listen bind failed: %d (query replies degraded)", errno);
            close(listen_sock);
            listen_sock = -1;
        } else {
            struct ip_mreq mreq = {};
            mreq.imr_multiaddr.s_addr = inet_addr(MIPLAY_MDNS_GROUP);
            mreq.imr_interface.s_addr = miplay_get_my_ipv4();
            if (setsockopt(listen_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq,
                           sizeof(mreq)) != 0) {
                ESP_LOGW(kTag, "5353 join failed: %d", errno);
                close(listen_sock);
                listen_sock = -1;
            } else {
                unsigned char loop = 0;
                setsockopt(listen_sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop,
                           sizeof(loop));
                struct timeval tv = {};
                tv.tv_usec = 100000;
                setsockopt(listen_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                ESP_LOGI(kTag, "5353 query responder active (multicast replies)");
            }
        }
    }

    // 周期刷新，防止手机侧 mDNS 缓存过期后丢失设备。
    while (s_running) {
        // 短步长等待 + 监听查询：两者在同一循环里，查询应答延迟 < 100ms。
        for (int i = 0; i < 300 && s_running; i++) {
            if (listen_sock >= 0) {
                uint8_t rx[768];
                struct sockaddr_in src = {};
                socklen_t srclen = sizeof(src);
                const int n = recvfrom(listen_sock, rx, sizeof(rx), 0,
                                       (struct sockaddr *)&src, &srclen);
                if (n >= 12 && !(rx[2] & 0x80)) {
                    // 只处理查询(QR=0)。匹配两个服务的 QNAME 前缀：
                    // "_lyra-mdns._udp.local" 或 "_mi-connect._udp.local"。
                    bool is_lyra = false, is_micon = false;
                    for (int p = 12; p + 21 <= n; p++) {
                        if (!is_lyra &&
                            memcmp(rx + p, kLyraQname + 1, sizeof(kLyraQname) - 2) == 0) {
                            is_lyra = true;
                        }
                        if (!is_micon &&
                            memcmp(rx + p, kMiconQname + 1, sizeof(kMiconQname) - 2) == 0) {
                            is_micon = true;
                        }
                    }
                    if (is_lyra || is_micon) {
                        // 应答 = announce 布局的完整权威应答(PTR+SRV+TXT+A) +
                        // 回显查询 TX ID(RFC 6762 6.7 对 legacy 查询的要求)。
                        // 目的地固定组播：HyperOS 的 browse socket 挂在组播上，
                        // 单播副本已由 esp-mdns 发送，这里补组播可见性。
                        AnnounceTxt txt;
                        fill_announce_txt(txt, "reply");
                        const uint32_t ip = miplay_get_my_ipv4();
                        uint8_t pkt[768];
                        if (is_lyra) {
                            const size_t len = build_mdns_announce(
                                pkt, sizeof(pkt), kLyraQname, sizeof(kLyraQname),
                                s_device_id, MIPLAY_MDNS_PORT, txt.lyra, 5, ip);
                            pkt[0] = rx[0];
                            pkt[1] = rx[1];
                            sendto(listen_sock, pkt, len, 0,
                                   (struct sockaddr *)&dest, sizeof(dest));
                        }
                        if (is_micon) {
                            const size_t len = build_mdns_announce(
                                pkt, sizeof(pkt), kMiconQname, sizeof(kMiconQname),
                                s_inst_name, MIPLAY_COAP_PORT, txt.micon, 9, ip);
                            pkt[0] = rx[0];
                            pkt[1] = rx[1];
                            sendto(listen_sock, pkt, len, 0,
                                   (struct sockaddr *)&dest, sizeof(dest));
                        }
                        ESP_LOGI(kTag, "query reply: %s from %u.%u.%u.%u (txid=%02X%02X)",
                                 is_micon ? "_mi-connect" : "_lyra-mdns",
                                 (unsigned)(src.sin_addr.s_addr & 0xFF),
                                 (unsigned)((src.sin_addr.s_addr >> 8) & 0xFF),
                                 (unsigned)((src.sin_addr.s_addr >> 16) & 0xFF),
                                 (unsigned)((src.sin_addr.s_addr >> 24) & 0xFF),
                                 rx[0], rx[1]);
                    }
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!s_running) {
            break;
        }
        send_announce_pair(sock, &dest, "refresh");
        ESP_LOGD(kTag, "mDNS announce refresh");
    }

    if (listen_sock >= 0) {
        close(listen_sock);
    }
    close(sock);
    s_announce_task = nullptr;
    miplay_delete_current_task();
}

// 主动扫描附近的手机。
//
// 发现是双向的：只靠设备广播、等手机来连是不够的——手机侧的 MiPlay
// 适配器看到设备也主动扫它，才会把这条 Lyra 记录纳入候选。参考实现
// 里这个任务叫 miplay_scan_task，承担的就是这一半。
//
// 判定手机的方式：查 _lyra-mdns 得 AppData，其 base64 首字节解码出
// 前 6 位为 0x02（0x00 0x40 0x02 结构里的 type 字段）。
void miplay_scan_task(void *)
{
    while (s_running) {
        // 10 秒一轮：手机侧缓存 TTL 是 120 秒，这个频率足够在
        // 手机冷启动扫描窗口内被发现，又不会打满多播。
        for (int i = 0; i < 100 && s_running; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!s_running) {
            break;
        }

        mdns_result_t *results = nullptr;
        if (mdns_query_ptr(MIPLAY_LYRA_SERVICE, MIPLAY_LYRA_PROTO, 2000, 10,
                           &results) != ESP_OK ||
            !results) {
            continue;
        }

        static const char kB64[] =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (mdns_result_t *r = results; r; r = r->next) {
            if (!r->addr || r->txt_count == 0) {
                continue;
            }
            const char *appdata = nullptr;
            for (int i = 0; i < r->txt_count; i++) {
                if (r->txt[i].key && strcmp(r->txt[i].key, "AppData") == 0) {
                    appdata = r->txt[i].value;
                    break;
                }
            }
            if (!appdata || strlen(appdata) < 2) {
                continue;
            }
            // 只解前两个 base64 字符：首字节 6 位 + 次字节高 2 位。
            // 目标是判断高 6 位是否等于 0x02（手机类型），不需要完整解码。
            int first_byte = 0;
            for (int i = 0; i < 64; i++) {
                if (kB64[i] == appdata[0]) {
                    first_byte = i << 2;
                    break;
                }
            }
            for (int i = 0; i < 64; i++) {
                if (kB64[i] == appdata[1]) {
                    first_byte |= (i >> 4) & 3;
                    break;
                }
            }
            if (first_byte != 0x02) {
                continue;
            }
            for (mdns_ip_addr_t *a = r->addr; a; a = a->next) {
                if (a->addr.type != ESP_IPADDR_TYPE_V4) {
                    continue;
                }
                const uint32_t ip = a->addr.u_addr.ip4.addr;
                ESP_LOGI(kTag, "found phone: %s at %u.%u.%u.%u:%u",
                         r->instance_name ? r->instance_name : "?",
                         (unsigned)(ip & 0xFF), (unsigned)((ip >> 8) & 0xFF),
                         (unsigned)((ip >> 16) & 0xFF),
                         (unsigned)((ip >> 24) & 0xFF), r->port);
                break;
            }
        }
        mdns_query_results_free(results);
    }
    s_scan_task = nullptr;
    miplay_delete_current_task();
}

}  // namespace

// ── mDNS 服务注册与发现层入口 ──

esp_err_t miplay_register_mdns_services(void)
{
    // ESP-IDF v5.5 起 mdns 是托管组件，没有任何隐式初始化：不先 mdns_init()
    // 则 s_server 为空，后面 hostname_set / service_add 全部返回
    // ESP_ERR_INVALID_ARG，且错误码相同难以定位。必须显式初始化。
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "mdns_init failed: %s", esp_err_to_name(err));
        return err;
    }

    // hostname 用 device_id，确保 SRV target 有对应的 A 记录。
    // 必须检查返回值：失败时 s_server->hostname 保持 NULL，会让后续所有
    // service_add 报同一个 INVALID_ARG，把真正原因埋掉。
    err = mdns_hostname_set(s_device_id);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "mdns_hostname_set(%s) failed: %s", s_device_id,
                 esp_err_to_name(err));
        return err;
    }

    // _mi-connect._udp：主发现服务，TXT 各字段含义见 fill_announce_txt。
    mdns_txt_item_t micon_txt[] = {
        {"version", MIPLAY_VERSION}, {"apps", "[5]"}, {"flags", "CgE="},
        {"name", MIPLAY_DEVICE_DISPLAY_NAME}, {"idHash", s_idhash},
        {"dev", MIPLAY_DEV}, {"sec", MIPLAY_SEC},
        {"appsData", s_appsdata}, {"mac", s_mac_b64},
    };
    err = mdns_service_add_for_host(
        s_inst_name, MIPLAY_MICON_SERVICE, MIPLAY_MICON_PROTO, s_device_id,
        MIPLAY_COAP_PORT, micon_txt,
        sizeof(micon_txt) / sizeof(micon_txt[0]));
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "_mi-connect failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(kTag, "Registered _mi-connect._udp dev=%s", MIPLAY_DEV);
    }

    // _lyra-mdns._udp：供支持 Lyra 的手机走 Lyra 遥控路径。
    {
        AnnounceTxt txt;
        fill_announce_txt(txt, "reply");
        err = mdns_service_add_for_host(
            s_device_id, MIPLAY_LYRA_SERVICE, MIPLAY_LYRA_PROTO, s_device_id,
            MIPLAY_MDNS_PORT, txt.lyra,
            sizeof(txt.lyra) / sizeof(txt.lyra[0]));
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "_lyra-mdns failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(kTag, "Registered _lyra-mdns._udp");
        }
    }

    // _miplay_lan 不注册：参考实现已弃用它，注册会与 _mi-connect 在
    // 手机的设备列表里产生重复行。
    return ESP_OK;
}

void miplay_start_discovery(void)
{
    snprintf(s_lan_hostname, sizeof(s_lan_hostname), "miplay-%02X%02X%02X",
             s_mac[3], s_mac[4], s_mac[5]);

    // LAN 与 announce 都放 core 0：core 1 在 RLCD_CLOCK 上被 UI 任务占用，
    // 且这两个任务只做轻量收发。
    if (miplay_create_task(lan_task, "miplay_lan", 12288, nullptr, 3,
                           (void **)&s_lan_task, 0) != pdPASS) {
        ESP_LOGE(kTag, "failed to create LAN task");
    }
    if (miplay_create_task(mdns_announce_task, "mdns_announce", 12288, nullptr, 3,
                           (void **)&s_announce_task, 0) != pdPASS) {
        ESP_LOGE(kTag, "failed to create announce task");
    }
    // 主动扫描手机。放 core 1：它每 10 秒才跑一轮，且 mdns_query_ptr 会
    // 阻塞等 2 秒应答，不适合与 UI 抢 core 0 的时隙。
    if (miplay_create_task(miplay_scan_task, "miplay_scan", 12288, nullptr, 3,
                           (void **)&s_scan_task, 1) != pdPASS) {
        ESP_LOGW(kTag, "failed to create scan task (phone discovery degraded)");
    }
}
