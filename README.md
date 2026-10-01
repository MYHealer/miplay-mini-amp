# MiPlay Mini Amp — 小米妙播最小耳放

**版本 v1.0**（2026-09-28，实机验证：配网 + 投屏 + 出声全通）

把 ESP32-S3 SuperMini + GY-PCM5102 做成一个**只做小米妙播接收播放**的耳放。

```
小米手机「妙播」
      │  Wi-Fi 局域网
      ▼
ESP32-S3 SuperMini ──I2S──▶ GY-PCM5102 ──▶ 3.5mm 耳机
```

**做**：网页配网（强制门户）→ 被手机发现 → 接收投屏音频 → 解码 → I2S 输出 → 耳机出声。外加一颗状态 LED。

**不做**：屏幕、DLNA、airkan 遥控、歌词封面、OTA、按键。

> Wi-Fi 凭据**不在编译期硬编码**：首次开机无凭据即开 AP + 强制门户，
> 手机连上自动弹配网页，选网络填密码，连上后 AP 自动关闭（见 §3.1）。

---

## 1. 硬件

| 项 | 规格 |
|---|---|
| 主控 | **ESP32-S3FH4R2**（4MB Flash + 2MB Quad SPI PSRAM，均片内） |
| 音频 | **GY-PCM5102** 模块（紫色小板，带 3.5mm 座） |
| 供电 | USB-C（SuperMini 板载） |

### 接线

| 信号 | GPIO | 说明 |
|---|---|---|
| BCLK | **8** | 位时钟 → PCM5102 的 BCK |
| LRCK | **7** | 左右声道时钟 → LRCK |
| DIN | **6** | 音频数据 → DIN |
| MCLK | **9** | → 模块的 SCK 焊盘。**默认已启用**，见下方说明 |
| GND | GND | **必须共地** |
| 3V3 | 3V3 | 模块供电 |

**MCLK 说明（重要）**

GY-PCM5102 在市面有两种流通版本，外观无法区分：

- **SCK 焊盘悬空** → **必须**由主控喂 MCLK，否则完全无声。**本工程按这一版接线**，
  `MIPLAY_I2S_ENABLE_MCLK` 默认已打开，GPIO9 接模块 SCK。
- **SCK 焊盘已接地** → PCM5102A 用内部 PLL 从 BCK 锁相，**不需要 MCLK**。
  此时若仍把 GPIO9 接上去，等于输出脚对地短路，必须关闭
  `MIPLAY_I2S_ENABLE_MCLK` 并断开这条线。

**怎么判断手上是哪一版**：万用表量 SCK 焊盘对 GND —— 开路是悬空版（保持默认），
接近 0Ω 是接地版（关掉 MCLK 开关、不接这根线）。

**避开**：GPIO19/20（USB-JTAG，日志就靠它）、GPIO0/3/45/46（strapping）、
GPIO26-32（片内 Flash/PSRAM 占用）。当前用的 6-9 全部落在 P2 区（无限制）。

---

## 2. 构建

必须用 **PowerShell**（Git Bash 会踩 MSYSTEM 污染，`idf.py` 秒退）。

⚠ `idf.py` **不在 PATH 上**：`esp_env.ps1` 只导出 `$PYTHON` 与 `$IDF_PY` 两个变量，
必须显式 `& $PYTHON $IDF_PY <args>` 调用。

```powershell
cd E:\ESP
. .\esp_env.ps1
cd E:\ESP\miplay-mini-amp
& $PYTHON $IDF_PY set-target esp32s3
& $PYTHON $IDF_PY build
& $PYTHON $IDF_PY -p COMx flash monitor
```

构建目录**必须是纯 ASCII 路径** —— ccache 4.x 遇到中文路径必崩，
所以本项目放在 `E:\ESP\miplay-mini-amp` 而不是中文目录。

**看构建日志**：PowerShell 的 `>` 重定向产出 UTF-16LE（字符间夹空格，无法读），
要落盘用 `| Out-File -Encoding utf8`。

---

## 3. 配置

`idf.py menuconfig` → `MiPlay Mini Amp`：

### 3.1 Wi-Fi 配网（无编译期凭据）

**凭据不在固件里。** 同一份固件刷到任意一台设备都能用，家庭 Wi-Fi 密码
不进源码、不进 sdkconfig。

状态机：

```
开机
 ├─ NVS 里有凭据 ──▶ 连 STA
 │                     ├─ 成功 ──▶ 正常待机（蓝灯），AP 不开
 │                     └─ 失败 ×MIPLAY_WIFI_MAX_RETRY ──▶ 回落开 AP
 └─ NVS 里没凭据 ──▶ 直接开 AP
                        │
                   AP + 强制门户（橙灯）
                        │  手机连上自动弹配网页
                        │  选网络 → 填密码 → 提交
                        ▼
                   阻塞试连 15s
                     ├─ 成功 ──▶ 存 NVS → 关 AP → 进正常待机
                     └─ 失败 ──▶ 页面提示重试，AP 继续开着
```

| 配置项 | 说明 |
|---|---|
| `MIPLAY_WIFI_MAX_RETRY` | 用已存凭据连失败几次后回落开 AP。范围 **1–20，默认 3** |

**刻意不设 0（无限重连）**：那样设备在换路由器或改密码之后会永远沉默——
没有屏幕，用户只能看到灯一直闪橙色，却没有任何途径改配置。留几次重试是为了
容忍路由器重启这类瞬时故障，重试完仍失败就必须把配置入口还给用户。

**AP 是开放热点**（无密码），SSID 形如 `MiPlay-Amp-XXXX`（XXXX 取 MAC 后两字节）。
开放是有意的：配网阶段还没有可用的密码可验，加一个"默认密码"只是多一道
用户会忘的门。AP 只活到配网成功为止。

手机与设备必须**同一网段**，否则 mDNS/UDP 发现不到。

### 3.2 配网页与强制门户

手机连上 AP 后应当**自动弹出**"此网络需要登录"窗口。这个"自动"需要两个
服务同时到位，缺一不可：

| 服务 | 作用 | 缺了会怎样 |
|---|---|---|
| **DNS 劫持**（UDP 53） | 把所有 A 记录查询都答成 AP 自身 IP（`*` 通配） | 手机探测域名（`connectivitycheck.gstatic.com` 等）**解析失败**，探测流程当场结束，压根不向 192.168.4.1 发 HTTP 请求 |
| **HTTP 302 重定向** | 探测 URL 与任何未注册路径都 302 回配网页 | 手机把 AP 当普通网络放行，不弹窗 |

**只做 302 不做 DNS 劫持是最常见的半成品**——表现正是"AP 有，但门户不弹"。

实现要点（都写在 `web_prov.cpp` 注释里）：

- DNS 组件取自 IDF 官方 `examples/protocols/http_server/captive_portal/components/dns_server/`，
  `diff -rq` 核对为**字节一致**，未改动。
- `esp_http_server` 的 `httpd_uri_match_wildcard()` **只认尾部 `*`/`?`**，
  所以探测 URL（`/generate_204`、`/hotspot-detect.html`、`/ncsi.txt`…）必须逐条注册，
  不能靠通配兜住；再叠一层 `HTTPD_404_NOT_FOUND` err handler 兜住未来新增的探测路径。
- **302 必须带响应体**。官方示例明确注明 iOS 需要响应里有内容才认定"此网络有门户"，
  只给空 302 它会静默放行。
- 配网页**零外部引用**（字体/图标/JS 全内联、`fetch` 全用相对路径），
  所以"所有域名都指向本机"不会破坏页面自身渲染。

页面本身（`main/prov_page.h`）只调用 `/api/scan`、`/api/connect`、`/api/info`
三个相对路径接口，风格仿 <https://mimo.xiaomi.com/>。

### 3.3 其他

| 配置项 | 默认 | 说明 |
|---|---|---|
| `MIPLAY_I2S_BCLK_GPIO` | 8 | |
| `MIPLAY_I2S_LRCK_GPIO` | 7 | |
| `MIPLAY_I2S_DIN_GPIO` | 6 | |
| `MIPLAY_I2S_ENABLE_MCLK` | **y** | 本工程按 SCK 悬空版接线。若模块 SCK 已接地必须关掉 |
| `MIPLAY_I2S_MCLK_GPIO` | 9 | 接模块 SCK 焊盘 |
| `MIPLAY_DEFAULT_VOLUME` | 70 | PCM5102A 无 I2C 口，音量是软件缩放 |
| `MIPLAY_LED_ENABLE` | y | 板子没灯就关掉 |
| `MIPLAY_LED_GPIO` | 48 | SuperMini 板载 WS2812 |

### 3.4 分区表（`partitions.csv`）

无 OTA，单 factory 分区：

| 分区 | 偏移 | 大小 |
|---|---|---|
| `nvs` | 0x9000 | 24 KB |
| `phy_init` | 0xF000 | 4 KB |
| `factory`（app） | 0x10000 | **2.94 MB** |
| `coredump` | 0x300000 | 64 KB |

app 给到 2.94 MB 而非更小：`esp_aac_dec` + mbedtls + Wi-Fi/lwIP 的静态占用不小，
留足余量比事后改分区表省事（改分区表要擦全片）。合计约 3.06 MB / 4 MB。

**⚠ `CONFIG_PARTITION_TABLE_CUSTOM=y` 必须显式打开**，否则 `partitions.csv`
形同不存在——IDF 会用它内置的 `partitions_singleapp_coredump.csv`（factory 只有 1 MB），
构建末尾报 `app partition is too small for binary ... overflow 0x136f0`。
**溢出量与二进制大小恰好差 1 MB**，这个数字关系就是"分区表用错"的判据。
已写入 `sdkconfig.defaults`。

### 3.5 内存要点

**必须 `CONFIG_SPIRAM_MODE_QUAD=y`。** FH4R2 的片内 PSRAM 是 Quad SPI，
照抄参考工程的 `OCT` 会启动即 panic（`Virtual address not enough for PSRAM!`）。
第 1 步验证就会暴露这个问题。

**`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y` 已打开。** 这是必须的：
`miplay_session.cpp` 的 `s_encrypt_buf`（256 KB）标了 `EXT_RAM_BSS_ATTR`，而该宏
**只在打开此 Kconfig 时才展开**，否则悄悄留 0 字节，把 256 KB 全压在内部 SRAM，
构建期直接 `region 'dram0_0_seg' overflowed by 48280 bytes`。

打开前已逐项核实安全性：该缓冲是纯软件中转（`memcpy` → `mbedtls_aes_crypt_cbc`
→ `memcpy`），**无 DMA、无 ISR 访问**；且同函数里的中间缓冲 `padded` 本来就已
用 `heap_caps_calloc(..., MALLOC_CAP_SPIRAM)` 分配在 PSRAM。见 §7。

---

## 4. 状态 LED

板载 WS2812（GPIO48）。下表取自 `status_led.cpp` 的 `state_color()` /
`brightness_phase()`，是**实际行为**而非设计意图：

| 状态 | 颜色 | 亮度行为 |
|---|---|---|
| `STATUS_BOOT` 启动中 | **白** `255,255,255` | 呼吸（三角波，周期 2s） |
| `STATUS_WIFI` Wi-Fi 连接中 | **橙** `255,140,0` | 闪烁（半亮/全暗，周期 1s） |
| `STATUS_AP` 配网热点已开 | **橙** `255,140,0` | 常亮，压到 60% |
| `STATUS_IDLE` 已联网待机 | **蓝** `0,120,255` | 低亮常亮（系数 35） |
| `STATUS_CONNECTED` 手机已连接 | **青** `0,255,255` | 常亮 |
| `STATUS_PLAYING` 播放中 | **紫** `180,0,255` | 常亮 |
| `STATUS_ERROR` 错误 | **红** `255,0,0` | 快闪（周期 0.5s） |

**用户语义**（原始需求）：**橙 = 连不上网，蓝 = 已联网**。
橙色同时覆盖"正在连"与"回落开热点"两态——两者对用户是同一件事：
还没上网，需要（或可能需要）配网。蓝色只在真正拿到 IP 之后出现。

不用蓝色表示"还没定论"，因为蓝已专属"已联网"；启动期用中性的白。
AP 态压到 60% 亮度是刻意的：配网要等很久，全亮的橙在暗处刺眼。

**全局亮度**：`status_led.cpp` 的 `kBrightnessPct`（当前 **50**）是唯一的总亮度旋钮。
它乘在各状态自己的相位系数之上，所以调它等于同时调暗全部状态，
**状态之间的相对明暗关系与呼吸/闪烁波形完全不变**。
觉得刺眼就改这一个常量——不要逐个改 RGB 基色，那样会破坏配色意图。

> 亮度实现上有个小细节：先乘后除，`phase × kBrightnessPct / 100`。
> 若反过来先算 `phase / 100`，IDLE 态的系数 35 会直接塌成 0（整数除法），
> 灯变成完全不亮。

**⚠ 一个曾长期存在的静默故障**：`led_strip` v3 的
`led_strip_rmt_dev.c:130-139` 只接受 `num_components` 为 3 或 4，
**没有任何默认回退**。本工程原先零初始化该字段，于是
`led_strip_new_rmt_device` 恒返回 `invalid number of color components: 0`，
状态灯**从未亮过**——而外层把失败降级成一行 W 日志继续跑，
表现是"灯不亮"而不是崩溃，极易被误判成硬件问题。现已显式赋值
`bytes_per_color = 1` / `num_components = 3`。

---

## 5. 验证顺序

分五步，每步都有明确判据。**卡在哪一步就只查那一步**。

| 步骤 | 动作 | 通过判据 |
|---|---|---|
| 1 | 编译烧录看日志 | 串口出现 `esp_psram: Adding pool of 1778K`、`status led on GPIO48` |
| 2 | 配网 | 手机连开放热点 `MiPlay-Amp-XXXX`，**自动弹**配网页；选网络填密码后串口出现 `online as "..."` |
| 3 | 手机妙播里找设备 | 设备出现在手机列表中（`miplay_remote: started: ...`） |
| 4 | 连接并播放 | 串口打出 `AAC: rate=48000 ch=2` |
| 5 | 听声音 | 耳机出声 |

**第 1 步就能验出 PSRAM 配错**——FH4R2 是 Quad PSRAM，
若误配 Octal 会立刻 panic：`spiram: Virtual address not enough for PSRAM!`

排错指引：

- **第 2 步失败（有 AP 但不弹门户）** → 八成是 DNS 劫持没起来。
  查串口有没有 `DNS hijack up: all A queries -> AP IP`；没有就是
  `dns_server` 组件没链进来。门户的这两半是**与**关系，缺一不弹，见 §3.2。
  另外浏览器缓存可能留着旧页面，先强刷一次。
- **第 2 步失败（连不上 STA）** → 页面提示"连接失败：密码不对，或这个网络不在范围内"。
  查 `scan served: N ap(s)` 看设备到底扫到了什么（扫到 0 条多半是 5GHz 或距离太远）。
  注意 ESP32-S3 **不支持 5GHz**。
- **第 3 步失败** → 查 mDNS 注册日志与 UDP 5355 应答；确认手机与设备同网段；
  确认路由器没开 AP 隔离（这是最常见的坑）。
- **第 4 步失败** → 查 RTSP 握手日志、`SetMirrorKey` 是否下发。
- **第 5 步失败** → 依次查：① 音量是否被置 0；② MCLK 线是否接对
  （默认已输出 MCLK，若模块 SCK 实为接地版必须关掉开关并断开连线）；
  ③ BCK/LRCK/DIN 是否接反。

---

## 6. 目录结构

```
miplay-mini-amp/
├── CMakeLists.txt
├── sdkconfig.defaults       # Quad PSRAM / 外部 BSS 段 / 自定义分区表
├── partitions.csv           # 单 factory，无 OTA
├── DESIGN.md                # 完整设计文档（决策依据）
├── README.md                # 本文
├── components/
│   ├── miplay_remote/       # 协议层，从 RLCD_CLOCK 移植（见 §7）
│   └── dns_server/          # 强制门户的 DNS 劫持半边
│                            #   取自 IDF 官方 captive_portal 示例，字节一致
└── main/
    ├── CMakeLists.txt
    ├── idf_component.yml
    ├── Kconfig.projbuild
    ├── app_main.cpp         # 启动编排 + Wi-Fi 状态机（连网/开热点）
    ├── wifi_sta.cpp/.h      # STA 连接 + AP 开关 + 扫描
    ├── wifi_creds.cpp/.h    # NVS 凭据读写（配网成功后落盘）
    ├── web_prov.cpp/.h      # 配网 HTTP 服务 + 强制门户（302 + DNS 劫持）
    ├── prov_page.h          # 内嵌配网页（HTML/CSS/JS 全内联，仿 mimo.xiaomi.com）
    ├── audio_i2s.cpp/.h     # ★ PCM5102A 输出层（I2S + 采样率自适应 + 音量）
    ├── app_volume.cpp/.h    # 软件音量（int32 累加器防溢出）
    └── status_led.cpp/.h    # WS2812 状态指示
```

> 全部写成 `.cpp`：用到 `namespace {}`、`constexpr`、`nullptr` 与匿名 union
> 成员访问，都是 C++ 特性。`app_main` 保留 `extern "C"` 入口。
> `dns_server` 是唯一保留 `.c` 的组件——原样复制，不改上游。

---

## 7. 与上游的差异

协议层 `components/miplay_remote/` 照搬自：

```
E:\ESP\ESP32-S3-RLCD-4.2-main\ESP32-S3-RLCD-4.2-main\RLCD_CLOCK\components\miplay_remote\
```

选它是因为该组件已从单体 `esp-miply-1.85touch/components/miplay`（205KB）
剥离为独立组件，**零 LVGL、零 GMF、零 airkan**，且协议实现已在 RLCD 硬件上跑通。

上游硬件是 **N16R8**（16MB Flash + 8MB Octal PSRAM），本机是 **FH4R2**
（4MB Flash + 2MB Quad PSRAM），所以内存与 Flash 配置全部重做。

下表的改动清单已用 `diff -rq` 对上游目录逐个核对过：**共 4 个文件有差异**，
其余 13 个与上游字节一致。任何后续修改都应先跑一次这个 diff 确认范围。

### 已改（共 4 处，均附理由写入源码注释）

| 文件 | 改动 | 理由 |
|---|---|---|
| `miplay_session.h` | 会话栈 **256KB → 64KB** | 实测该任务栈上最大对象是 `body[512]`，调用链深度估算 <8KB，64KB 留 8 倍余量 |
| `miplay_media.cpp:1672` | rtsp 任务退出改用 `miplay_delete_current_task()` | **修 bug**：原为 `vTaskDeleteWithCaps()`，绕过追踪表清槽 → 槽位泄漏 → 后续任务 PSRAM 栈永不释放 |
| `miplay_remote_internal.h` | `MIPLAY_DEVICE_DISPLAY_NAME` → `"小米妙播耳放"` | 设备在手机妙播列表里的显示名。硬约束只有两条：不含 `.`（会破坏 DNS label 压缩指针）、编码后 ≤63 字节（`dns_push_label` 用 `strlen` 截断） |
| `miplay_proto.h` | 仅补注释（值未改） | 记录「256KB 为何不能下调」的核查结论 |

### 已核查但**刻意不改**（原计划要改，证据不支持）

| 项 | 原计划 | 实际 | 理由 |
|---|---|---|---|
| 单帧上限 `MIPLAY_MAX_FRAME_PAYLOAD` | 256KB → 64KB | **保持 256KB** | 上游明确警告「缩小它会在收到大帧时截断并失步」；该值同时是 `frame_hdr_plausible()` 校验上界与 `dec_buf` 分配依据，改小即损坏协议 |
| 最大控制会话 `MIPLAY_MAX_CONTROL_SESSIONS` | 3 → 2 | **保持 3** | 会话表 `s_sessions` 标了 `EXT_RAM_BSS_ATTR`，在本配置（已开外部 BSS 段）下落在 PSRAM，单条 ~200B，省 200B 无收益 |
| 任务追踪槽 `kMaxTrackedTasks` | 8 → 16 | **保持 8** | 8 槽对应最多 3 会话 + 2 瞬态 = 5 个活跃项。注意 `client_task` 是用 `vTaskDelete` 自删的，槽位要等 idle 任务回收后才真正释放，快速重连会留下数个残留项——8 槽是合理上界，加到 16 无收益 |
| RTSP `lib_version: audio-display-release2.1` | 当作"屏幕残留"删掉 | **保留原值** | 该字段位于 RTSP `OPTIONS` 请求内，与 `Require: org.wfa.wfd1.0`、`authMsg:` 同处——是手机端解析的 **WFD 协议指纹**，不是可自由改的版本号 |

### 内存占用

| | 上游 N16R8 | 本方案 FH4R2 |
|---|---|---|
| 接收缓冲 | 256 KB | 256 KB（不可降） |
| 会话任务栈 | 256KB × 3 = 768 KB | **64KB × 3 = 192 KB** |
| rtsp 任务栈 | 48 KB | 48 KB |
| 单帧解密拷贝 | ≤256 KB | ≤256 KB |
| 其他（RTSP/PCM/ADTS 缓冲、发现任务） | ~30 KB | ~30 KB |
| **峰值合计** | **≈ 1.36 MB / 8 MB** | **≈ 780 KB / 2 MB** |

峰值按「单会话活跃」估（同一时刻通常只有一个控制会话在收大帧）。
2MB 可用 PSRAM 下余量约 1.2MB，供 Wi-Fi/lwIP 与音频 DMA 使用。

**⚠ 关于 `s_encrypt_buf`（256 KB）—— 已搬进 PSRAM**

`miplay_session.cpp` 的 `s_encrypt_buf` 标了 `EXT_RAM_BSS_ATTR`，但该宏
（`esp_attr.h`）**只在 `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y` 时
才展开成 `_SECTION_ATTR_IMPL(".ext_ram.bss", ...)`，否则展开为「空」**——
变量会静默留在内部 `.bss`，编译器不报任何警告。

本项目已打开该 Kconfig。在此之前，256 KB 全压在内部 SRAM 上，构建期报：

```
region 'dram0_0_seg' overflowed by 48280 bytes
```

（内部 SRAM 共 512 KB，被这块吃掉一半。）

**打开前做过的三项安全核实**：

1. **无 DMA**：`s_encrypt_buf` 是纯软件中转。数据路径
   （`miplay_proto.cpp` 的 `miplay_safety_encrypt()`）就是
   `memcpy` → `mbedtls_aes_crypt_cbc` → `memcpy`，全程 CPU 访问。
2. **无 ISR**：只在任务上下文使用。
3. **旁证**：同一函数里的中间缓冲 `padded` 本来就已经是
   `heap_caps_calloc(1, padded_len, MALLOC_CAP_SPIRAM)`——分配在 PSRAM
   的同级用途早已在跑，把源缓冲也放进 PSRAM 不引入新风险。

另注：打开该 Kconfig 会**连带把 lwip / net80211 / pp / bt 的 `.bss` 也自动
放进 PSRAM**（其 Kconfig 里有 `select ESP_ALLOW_BSS_SEG_EXTERNAL_MEMORY`）。
实测启动日志 `esp_psram: Adding pool of 1778K of PSRAM memory to heap allocator`
说明 PSRAM 空间充裕，这个连带效果是净收益（内部 SRAM 更宽松）。

**⚠ 符号名撞车（已修）**：`wifi_sta_disconnect` 这个名字在
`libnet80211.a(ieee80211_ioctl.o)` 里是 IDF 自己的全局符号（同名出现 9 次），
链接期必报 `multiple definition`。已改名为 `wifi_sta_drop()`。
用 `nm` 扫过全部 130 个 IDF 静态库与我们的 86 个全局符号，
**这是唯一的碰撞**——所以是改名而非加 `static`。

---

## 8. 已知风险 / 待实测确认

| 项 | 状态 | 说明 |
|---|---|---|
| Quad PSRAM 带宽低于 Octal | 待实测 | 256KB 缓冲拷贝较慢；必要时降缓冲尺寸 |
| GY-PCM5102 SCK 版本 | **已验证** | v1.0 实机按**悬空版**接线（GPIO13 输出 MCLK）出声正常。若换到 SCK 已接地的模块，必须关 `MIPLAY_I2S_ENABLE_MCLK` 并断线 |
| **I2S 写阻塞会掐住网络收包** | **已验证不成立** | v1.0 实机连续播放无卡顿、无丢包。见下方说明 |
| 封面帧尺寸未知 | 待实测 | 上游握手期实测最大帧 7257B，未覆盖播放期封面推送。本工程**不用封面**，仅在收到时记一行日志，所以该风险对本工程不构成影响 |
| 长时间播放稳定性 | 待实测 | v1.0 只做了短时播放验证，未做小时级老化 |

### 关于 I2S 阻塞（重要设计决策）

上游 RLCD 在解码与 I2S 之间插了一个 32KB ring buffer + 独立输出任务，
理由是「解码线程不再被 I2S 阻塞」。

**本工程刻意不加这一层**，依据：

1. 音频解码与 I2S 写**在同一个任务循环里**（`miplay_rtsp_run`，即 `miplay_rtsp`
   任务，48KB 栈）。`i2s_channel_write()` 只在 DMA 满时阻塞，DMA 配置可容纳
   约 30ms 音频，构成天然的流量控制与抖动缓冲。
2. UDP socket 接收缓冲本身能容纳数秒压缩音频，突发包不会丢。
3. 最小系统的目标是「少活动部件」；ring buffer 会引入任务生命周期、
   PSRAM 静态缓冲归还等一整类新问题。

**升级路径**：若实测出现卡顿或丢包，`audio_i2s_write()` 是唯一接缝 ——
在它前面插 ring buffer + 输出任务即可，`miplay_remote` 侧无需改动。

> 注：`media_receive_task`（`miplay_media.cpp:580`）经核查是**死代码**（零调用点）。
> 真正执行「收包→解码→写 I2S」的是 `miplay_rtsp_run`（`decrypt_ts_media` 在 1495 行的
> 调用点）。结论不变。`DESIGN.md` §3.4 已就地订正。

### 关于组件头文件注释

`components/miplay_remote/include/miplay_remote.h` 顶部注释写着
「剥离了 RTSP 投屏、音频管线」——**这句已过时**。该组件实际已包含
完整的 RTSP/RTP/TS/AES/AAC 投屏播放链路，只是由控制口的
`SetMirrorKey(0x006C)` **事件驱动**启动，而非 `miplay_remote_start()` 显式启动。

为保持「原样照搬」的可追溯性，未改动该注释，以本 README 为准。

---

## 9. 环境

| 项 | 值 |
|---|---|
| ESP-IDF | v5.5.4（`E:\ESP\.espressif\v5.5.4\esp-idf`） |
| 环境脚本 | `E:\ESP\esp_env.ps1`（**必须 PowerShell**） |
| 构建目录 | 必须纯 ASCII 路径（ccache 4.x + 中文路径必崩） |
