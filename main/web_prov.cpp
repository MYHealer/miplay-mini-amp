#include "web_prov.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
// ⚠ 必须在 esp_netif.h 之后：上游 dns_server.h 用了 esp_ip4_addr_t 却没自己
// include 它（头文件不自洽），依赖调用方先把 esp_netif.h 拉进来。
// 官方 captive_portal 示例的 main.c 就是这个顺序。组件是原样复制的，不改它。
#include "dns_server.h"

#include "prov_page.h"
#include "wifi_creds.h"
#include "wifi_sta.h"

namespace {

constexpr char kTag[] = "web_prov";

// 与页面文案"最多等 15 秒"必须一致——两边不一致时，用户会在设备其实还在
// 尝试连接的时候就看到"请求中断"。
constexpr int kConnectTimeoutMs = 15000;

// 扫描结果上限。每条 wifi_ap_record_t 约 70 字节，40 条 ≈ 3KB，放堆上。
constexpr int kMaxAps = 40;

// 请求体上限。ssid(32) + pass(64) 的 JSON 展开留足余量即可。
constexpr size_t kMaxBody = 320;

httpd_handle_t s_server = nullptr;
web_prov_connected_fn s_on_connected = nullptr;

// DNS 劫持服务器。强制门户的另一半——没有它，手机探测用的域名根本解析不出来，
// 请求永远不会到达本 HTTP 服务器（见文件末尾 start_portal_dns 的说明）。
dns_server_handle_t s_dns = nullptr;

// 门户跳转目标。在 start 时算一次——服务器活着的时候 AP 的 IP 不会变。
char s_portal_url[32] = "http://192.168.4.1/";

// ── 响应辅助 ──

// 发一个非 JSON 的错误。这是刻意的：页面用 .json() 解析，解析失败会走
// catch 分支显示"重试一次"。所以错误路径必须给非 JSON 体，否则页面会
// 把一个 500 当成空结果静默渲染成"没有扫到网络"。
esp_err_t send_plain_error(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, msg);
}

// 发 {"ok":..,"msg":..}。用 cJSON 而不是 snprintf：SSID 和错误文案都可能
// 含引号或反斜杠，手拼 JSON 迟早拼出个解析不了的响应。
esp_err_t send_result(httpd_req_t *req, bool ok, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) {
        return send_plain_error(req, "500 Internal Server Error", "out of memory");
    }
    cJSON_AddBoolToObject(root, "ok", ok);
    if (msg != nullptr) {
        cJSON_AddStringToObject(root, "msg", msg);
    }
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (out == nullptr) {
        return send_plain_error(req, "500 Internal Server Error", "out of memory");
    }
    httpd_resp_set_type(req, "application/json");
    const esp_err_t err = httpd_resp_sendstr(req, out);
    cJSON_free(out);
    return err;
}

// ── 配网页 ──

esp_err_t on_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    // 无缓存：配网页改一次就得立刻生效，而设备重启后浏览器缓存里那份
    // 很可能还是上一个固件的旧版。
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, miplay::kProvisionPage);
}

// ── /api/scan ──

esp_err_t on_scan(httpd_req_t *req)
{
    auto *recs = static_cast<wifi_ap_record_t *>(
        calloc(kMaxAps, sizeof(wifi_ap_record_t)));
    if (recs == nullptr) {
        return send_plain_error(req, "500 Internal Server Error", "out of memory");
    }

    const int n = wifi_scan(recs, kMaxAps);
    if (n < 0) {
        free(recs);
        return send_plain_error(req, "500 Internal Server Error", "scan failed");
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *aps = root != nullptr ? cJSON_AddArrayToObject(root, "aps") : nullptr;
    if (aps == nullptr) {
        cJSON_Delete(root);
        free(recs);
        return send_plain_error(req, "500 Internal Server Error", "out of memory");
    }

    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        if (o == nullptr) {
            continue;  // 少一条比整个响应失败强
        }
        cJSON_AddStringToObject(o, "ssid",
                                reinterpret_cast<const char *>(recs[i].ssid));
        cJSON_AddNumberToObject(o, "rssi", recs[i].rssi);
        cJSON_AddNumberToObject(o, "auth", static_cast<int>(recs[i].authmode));
        cJSON_AddItemToArray(aps, o);
    }
    free(recs);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (out == nullptr) {
        return send_plain_error(req, "500 Internal Server Error", "out of memory");
    }

    // 日志里留一份完整结果：配网失败时串口是唯一能看出"到底扫到了什么"的地方。
    ESP_LOGI(kTag, "scan served: %d ap(s)", n);
    httpd_resp_set_type(req, "application/json");
    const esp_err_t err = httpd_resp_sendstr(req, out);
    cJSON_free(out);
    return err;
}

// ── /api/connect ──

// 把请求体读进 out（含 NUL）。超过容量或读失败返回 false。
bool read_body(httpd_req_t *req, char *out, size_t cap)
{
    if (req->content_len == 0 || req->content_len >= cap) {
        return false;
    }
    size_t got = 0;
    while (got < req->content_len) {
        const int r = httpd_req_recv(req, out + got, req->content_len - got);
        if (r <= 0) {
            return false;
        }
        got += static_cast<size_t>(r);
    }
    out[got] = '\0';
    return true;
}

esp_err_t on_connect(httpd_req_t *req)
{
    char body[kMaxBody];
    if (!read_body(req, body, sizeof(body))) {
        return send_result(req, false, "请求格式错误");
    }

    cJSON *root = cJSON_Parse(body);
    if (root == nullptr) {
        return send_result(req, false, "请求格式错误");
    }

    const cJSON *jssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *jpass = cJSON_GetObjectItemCaseSensitive(root, "pass");
    if (!cJSON_IsString(jssid) || jssid->valuestring == nullptr ||
        jssid->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return send_result(req, false, "请先选择一个网络");
    }

    // 页面已经 trim 过，但服务端不能信客户端：超长会被 esp_wifi_set_config()
    // 静默截断，设备"启动成功"却永远连不上一个不存在的 AP，日志里毫无异常。
    if (strlen(jssid->valuestring) >= 32) {
        cJSON_Delete(root);
        return send_result(req, false, "网络名称过长");
    }
    const char *pass = (cJSON_IsString(jpass) && jpass->valuestring != nullptr)
                           ? jpass->valuestring
                           : "";
    if (strlen(pass) >= 64) {
        cJSON_Delete(root);
        return send_result(req, false, "密码过长（最多 64 字符）");
    }

    char ssid[33] = {};
    char password[64] = {};
    strncpy(ssid, jssid->valuestring, sizeof(ssid) - 1);
    strncpy(password, pass, sizeof(password) - 1);
    cJSON_Delete(root);

    ESP_LOGI(kTag, "connect request: ssid=\"%s\" pass=%u chars", ssid,
             static_cast<unsigned>(strlen(password)));

    // 阻塞式试连。这 15 秒里本服务器无法响应其他请求——可以接受，
    // 因为页面此刻正停在这个 POST 上等结果。
    if (!wifi_sta_connect(ssid, password, kConnectTimeoutMs)) {
        return send_result(req, false, "连接失败：密码不对，或这个网络不在范围内");
    }

    // 验证通过才落盘。密码打错一次不该毁掉设备里已经能用的旧配置——
    // 这是"先验证再提交"，不是"先写再试"。
    if (!wifi_creds_save(ssid, password)) {
        // 已经连上了，只是存不下来（NVS 满/坏）。告诉用户但仍算成功：
        // 这次能用，重启后要重配。
        ESP_LOGW(kTag, "connected but failed to persist credentials");
    }

    // 先把响应发完，再通知上层。反过来的话手机可能收不到这个响应，
    // 页面会显示"请求中断"，虽然设备其实已经连上了。
    const esp_err_t err = send_result(req, true, "已连接");

    if (s_on_connected != nullptr) {
        // 回调在 httpd 任务上下文执行，所以实现方必须做两件事：立刻返回
        // （页面在等这个响应），以及把状态迁移甩给别的任务去做——
        // 在这里拆 AP 会让这个 socket 连同 netif 一起悬空。
        s_on_connected(ssid);
    }
    return err;
}

// ── /api/info ──

esp_err_t on_info(httpd_req_t *req)
{
    // 从驱动里读回真实 SSID，而不是把 wifi_ap_start() 里的拼装逻辑抄一遍：
    // 抄一遍就等于有了两份可能不一致的真相。
    wifi_config_t cfg = {};
    char ap_ssid[33] = "";
    if (esp_wifi_get_config(WIFI_IF_AP, &cfg) == ESP_OK) {
        // ap.ssid 是 char[32] 且未必以 NUL 结尾（ssid_len 才是权威长度），
        // 用 %.32s 限定读取宽度，既不会越界也不会漏掉没有终止符的情况。
        snprintf(ap_ssid, sizeof(ap_ssid), "%.32s",
                 reinterpret_cast<const char *>(cfg.ap.ssid));
    }

    cJSON *root = cJSON_CreateObject();
    if (root == nullptr) {
        return send_plain_error(req, "500 Internal Server Error", "out of memory");
    }
    cJSON_AddStringToObject(root, "ap", ap_ssid);
    cJSON_AddBoolToObject(root, "connected", wifi_is_connected());
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (out == nullptr) {
        return send_plain_error(req, "500 Internal Server Error", "out of memory");
    }
    httpd_resp_set_type(req, "application/json");
    const esp_err_t err = httpd_resp_sendstr(req, out);
    cJSON_free(out);
    return err;
}

// ── 强制门户 ──

// 各平台的"我连上网了吗"探测地址。命中任意一条即 302 回配网页，
// 手机/电脑就会自动弹出"此网络需要登录"的窗口。
//
// 必须逐条精确注册，不能靠通配兜住：esp_http_server 的
// httpd_uri_match_wildcard() 只认**尾部**星号（/api/* 可以，*.html 不行）。
constexpr const char *kProbeUris[] = {
    "/generate_204",               // Android 7+
    "/gen_204",                    // Android 旧版
    "/hotspot-detect.html",        // Apple iOS / macOS
    "/library/test/success.html",  // Apple 旧版
    "/ncsi.txt",                   // Windows NCSI
    "/connecttest.txt",            // Windows 10+
    "/redirect",                   // Windows 探测重定向页
    "/canonical.html",             // Firefox
    "/success.txt",                // Firefox 旧版
    "/kindle-wifi/wifistub.html",  // Kindle
};

esp_err_t on_portal_probe(httpd_req_t *req)
{
    // 用绝对 URL 而不是 "/"：探测请求的 Host 头五花八门（有时是 IP，
    // 有时是设备名），绝对地址能保证跳到用户真正能打开的那个地址。
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", s_portal_url);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    // ⚠ 必须带响应体，不能发空。官方 captive_portal 示例明确注明：
    // iOS 需要响应里有内容才认定"此网络有门户"，只给 302 它会静默放行。
    return httpd_resp_send(req, "Redirecting to provisioning page",
                           HTTPD_RESP_USE_STRLEN);
}

// 404 兜底：把任何未注册路径都 302 回配网页。
//
// 这比只兜 /api/* 覆盖面大得多——手机探测的 URL 会随系统版本变
// （/generate_204、/hotspot-detect.html、/ncsi.txt……），逐条列举必然漏。
// 官方示例正是用 404 handler 做这件事，这里保持一致。
esp_err_t on_not_found(httpd_req_t *req, httpd_err_code_t err)
{
    ESP_LOGI(kTag, "404 -> portal: %s", req->uri);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", s_portal_url);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, "Redirecting to provisioning page",
                           HTTPD_RESP_USE_STRLEN);
}

// /api/* 兜底。真正存在的端点都注册在它之前——匹配循环按注册顺序取
// 第一个命中的 handler，所以顺序就是优先级。
esp_err_t on_api_not_found(httpd_req_t *req)
{
    ESP_LOGW(kTag, "unhandled api path: %s", req->uri);
    return send_result(req, false, "没有这个接口");
}

// ── 启动 / 停止 ──

esp_err_t register_all(httpd_handle_t server)
{
    // 页面与接口。顺序在 /api/* 之前，否则会被兜底吞掉。
    const httpd_uri_t page = {
        .uri = "/", .method = HTTP_GET, .handler = on_page, .user_ctx = nullptr};
    const httpd_uri_t scan = {
        .uri = "/api/scan", .method = HTTP_GET, .handler = on_scan, .user_ctx = nullptr};
    const httpd_uri_t connect = {
        .uri = "/api/connect", .method = HTTP_POST, .handler = on_connect, .user_ctx = nullptr};
    const httpd_uri_t info = {
        .uri = "/api/info", .method = HTTP_GET, .handler = on_info, .user_ctx = nullptr};

    const httpd_uri_t *fixed[] = {&page, &scan, &connect, &info};
    for (const httpd_uri_t *h : fixed) {
        const esp_err_t err = httpd_register_uri_handler(server, h);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "register %s failed: %s", h->uri, esp_err_to_name(err));
            return err;
        }
    }

    for (const char *uri : kProbeUris) {
        const httpd_uri_t probe = {
            .uri = uri, .method = HTTP_GET, .handler = on_portal_probe, .user_ctx = nullptr};
        const esp_err_t err = httpd_register_uri_handler(server, &probe);
        if (err != ESP_OK) {
            ESP_LOGE(kTag, "register %s failed: %s", uri, esp_err_to_name(err));
            return err;
        }
    }

    const httpd_uri_t api_fallback = {
        .uri = "/api/*", .method = HTTP_GET, .handler = on_api_not_found, .user_ctx = nullptr};
    esp_err_t err = httpd_register_uri_handler(server, &api_fallback);
    if (err != ESP_OK) {
        return err;
    }

    // 兜底之上的兜底：任何未注册路径（探测 URL 会随手机系统版本变）都 302 回门户。
    // 优先级低于上面所有具体 handler——err handler 只在没人接手时才跑。
    return httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, on_not_found);
}

// 门户跳转目标：查 AP netif 的真实地址，查不到才退回 IDF 的默认网关。
void resolve_portal_url()
{
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ip = {};
    if (ap != nullptr && esp_netif_get_ip_info(ap, &ip) == ESP_OK && ip.ip.addr != 0) {
        snprintf(s_portal_url, sizeof(s_portal_url), "http://" IPSTR "/",
                 IP2STR(&ip.ip));
    }
}

// 起 DNS 劫持。这是强制门户**必需的另一半**，不是可选项。
//
// 原理：手机的"我连上网了吗"探测请求打的是域名（connectivitycheck.gstatic.com、
// captive.apple.com、www.msftconnecttest.com…）。AP 若没有 DNS 应答器，
// 这些名字解析失败，手机直接判定"无互联网"，探测流程当场结束——
// 压根不会向 192.168.4.1 发任何 HTTP 请求，我们的 302 也就无从生效。
//
// 所以：DNS 把**所有** A 记录查询都应答为 AP 自身 IP（"*" 通配），
// 手机于是把探测请求发到 192.168.4.1，此时 HTTP 层的 302 才被触发。
//
// 注意本页零外部引用（字体/图标/JS 全内联、fetch 全用相对路径），
// 所以"所有域名都指向本机"不会破坏配网页自身的渲染。
bool start_portal_dns()
{
    // 非 const：start_dns_server() 的形参是 dns_server_config_t*（非 const），
    // 而 DNS_SERVER_CONFIG_SINGLE 展开成的是初始化列表。官方示例同样用非 const。
    dns_server_config_t cfg = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    s_dns = start_dns_server(&cfg);
    if (s_dns == nullptr) {
        ESP_LOGE(kTag, "DNS hijack start failed — captive portal will not pop up");
        return false;
    }
    ESP_LOGI(kTag, "DNS hijack up: all A queries -> AP IP");
    return true;
}

}  // namespace

esp_err_t web_prov_start(web_prov_connected_fn on_connected)
{
    if (s_server != nullptr) {
        return ESP_OK;
    }

    resolve_portal_url();
    s_on_connected = on_connected;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    // 4 个页面/接口 + 10 条探测 URL + 1 条 /api/* 兜底 = 15，留些余量。
    cfg.max_uri_handlers = 20;
    // 必须开：esp_http_server 默认用简单字符串匹配，那样 /api/* 和 / 上的
    // 通配语义都不成立。
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    // 手机探测门户时会短时间开一堆连接，不回收就会顶满 socket 数。
    cfg.lru_purge_enable = true;
    // 默认 4096 偏紧：handler 里有 320B 请求体、cJSON 解析和日志格式化。
    cfg.stack_size = 6144;

    const esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "httpd_start failed: %s", esp_err_to_name(err));
        s_server = nullptr;
        return err;
    }

    // 注册在 start 之后、对外可服务之前——顺序仍是"全齐了才开门"：
    // 中途注册失败的路径会直接把服务器停掉，不会留下一个半残的门户
    // （手机点了"登录"却打到 404，比压根没有门户更让人困惑）。
    const esp_err_t rerr = register_all(s_server);
    if (rerr != ESP_OK) {
        httpd_stop(s_server);
        s_server = nullptr;
        return rerr;
    }

    // DNS 劫持。失败不致命：配网页仍能用（用户手动开浏览器输 192.168.4.1），
    // 只是不会自动弹窗。所以记警告但不回滚。
    start_portal_dns();

    ESP_LOGI(kTag, "provisioning server up at %s", s_portal_url);
    return ESP_OK;
}

void web_prov_stop(void)
{
    // 先停 DNS：它比 HTTP 服务器更"外围"，先摘掉它可以让残留的探测请求
    // 直接失败，而不是打到正在拆的服务器上。
    if (s_dns != nullptr) {
        stop_dns_server(s_dns);
        s_dns = nullptr;
    }
    if (s_server == nullptr) {
        return;
    }
    httpd_stop(s_server);
    s_server = nullptr;
    s_on_connected = nullptr;
    ESP_LOGI(kTag, "provisioning server down");
}