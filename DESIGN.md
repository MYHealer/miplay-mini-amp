# 小米妙播耳放最小系统 — 设计文档

- 日期：2026-09-27（初稿）/ 2026-09-28（**v1.0 实机验证后订正**）
- 状态：**v1.0 已实机验证通过**——配网、强制门户、投屏、出声全通
- 目标目录：`E:\ESP\miplay-mini-amp`

> **文档状态说明**：本文是设计期的决策记录，含若干「初稿判断 → 实测订正」的痕迹，
> 这些痕迹刻意保留（便于追溯为什么某个选择是错的）。
> **当前实际行为以 `README.md` 为准**，本文负责解释"为什么"。
>
> v1.0 相对初稿的**实质变化**有三处，都改变了架构而非参数：
> 1. **Wi-Fi 凭据从编译期硬编码改为运行时网页配网 + 强制门户**（§1、§4.3、新增 §3.6）
> 2. **`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` 从"刻意不开"改为"必须开"**（§4.1、§5）
> 3. **状态 LED 配色重做**，橙/蓝承载"连不上网/已联网"的用户语义（§6）

---

## 1. 目标

做一个**只做 MiPlay 接收播放**的最小系统：

```
手机（小米妙播 App）
   │  Wi-Fi 局域网
   ▼
ESP32-S3 SuperMini  ←→  GY-PCM5102 模块  →  3.5mm 耳机
```

**功能边界（明确不做）**：无屏幕、无 DLNA、无 airkan 遥控、无歌词封面、无 OTA、无按键。

**核心功能**：首次开机网页配网（强制门户）→ 被小米妙播发现 → 接收投屏音频流 → 解码
→ 从 I2S 输出到 PCM5102A → 耳机出声。外加一个状态 LED。

**Wi-Fi 凭据不在固件里**（v1.0 改动，见 §3.6）：同一份固件刷到任意一台设备都能用，
家庭 Wi-Fi 密码不进源码、不进 sdkconfig。代价是多了一个配网子系统与它的强制门户。

---

## 2. 硬件

### 2.1 主控

| 项 | 值 |
|---|---|
| 型号 | **ESP32-S3FH4R2** |
| Flash | 4 MB（Quad SPI，片内） |
| PSRAM | **2 MB（Quad SPI，片内）** |
| 可用 GPIO | 1-13 已授权使用；FH4R2 的 Quad PSRAM **不占用 GPIO33-37**（Octal 才占） |

**关键约束**：FH4R2 的片内 PSRAM 是 **Quad SPI**，必须 `CONFIG_SPIRAM_MODE_QUAD=y`。
若照抄参考工程的 `CONFIG_SPIRAM_MODE_OCT=y`，启动会 panic：

```
E (232) spiram: Virtual address not enough for PSRAM!
```

依据：乐鑫硬件设计指南 in-package 引脚映射表 —— `ESP32-S3R2/ESP32-S3FH4R2 | In-Package PSRAM (2 MB, Quad SPI)`；
以及 esp-faq 条目明确要求 ESP32-S3R2 必须把 PSRAM Mode 设为 Quad。

### 2.2 音频模块

**GY-PCM5102**（紫色小板，带 3.5mm 耳机座）

- 纯 DAC，**无 I2C 控制口** → 音量只能软件处理
- 模块自带 XSMT 软静音电路，无需我们控制
- 模块自带输出低通滤波与隔直电容

### 2.3 接线

| 信号 | GPIO | 备注 |
|---|---|---|
| BCLK | 12 | 位时钟 |
| LRCK | 10 | 左右声道时钟 / WS |
| DIN | 11 | 音频数据 |
| MCLK | 13 | → 模块 SCK 焊盘。**默认启用**，Kconfig 可关 |
| LED | 48 | 板载 WS2812，Kconfig 可改/关 |

**避开**：GPIO19/20（USB-JTAG）、GPIO0/3/45/46（strapping）、GPIO26-32（片内 flash/PSRAM）。
本工程用的 10-13 全部落在 P2 区（无限制），与上述任何保留脚不冲突。

**关于 MCLK**：GY-PCM5102 的 SCK 焊盘若悬空，PCM5102A 拿不到时基，**完全无声**，
必须由主控喂 MCLK；若焊盘已接地，芯片用内部 PLL 从 BCK 锁相，无需外部 MCLK，
此时把主控引脚接上去等于输出脚对地短路，必须断开。

市面两种版本都流通，外观无法区分，用万用表量 SCK 对 GND 阻值即可判定
（开路 = 悬空版，接近 0Ω = 接地版）。

**本工程按悬空版接线**，故 `MIPLAY_I2S_ENABLE_MCLK` 默认打开（GPIO13 → SCK）。
好处是采样率切换（44100↔48000）不再触发 PLL 重锁，消除了对应的 pop 声；
代价是多占一根 GPIO、多一根连线。

---

## 3. 软件架构

### 3.1 复用策略：复制 `miplay_remote`（仅 4 处改动）

**来源**：`E:\ESP\ESP32-S3-RLCD-4.2-main\ESP32-S3-RLCD-4.2-main\RLCD_CLOCK\components\miplay_remote\`

选择理由：该组件已从 `esp-miply-1.85touch/components/miplay`（205KB 单体，绑死 GMF 音频管线 + DLNA + 歌词 + LVGL）
剥离为独立组件，**零 LVGL、零 GMF、零 airkan**，公开 API 为回调式设计，协议层与输出层天然解耦。
协议实现已在 RLCD 硬件上跑通，复制可规避全部协议风险。

**复制文件清单**（17 个）：
```
components/miplay_remote/
├── CMakeLists.txt
├── idf_component.yml
├── include/
│   └── miplay_remote.h          ← 唯一公开头文件
├── miplay_control.cpp/.h        ← TCP 8899 控制会话、AES-GCM、OPack 编解码
├── miplay_discovery.cpp         ← mDNS 注册 + UDP 5355 DNS-SD 应答
├── miplay_identity.cpp          ← 设备身份（efuse 派生 device_id）
├── miplay_media.cpp/.h          ← RTSP 信令 + RTP 收流 + TS 解密 + AAC 解码
├── miplay_proto.cpp/.h          ← 协议常量与帧格式
├── miplay_remote.cpp            ← 启动编排 + 反控命令
├── miplay_remote_internal.h
├── miplay_safety.cpp/.h         ← 安全值校验
└── miplay_session.cpp/.h        ← 会话生命周期与锁
```

**实际改动**（`diff -rq` 核对，共 4 个文件）：`miplay_session.h`（栈 256KB→64KB）、
`miplay_media.cpp`（修 `vTaskDeleteWithCaps` 槽位泄漏）、`miplay_remote_internal.h`
（设备显示名）、`miplay_proto.h`（仅补注释）。逐条理由见 **README §7**。

**沿用的依赖声明**（来自其 CMakeLists.txt，未改动）：

- `REQUIRES`：`mdns lwip esp_netif esp_wifi mbedtls esp_timer nvs_flash`
- `PRIV_REQUIRES`：`efuse esp_audio_codec`
- `idf_component.yml`：`espressif/mdns: ">=1.0.0"`

> 初稿声称「原样复制，不改一行」——**不成立**，已订正。实际改了 4 个文件，
> 其中 1 个是真 bug 修复、1 个是硬件适配（栈尺寸）、1 个是需求（显示名）。
> 保留这张清单的目的是让后续维护者能一眼看出「哪些是上游行为、哪些是我们引入的」。

**不引入**：`ExternLib/`（SensorLib / codec_board / esp_codec_dev 全不需要）、`airkan-sdk`、`lyrics_fetch`、`app_bsp/`、`port_bsp/`、LVGL 全家桶。

### 3.2 目录结构

```
E:\ESP\miplay-mini-amp\
├── CMakeLists.txt              # 顶层工程定义
├── sdkconfig.defaults          # ★ Quad PSRAM + 外部BSS段 + 自定义分区表
├── partitions.csv              # nvs + phy_init + factory + coredump
├── README.md                   # 中文使用说明（当前行为以此为准）
├── DESIGN.md                   # 本文档
├── components/
│   ├── miplay_remote/          # ← 复制自上游，仅 4 处改动（见 README §7）
│   └── dns_server/             # ← 复制自 IDF 官方 captive_portal 示例，字节一致
│       ├── CMakeLists.txt
│       ├── dns_server.c
│       └── include/dns_server.h
└── main/
    ├── CMakeLists.txt
    ├── idf_component.yml       # mdns + esp_audio_codec + led_strip
    ├── Kconfig.projbuild       # 重试次数 + 引脚 + 音量 + LED（**无凭据项**）
    ├── app_main.cpp            # 启动编排 + Wi-Fi 状态机（连网/开热点）
    ├── wifi_sta.cpp/.h         # STA 连接 + AP 开关 + 扫描
    ├── wifi_creds.cpp/.h       # NVS 凭据读写
    ├── web_prov.cpp/.h         # 配网 HTTP 服务 + 强制门户（302 + DNS 劫持）
    ├── prov_page.h             # 内嵌配网页（HTML/CSS/JS 全内联，仿 mimo.xiaomi.com）
    ├── audio_i2s.cpp/.h        # ★ PCM5102A 输出层
    ├── app_volume.cpp/.h       # 软件音量缩放
    └── status_led.cpp/.h       # WS2812 状态指示
```

> 实现时全部写成 `.cpp`：这些文件用了 `namespace {}`、`constexpr`、`nullptr`，
> 以及 `led_strip_config_t` 的匿名 union 成员访问，都是 C++ 特性。
> `app_main` 保留 `extern "C"` 入口，IDF 照常识别。
> 唯一的 `.c` 是 `dns_server`——原样复制上游，不改。

`miplay_remote`（17 个文件，复用）+ `dns_server`（3 个文件，复用）之外，
新写代码 **约 2300 行**（14 个源文件 1904 行 + 内嵌配网页 384 行）。

**main/idf_component.yml**：

```yaml
dependencies:
  idf:
    version: '>=5.0'
  # AAC 解码（miplay_media.cpp 用 esp_aac_dec API）
  espressif/esp_audio_codec: '^2.4.1'
  # 板载 WS2812（RMT 后端，ESP32-S3 支持）
  espressif/led_strip: '^3.0.0'
  # ESP-IDF v5.5 起 mdns 已迁出内置组件
  espressif/mdns: ">=1.0.0"
```

> 注意：`miplay_remote` 组件自身的 `CMakeLists.txt` 已声明
> `PRIV_REQUIRES: efuse esp_audio_codec`，AAC 解码依赖由它自带，此处重复声明
> 仅为版本锁定清晰。`led_strip` 若关闭状态 LED（`MIPLAY_LED_ENABLE=n`）
> 可不引入，但保留声明不影响构建产物。

### 3.3 数据流

```
【配网阶段】手机浏览器 → 开放 AP → DNS 劫持(53) + HTTP(80) 强制门户
   │  /api/scan 扫描列表 → /api/connect 提交凭据 → 阻塞试连 15s
   │  成功 → 写 NVS → 关 AP           （详见 §3.6）
   ▼
【播放阶段】
手机 App
   │ ① mDNS 注册 _mi-connect._udp / _lyra-mdns._udp
   │    ② UDP 5355 DNS-SD 应答（手机单播查询）
   ▼
miplay_discovery.cpp
   │ 手机获知设备 IP
   ▼
miplay_control.cpp ── TCP 8899 控制会话
   │   AES-GCM 加密，OPack 编解码
   │   CMD_OPEN_DEVICE → 解析 wfd://host:port?mirrorMode=1
   ▼
miplay_media.cpp ── 独立 RTSP 任务
   │   RTSP 握手 → RTP 收包 → TS 解复用 → AES-CTR 解密 → AAC 帧
   │   esp_aac_dec 解码
   │
   │   ▼ 回调 miplay_write_speaker_fn(const int16_t*, size_t, int)
   ▼
★ audio_i2s.cpp ── ① 采样率自适应 ② 软件音量缩放 ③ 写 I2S DMA
   ▼
PCM5102A ──► 3.5mm 耳机
```

### 3.4 输出层接口

`audio_i2s.cpp` 暴露的回调签名必须与 `miplay_remote.h` 中的 typedef 完全一致：

```c
typedef int (*miplay_write_speaker_fn)(const int16_t *stereo_samples,
                                       size_t frame_count, int sample_rate);

// 注册时机：app_main 中 wifi_sta 启动前
miplay_media_set_speaker_callback(audio_i2s_write);
```

内部职责：

1. **采样率自适应**
2. **软件音量**：int32 累加器做 `sample * vol / 100`，避免 int16 中间溢出
3. **阻塞写**：`i2s_channel_write()` 带超时；DMA 满时自然阻塞，即天然流量控制

**采样率切换的正确序列（易错点）**

`i2s_channel_reconfig_std_clock()` 在通道已启动时**必须**先 disable。
IDF v5.5.4 头文件 `driver/i2s_std.h:322` 注释：

> this function won't change the state. `i2s_channel_disable` should be called
> before calling this function if I2S has started.

因此重配时钟的正确顺序是：

```c
i2s_channel_disable(tx);                       // ① 必须先停
i2s_channel_reconfig_std_clock(tx, &clk_cfg);  // ② 改采样率
i2s_channel_enable(tx);                        // ③ 再启
```

若省略第 ① 步，重配静默失败，输出仍是旧采样率 → 声音变调。

**不使用 `esp_codec_dev` 的理由**：该组件是"有 I2C 控制口的 codec"抽象层。
PCM5102A 只需 `driver/i2s_std.h`，多引一层抽象是负担。

**为什么直接写、不加 ring buffer（关键决策）**

> 勘误（实现后核对）：`media_receive_task`（第 580 行起）是**死代码**，全文件零调用点。
> 真正在跑的是 `miplay_rtsp_run()`，它由 `miplay_media_set_stream_key()` 经
> `miplay_create_task(miplay_rtsp_task_wrapper, ...)` 创建（48 KB 栈）。
> `decrypt_ts_media()` 的调用点只有两处：744 行（死路径）与 **1495 行（活路径，在
> `miplay_rtsp_run` 内）**。下面的"同任务收包+解码+写 I2S"结论对 `miplay_rtsp_run` 同样成立，
> 只是任务名与栈大小不同。

`miplay_rtsp_run()` 在**同一个任务、同一个循环**里
同时做三件事：`recv()` 收 RTP 包、`esp_aac_dec_decode`、调 `s_write_speaker`。
因此 **I2S 写阻塞会直接掐住网络收包**。

参考工程（RLCD）为此在解码与 I2S 之间插了一个 32KB ring buffer + 独立输出任务
（`audio_services.cpp:515`），注释写明理由："解码线程不再被 I2S 阻塞"。

**本设计决定不加这一层**，理由：

1. `i2s_channel_write()` 只在 DMA 满时阻塞，不会阻塞整个播放周期。
   按下面配置 DMA 可容纳约 30 ms 音频，构成天然的流量控制与抖动缓冲。
2. UDP socket 接收缓冲本身可容纳数秒压缩音频，突发包不会丢。
3. 稳态下任务每 30 ms 只忙 5 ms 左右（AAC 解码），余量充足。
4. 最小系统的目标是"少活动部件"；ring buffer 引入了任务生命周期、
   PSRAM 静态缓冲归还等一整类新问题（参考实现为清理它写了 30 行代码）。

**DMA 配置**（吸收网络抖动）：

```c
i2s_std_config_t cfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                    I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = { .bclk = BCLK_GPIO, .ws = LRCK_GPIO, .dout = DIN_GPIO,
                  .mclk = MCLK_GPIO_OR_NONE, .invert_flags = {} },
};
// 6 个描述符 × 240 帧 = 1440 帧 ≈ 30 ms @48kHz 立体声
```

**升级路径**：若实测出现卡顿或丢包，`audio_i2s_write()` 是唯一接缝——
在它前面插 ring buffer + 输出任务即可，`miplay_remote` 侧无需改动。

### 3.5 回调注册与语义

`miplay_remote.h` 提供的回调，本工程的**具体实现职责**：

| 回调 | 何时触发 | 本工程实现 |
|---|---|---|
| `miplay_media_set_speaker_callback` | 每解出一帧 PCM | → `audio_i2s_write()` |
| `miplay_media_set_stream_callback` | 媒体流开始 / 结束 | → `audio_i2s_stream_start()` / `_stop()` |
| `miplay_media_set_volume_callback` | 手机调音量 | → `app_volume_set()` / `app_volume_get()` |
| `miplay_remote_set_connection_callback` | 手机建立 / 断开控制会话 | → `status_led_set(STATUS_CONNECTED/IDLE)` |
| `miplay_media_set_pause_callback` | 手机播放 / 暂停 | → `status_led_set(STATUS_PLAYING/CONNECTED)` |
| `miplay_media_set_meta_callback` | 收到歌曲元数据 | **空实现**（无屏幕，仅记日志） |

**回调的调用者（实现后核对，用于判断并发模型）**

| 回调 | 调用点 | 所在任务 |
|---|---|---|
| `stream_start` / `stream_stop` / speaker | `miplay_media.cpp:1313` / `1636` / `1557` | **`miplay_rtsp`**（48 KB 栈，由 `miplay_create_task` 创建） |
| `pause` | `miplay_media.cpp:1887` ← `miplay_control.cpp:1067` | **`miplay_cli` 控制会话任务**（64 KB 栈） |
| `meta` | `miplay_media.cpp:1869` ← `miplay_control.cpp:931/992/1026/1076` | **`miplay_cli`** |
| `connection` | `miplay_control.cpp:52`（`set_connected`，被 `disconnect_cleanup` / `client_loop` / 监听任务停止路径调用） | **控制侧任务**（`miplay_cli` 或控制监听任务） |

**结论**：`app_main.cpp` 里那些回调转发函数会被**两个任务**并发调用。
因此它们各自触碰的状态必须是原子的：

- `status_led.cpp` 的 `s_state` 声明为 `volatile status_led_state_t`，单字写入，无撕裂；
- `app_volume.cpp` 的 `s_volume` 是 `int`，同样单字写入；
- `audio_i2s.cpp` 的 `s_tx` / `s_sample_rate` **只被 `miplay_rtsp` 一个任务触碰**
  （三个回调同源），不存在竞态，无需加锁。

> 这是刻意留下的最小设计：两个跨任务变量都是单字写、单字读的枚举/整数，
> 用 `volatile` 足以。若日后往这些状态里加结构体（比如 LED 调色板、音量渐变曲线），
> 单字保证就失效了，那时必须补临界区——这个前提值得写在注释里提醒后人。

**`stream_start` 的职责**（返回值 `bool`，`false` 表示拒绝本次流）：

1. `i2s_new_channel` + `i2s_channel_init_std_mode`，**以 48000 占位**建通道
2. `i2s_channel_enable()`，记录 `s_sample_rate = 48000`
3. 返回 `true`

> 实现订正：初稿写的是"按首帧采样率创建通道"，实际做不到——`stream_start`
> 回调不带采样率参数，AAC 流的采样率此刻还未知。真实采样率在**首帧 PCM 到达时**
> 由 `audio_i2s_write()` 的第三个参数给出，那里做 disable→reconfig→enable 重配。
> 占位值选 48000 是因为绝大多数投屏音频是 48 kHz，命中时省掉一次重配。

**`stream_stop` 的职责**（返回值 `void`）：

1. `i2s_channel_disable()` → `i2s_channel_del()`，释放通道
2. 复位 `s_sample_rate = 0`，使下次流重建通道

**为什么必须在 stop 里删通道**：AAC 流的采样率在开始前未知，
若通道常驻则首次必须猜采样率；删通道后每次流都用实测值创建，无猜测。

**`stream_start` 失败的处理**：返回 `false` 时 `miplay_media.cpp` 会中止本次流，
手机端表现为投屏立即结束。故失败原因必须打日志（通常是 PSRAM 或 DMA 描述符不足）。

### 3.6 配网子系统（v1.0 新增）

#### 3.6.1 需求与状态机

原始需求：「**连不上网就打开 AP 的 web 后台，能连上就关闭 AP**」。
AP 触发条件（用户确认）：**无凭据 + 连不上回落**；AP 为**开放热点**；页面范围 = **扫描列表 + 选网络**。

```
app_main
  └─ supervisor_task ── 循环
       ├─ connect_with_saved_creds()   ← NVS 有凭据才试
       │    └─ 成功 → go_online()（蓝灯 + miplay_remote_start）
       │
       └─ 失败 → run_provisioning()
            ├─ wifi_ap_start()         开开放热点
            ├─ web_prov_start()        起 HTTP + DNS 劫持
            ├─ 阻塞在 xEventGroupWaitBits(kBitProvisioned, portMAX_DELAY)
            │     ↑ 由 web_prov 的 /api/connect 成功后回调置位
            ├─ vTaskDelay(1000ms)      ← 等 TCP 发送缓冲落地（见 3.6.4）
            ├─ web_prov_stop() / wifi_ap_stop()
            └─ 回顶部，用刚存下的凭据重走 connect_with_saved_creds()
```

**为什么在线保持也是一个 while 循环**：Wi-Fi 掉线没有回调暴露给这一层，
所以用 `kOnlinePollMs = 2000` 周期读 `wifi_is_connected()`。这一个循环同时覆盖了
"路由器重启"和"用户改了密码"两种情况——掉线就回顶部，回顶部就重试，
重试完还失败就自动回落开热点。**不需要两套重试逻辑**，这是本设计最省事的地方。

#### 3.6.2 强制门户 = DNS 劫持 + HTTP 302（两半，缺一不弹）

这是 v1.0 开发中**踩得最久的一个坑**，值得完整记录。

**症状**：AP 起来了，配网页在浏览器手动输 `192.168.4.1` 也能打开，
但手机连上后**不弹**"此网络需要登录"窗口。

**根因**：只做了 HTTP 302，**没做 DNS 劫持**。

手机的"我连上网了吗"探测打的是**域名**（`connectivitycheck.gstatic.com`、
`captive.apple.com`、`www.msftconnecttest.com`…）。AP 若没有 DNS 应答器，
这些名字**解析失败**，手机直接判定"无互联网"，探测流程当场结束——
**压根不会向 192.168.4.1 发任何 HTTP 请求**，我们的 302 也就无从生效。

```
缺 DNS 劫持：  手机 ──DNS 查询──▶ ✗ 解析失败 ──▶ 判定无网，结束（不弹窗）
有 DNS 劫持：  手机 ──DNS 查询──▶ 全部答成 192.168.4.1
                    ──HTTP 探测──▶ 302 回配网页 ──▶ 弹窗 ✓
```

**修复**：把 IDF 官方 `examples/protocols/http_server/captive_portal/components/dns_server/`
原样复制为 `components/dns_server/`（`diff -rq` 核对字节一致），
以 `DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF")` 启动——`"*"` 通配表示
**所有 A 记录查询都应答为 AP 自身 IP**。

配网页零外部引用（字体/图标/JS 全内联、`fetch` 全用相对路径）是前提条件：
否则"所有域名都指向本机"会把页面自身的外部资源也劫持掉，页面反而崩。

#### 3.6.3 门户的三个实现细节（都踩过）

| 细节 | 内容 | 不这么做会怎样 |
|---|---|---|
| **探测 URL 逐条注册** | `esp_http_server` 的 `httpd_uri_match_wildcard()` **只认尾部 `*`/`?`**（已读 `httpd_uri.c:24` 确认）。`/api/*` 可以，`*.html` 不行。所以 10 条已知探测路径（`/generate_204`、`/hotspot-detect.html`、`/ncsi.txt`…）必须逐条注册 | 探测请求打不到 302，手机静默放行 |
| **叠一层 404 err handler** | `httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, on_not_found)` 兜住所有未注册路径 | 探测 URL 会随手机系统版本变，逐条列举必然漏。官方示例正是用 404 handler 做兜底 |
| **302 必须带响应体** | 用 `httpd_resp_send(req, "Redirecting...", HTTPD_RESP_USE_STRLEN)` 而不是空 302 | 官方示例明确注明：**iOS 需要响应里有内容**才认定"此网络有门户"，只给 302 它会静默放行 |

**另一个刻意的设计**：错误响应故意发**非 JSON** 体。配网页用 `.json()` 解析响应，
解析失败会走 `catch` 分支显示"重试一次"。所以 500 类错误必须给非 JSON 体，
否则页面会把一个 500 当成空结果，静默渲染成"没有扫到网络"——
**把一个服务器错误伪装成正常的空扫描结果**，是最难查的一类 bug。

#### 3.6.4 时序上的两个刻意的等待

1. **先发响应，再通知上层**。`/api/connect` 里 `send_result(req, true, "已连接")`
   必须**在** `s_on_connected(ssid)` **之前**调用。反过来的话手机可能收不到这个响应，
   页面会显示"请求中断"，虽然设备其实已经连上了。

2. **拆 AP 前等 1000ms**（`kHttpDrainMs`）。页面已经收到 200，但 TCP 发送缓冲里
   可能还有没落地的字节；立刻拆 netif 会让手机看到"请求中断"。
   同理，`s_on_connected` 回调运行在 **httpd 任务上下文**，所以它只做
   "记录 SSID + 置事件位"两件不阻塞的事，真正的状态迁移（关热点、起 MiPlay）
   交给 supervisor 任务——**在这里拆 AP 会让这个 socket 连同 netif 一起悬空**。

3. **验证通过才落盘**（`wifi_creds_save` 在 `wifi_sta_connect` 成功之后）。
   密码打错一次不该毁掉设备里已经能用的旧配置——这是"先验证再提交"，
   不是"先写再试"。若连上了但 NVS 存不下（满/坏），只记 W 日志但仍算成功：
   这次能用，重启后要重配。

#### 3.6.5 服务端不信客户端

配网页已经 trim 过 SSID，但服务端**必须重新校验长度**：
超长会被 `esp_wifi_set_config()` **静默截断**，设备"启动成功"却永远连不上
一个不存在的 AP，日志里毫无异常。所以 `/api/connect` 显式检查
`strlen(ssid) < 32` 与 `strlen(pass) < 64`，超了直接返回中文错误文案。

同理 `/api/info` 从驱动读回真实 SSID（`esp_wifi_get_config(WIFI_IF_AP, ...)`）
而不是把 `wifi_ap_start()` 里的拼装逻辑抄一遍——抄一遍就等于有了两份
可能不一致的真相。读回时用 `%.32s` 限定宽度：`ap.ssid` 是 `char[32]` 且
**未必以 NUL 结尾**（`ssid_len` 才是权威长度），限宽既不越界也不漏。

---

## 4. 关键配置

### 4.1 sdkconfig.defaults

```ini
# ── 目标 ──
CONFIG_IDF_TARGET="esp32s3"

# ── ★★ Flash：FH4R2 是 4MB，不是 16MB ──
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
CONFIG_ESPTOOLPY_FLASHFREQ_80M=y

# ── ★★ PSRAM：FH4R2 是 Quad，抄 RLCD 的 OCT 会启动崩溃 ──
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_QUAD=y
CONFIG_SPIRAM_SPEED_80M=y

# 大于此阈值的分配优先走 PSRAM
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=512
# 内部 RAM 保留量，防止小分配挤占
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=49152

# ── ★★ 必须开启：EXT_RAM_BSS_ATTR 只有在这个开关下才生效 ──
# （初稿写"刻意不开"，第一次链接即被打脸，见下方订正说明）
CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y

# ── ★★ 分区表：必须显式选 CUSTOM，否则 partitions.csv 形同不存在 ──
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"

# ── FreeRTOS ──
CONFIG_FREERTOS_HZ=1000
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192

# ── lwIP：MiPlay 并发占用 TCP×3 + RTSP + image + multi + UDP 发现 + mDNS ──
CONFIG_LWIP_MAX_SOCKETS=16

# ── 控制台：SuperMini 板载 USB-C 直连 GPIO19/20（USB-Serial-JTAG）──
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CONFIG_ESP_CONSOLE_SECONDARY_NONE=y

# ── 日志 ──
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y

# ── coredump（调试期有用）──
CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y
CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y
CONFIG_ESP_COREDUMP_CHECKSUM_CRC32=y
```

**刻意不加**：

- `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`
  在同类 MiPlay 工程（1.85C）上实测**净亏 93 KiB**，不盲抄参考工程的调优项。

**★ 订正：`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` 必须开（初稿判断错误）**

初稿写的是"刻意不开"，理由是"协议层其他大缓冲都已显式走 PSRAM，
内部堆是否真吃紧需实测"。**第一次链接就证伪了**：

```
section '.dram0.bss' will not fit in region 'dram0_0_seg'
region 'dram0_0_seg' overflowed by 48280 bytes
```

map 文件把账算得很清楚：`.bss` 合计 **287 KiB**，其中
`s_encrypt_buf` **独占 256.04 KiB**，其余 **1707 个符号全部 ≤ 3.8 KiB**。
单这一个变量就是溢出本身——不是"内部堆吃紧"，是静态 `.bss` 直接装不下。

**为什么初稿会判断错**：`EXT_RAM_BSS_ATTR`（`esp_attr.h:128`）的展开是条件式的：

```c
// 定义（简化）
#if CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
#  define EXT_RAM_BSS_ATTR _SECTION_ATTR_IMPL(".ext_ram.bss", __COUNTER__)
#else
#  define EXT_RAM_BSS_ATTR          // ← 展开为「空」，编译器不报任何警告
#endif
```

也就是说：**源码里看着"已经标了 PSRAM 属性"，实际它仍在内部 SRAM**。
这种"声明了但静默失效"的属性是最难排查的一类——没有警告、没有错误，
只有在链接期以"总量超了"的形式暴露，且看不出是谁占的（要读 map 才知道）。

**开启前做的三项安全核实**（不是默认假设）：

1. **无 DMA**：`s_encrypt_buf` 是 `miplay_safety_encrypt()` 的纯软件中转缓冲，
   数据路径是 `memcpy` → `mbedtls_aes_crypt_cbc` → `memcpy`
   （`miplay_proto.cpp:120-134`），全程 CPU 访问。
2. **无 ISR**：`miplay_session.cpp` 文件内注释已声明会话槽
   "只在任务上下文访问"。
3. **旁证**：同一条路径里的中间缓冲 `padded` **本来就已经是**
   `heap_caps_calloc(1, padded_len, MALLOC_CAP_SPIRAM)`——PSRAM 上跑同级
   用途早已是既成事实，把源缓冲也搬进去不引入新风险。

**连带效果（净收益）**：开启后 `lwip` / `net80211` / `pp` / `bt` 的 `.bss`
也会自动进 PSRAM（`Kconfig.spiram.common` 里的 `select ESP_ALLOW_BSS_SEG_EXTERNAL_MEMORY`）。
实测启动日志 `esp_psram: Adding pool of 1778K of PSRAM memory to heap allocator`
说明 PSRAM 空间充裕，内部 SRAM 因此更宽松。这一条从"代价"变成了"好处"。

### 4.1.1 ★ 订正：`partitions.csv` 需要显式选 CUSTOM

**症状**：

```
Error: app partition is too small for binary miplay-mini-amp.bin
       size 0x1136f0: Part 'factory' @ 0x10000 size 0x100000 (overflow 0x136f0)
```

**根因**：`CONFIG_PARTITION_TABLE_SINGLE_APP=y` 是 IDF 默认值，
它让 IDF 使用**自己内置的** `partitions_singleapp_coredump.csv`（factory 只有 1 MB），
本工程的 `partitions.csv`（factory `0x2F0000` = 2.94 MB）**从未参与编译**。

**判据（记住这个数字关系）**：

```
0x1136f0（二进制大小） − 0x100000（内置 factory 1MB） = 0x136f0（报出的 overflow）
```

**溢出量与二进制大小恰好差 1 MB** —— 看到这个差值就说明分区表用错了，
不用去查代码大小。修复：显式打开 `CONFIG_PARTITION_TABLE_CUSTOM=y`
并指定 `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"`。

> 教训：`sdkconfig.defaults` 里的 `partitions.csv` **文件存在 ≠ 被使用**。
> IDF 的分区表来源由 Kconfig 决定，不取决于目录里有没有那个文件。

### 4.2 partitions.csv

无 OTA，单 factory 分区：

```csv
# Name,   Type, SubType,  Offset,   Size,     Flags
nvs,       data, nvs,      0x9000,   0x6000,
phy_init,  data, phy,      0xF000,   0x1000,
factory,   app,  factory,  0x10000,  0x2F0000,
coredump,  data, coredump, 0x300000, 0x10000,
```

app 分区给到 2.94 MB（不是初稿的 1.94 MB）：esp_aac_dec + mbedtls + Wi-Fi/lwIP
的静态占用不小，留足余量比事后改分区表省事——改分区表要擦全片。
合计约 3.06 MB，4 MB flash 仍有约 0.94 MB 余量。

**⚠ 这张表要被 IDF 采用，前提是 `CONFIG_PARTITION_TABLE_CUSTOM=y`**
（见 §4.1.1）。文件存在不等于被使用。

### 4.1.2 ★ 符号名撞车：`wifi_sta_disconnect`

自写的 `wifi_sta.cpp` 里原本有个函数叫 `wifi_sta_disconnect()`，
链接期报：

```
multiple definition of `wifi_sta_disconnect'
```

**根因**：这个名字在 `libnet80211.a(ieee80211_ioctl.o)` 里是 **IDF 自己的全局符号**
（同名出现 **9 次**）。我们用 C 链接的短通用名，撞进了 Wi-Fi 驱动的保留名域。

**排查方法（值得复用）**：用 `nm` 把全部 **130 个 IDF 静态库**的符号与
我们的 **86 个全局符号**做交集。结果是**只有这一个碰撞**——
所以解法是**改名**（`wifi_sta_disconnect` → `wifi_sta_drop`），而不是加 `static`
（该函数是模块对外 API，不能私有化）。内部调用的仍是 IDF 的
`esp_wifi_disconnect()`，两者不冲突。

> 教训：给 C 链接符号起名时要避开通用动词。`xxx_disconnect` / `xxx_init` /
> `xxx_start` 这类名字在 IDF 里大概率已被占用，加模块前缀（`wifi_sta_`）
> 也可能不够——这里撞的正是加了前缀的名字。

### 4.3 Kconfig.projbuild

```ini
menu "MiPlay Mini Amp"

    menu "Wi-Fi"
        # 凭据不在编译期固化：设备首次上电没有凭据时会开一个开放热点，
        # 手机连上去在 192.168.4.1 填 SSID/密码，验证通过才写进 NVS。
        # 这样同一份固件可以刷到任意一台设备，也不用把家庭 Wi-Fi 密码
        # 留在源码或 sdkconfig 里。
        config MIPLAY_WIFI_MAX_RETRY
            int "STA retries before falling back to AP"
            range 1 20
            default 3
    endmenu

    menu "I2S / PCM5102A"
        config MIPLAY_I2S_BCLK_GPIO
            int "BCLK GPIO"
            range 0 48
            default 12
        config MIPLAY_I2S_LRCK_GPIO
            int "LRCK (WS) GPIO"
            range 0 48
            default 10
        config MIPLAY_I2S_DIN_GPIO
            int "DIN (SDATA) GPIO"
            range 0 48
            default 11
        config MIPLAY_I2S_ENABLE_MCLK
            bool "Output MCLK to PCM5102A"
            default y
        config MIPLAY_I2S_MCLK_GPIO
            int "MCLK GPIO"
            depends on MIPLAY_I2S_ENABLE_MCLK
            range 0 48
            default 13
    endmenu

    menu "Audio"
        config MIPLAY_DEFAULT_VOLUME
            int "Default volume (0-100)"
            range 0 100
            default 70
    endmenu

    menu "Status LED"
        config MIPLAY_LED_ENABLE
            bool "Enable status LED"
            default y
        config MIPLAY_LED_GPIO
            int "LED GPIO (48 = onboard WS2812 on SuperMini)"
            depends on MIPLAY_LED_ENABLE
            range 0 48
            default 48
    endmenu

endmenu
```

**★ 订正：Wi-Fi 凭据项已全部删除**

初稿的 `MIPLAY_WIFI_SSID` / `MIPLAY_WIFI_PASSWORD`（string 项）**已移除**，
理由见 §3.6：凭据改为运行时网页配网后写入 NVS。

`MIPLAY_WIFI_MAX_RETRY` 的语义也随之改变：

| | 初稿 | 实际 |
|---|---|---|
| 含义 | 重连次数（连上之后的掉线重连） | **用已存凭据连接失败几次后，放弃并改开配网热点** |
| 范围 | 无 | **`range 1 20`** |
| 默认 | `0`（无限重连） | **`3`** |
| 取值 0 | 允许 | **被 range 显式拒绝** |

**为什么把 0（无限重连）从"默认"改成"不允许"**：设备没有屏幕。
无限重连意味着在用户换了路由器或改了密码之后，设备会**永远沉默**——
用户只能看到灯一直闪橙色，却没有任何途径改配置（热点永远不会开）。
留几次重试是为了容忍路由器重启这类**瞬时**故障；重试完仍失败，
就必须把配置入口还给用户。这是"无屏幕设备"这个约束下的必然选择。

---

## 5. 内存预算

`miplay_remote` 的 PSRAM 需求（源码实测值）：

| 项目 | 大小 | 来源 |
|---|---|---|
| 接收缓冲 `MIPLAY_RX_BUF_LEN` | 256 KB + 9 | `miplay_proto.h:79`，`heap_caps_malloc(SPIRAM)` |
| 会话任务栈 `MIPLAY_CLIENT_TASK_STACK_BYTES` | **64 KB**（原 256 KB） | `miplay_session.h:37`，`xTaskCreatePinnedToCoreWithCaps(SPIRAM)` |
| 单帧拷贝临时缓冲 | 最多 256 KB | `miplay_control.cpp:705` |
| RTSP 持久缓冲 `kRtspBufSize` | 4 KB | `miplay_media.cpp:68` |
| PCM 输出缓冲 `kPcmBufSize` | 8 KB | `miplay_media.cpp:483` |
| ADTS 缓冲 `kAdtsBufSize` | 8 KB | `miplay_media.cpp:627` |
| **PSRAM 峰值合计** | **≈ 600 KB** | / 2 MB 可用 |

**结论**：够用，余量约 1.4 MB 供 Wi-Fi 与 lwIP。

**★ 订正：静态 BSS 已搬入 PSRAM**

`miplay_session.cpp:25` 的 `EXT_RAM_BSS_ATTR uint8_t s_encrypt_buf[256KB + 32]`
与 `:17` 的 `s_sessions[MIPLAY_MAX_CONTROL_SESSIONS]` 同宏同段。

初稿称"本设计刻意不开 `ALLOW_BSS_SEG_EXTERNAL_MEMORY`，因此该数组留在内部"
——**已被第一次链接证伪**（`region 'dram0_0_seg' overflowed by 48280 bytes`）。
现已在 `sdkconfig.defaults` 打开该 Kconfig，这 256 KB 落在 PSRAM。
完整订正与安全核实见 **§4.1**。

回顾一下这个坑的本质，因为它比"改一行配置"更值得记住：
`EXT_RAM_BSS_ATTR` 未开启时**展开为「空」**，源码里看着"已经标了 PSRAM 属性"，
实际仍在内部 SRAM，**编译器不给任何警告**。这种"声明了但静默失效"的属性，
只有在链接期以"总量超了"的形式暴露，且看不出是谁占的——必须读 map 文件
才能定位到"1707 个符号里就这一个占了 256 KB"。

**降级预案**（若实测 PSRAM 不足）：
第一降级点是接收缓冲，从 256 KB 降到 64 KB 并加断言。
协议规定的单帧上限是 256 KB，但实际 AAC 帧远小于此。

### 5.1 值得注意的实现细节

`miplay_create_task()`（`miplay_remote.cpp`）有一处精心设计：
先试 PSRAM 栈，失败回退内部 SRAM，并**登记每块栈的内存类型**，
删除时按类型选择 `vTaskDeleteWithCaps()` 或 `vTaskDelete()`。

源码注释记录了踩坑原因：

> 参考实现用 `xTaskGetStaticBuffers` 判断，但那只对 `xTaskCreateStatic` 有效，
> 对 `WithCaps` 永远返回 false，导致栈内存持续泄漏。

这正是"照搬而非重写"的价值所在 —— 这类细节重写时必然重踩。

---

## 6. 状态 LED 语义

原始需求：**连不上网用橙色，连上了用蓝色**。下表取自 `status_led.cpp` 的
`state_color()` / `brightness_phase()`，是**实际行为**：

| 状态 | 颜色 | RGB | 亮度行为 |
|---|---|---|---|
| `STATUS_BOOT` 启动中 | **白** | `255,255,255` | 呼吸（三角波，周期 2 s） |
| `STATUS_WIFI` Wi-Fi 连接中 | **橙** | `255,140,0` | 闪烁（半亮/全暗，周期 1 s） |
| `STATUS_AP` 配网热点已开 | **橙** | `255,140,0` | 常亮，压到 60% |
| `STATUS_IDLE` 已联网待机 | **蓝** | `0,120,255` | 低亮常亮（系数 35） |
| `STATUS_CONNECTED` 手机已连接 | **青** | `0,255,255` | 常亮 |
| `STATUS_PLAYING` 播放中 | **紫** | `180,0,255` | 常亮 |
| `STATUS_ERROR` 错误 | **红** | `255,0,0` | 快闪（周期 0.5 s） |

**配色推理**（几条不显然的取舍）：

- **橙色覆盖两态**（"正在连"与"回落开热点"）：两者对用户是同一件事——
  还没上网，需要（或可能需要）配网。分成两个颜色反而要用户学一套映射。
- **启动期用中性的白，不用蓝**：蓝已专属"已联网"，启动期用它会让用户
  以为已经连上了。白 = "还没定论"。
- **AP 态压到 60% 亮度**：配网要能开很久（等人来配），全亮的橙在暗处刺眼。
- **待机蓝压到系数 35**：同理，常亮的灯不刺眼才适合长期挂着。

驱动：板载 WS2812 用 `led_strip` 组件（RMT 后端）。若 GPIO48 无灯，Kconfig 可关闭。

### 6.1 ★ 一个曾长期存在的静默故障（LED 从未亮过）

**症状**：状态灯完全不亮。串口只有一行警告，设备其他一切正常：

```
E (480) led_strip_rmt: led_strip_new_rmt_device(139): invalid number of color components: 0
W (480) status_led: led_init failed on GPIO48: ESP_ERR_INVALID_ARG (continuing without LED)
```

**根因**：`led_strip` v3 起把颜色分量顺序从隐式默认改为**显式字段**，
而 `status_led.cpp` 当时只赋了 `r_pos`/`g_pos`/`b_pos`，
**漏掉了 `num_components` 与 `bytes_per_color`**，两者零初始化为 0。

驱动 `led_strip_rmt_dev.c:130-139` 的校验是**硬性的、无回退的**：

```c
if (component_fmt.format.num_components == 3) { /* 校验 r/g/b_pos 掩码 == 0x07 */ }
else if (component_fmt.format.num_components == 4) { /* 掩码 == 0x0F */ }
else {
    ESP_RETURN_ON_FALSE(false, ESP_ERR_INVALID_ARG, TAG,
                        "invalid number of color components: %d", ...);
}
```

**没有"留 0 等驱动回退"这回事**——0 既不是 3 也不是 4，直接失败。

**为什么长期没被发现**：外层错误处理把它**降级成一行 W 日志继续跑**。
这个降级本身是合理设计（LED 坏不该拖垮音频），但它把一个功能级故障
伪装成了"硬件可能有问题"。所以表现是"灯不亮"而不是崩溃——
很容易被误判成接线错误或灯珠坏掉，去反复量硬件。

**修复**：显式赋值 `bytes_per_color = 1` / `num_components = 3`。

**另一个被顺手修掉的坑（同文件）**：`strip_cfg` 是零初始化的，
而 `LED_STRIP_COLOR_COMPONENT_FMT_GRB` 宏展开为 **C99 复合字面量**，
在 C++ 下是 GCC 扩展，会引入可移植性与告警问题。所以改成**逐个字段赋值**
（`r_pos=1, g_pos=0, b_pos=2`，即 WS2812 的线上顺序 GRB），
既避开宏的 C++ 兼容问题，也把顺序意图写明白。

> 方法论教训：**代码里的注释不能当证据**。这段代码原本的注释声称
> "驱动分别回退到 1 与 3"——这个说法是错的，而它恰恰是"为什么没人去查驱动源码"
> 的原因。凡是"注释说了会回退"这类断言，都值得去驱动源码里核实一次。

---

## 7. 验证策略

分五步，每步有明确判据：

| 步骤 | 动作 | 通过判据 |
|---|---|---|
| 1 | 编译烧录，看启动日志 | `esp_psram: Adding pool of 1778K`、`status led on GPIO48` |
| 2 | 配网（首次） | 手机连开放热点 `MiPlay-Amp-XXXX`，**自动弹**配网页；提交后串口出现 `online as "..."` |
| 3 | 手机妙播搜索设备 | 手机列表中出现在线设备（`miplay_remote: started: ...`） |
| 4 | 连接并播放 | 串口打出 `AAC: rate=48000 ch=2` |
| 5 | 出声 | PCM5102A 输出到耳机有声音 |

**第 1 步即可验出 Quad PSRAM 配置错误**（会直接 panic 并打印 `Virtual address not enough for PSRAM!`）。

后续排错顺序：

- **第 2 步失败（有 AP 但不弹门户）** → 先查串口有没有
  `DNS hijack up: all A queries -> AP IP`。没有就是 `dns_server` 组件没链进来——
  这是"门户只做了一半"的典型症状，见 §3.6.2。
- 第 2 步失败（连不上 STA）→ 看 `scan served: N ap(s)` 确认设备扫到了什么；
  扫到 0 条多半是目标网络在 5GHz（**ESP32-S3 不支持 5GHz**）或距离太远。
- 第 3 步失败 → 查 mDNS 注册日志与 UDP 5355 应答；确认手机同网段；
  确认路由器没开 AP 隔离。
- 第 4 步失败 → 查 RTSP 握手日志与 `stream_key` 下发。
- 第 5 步失败 → 依次查：① 软件音量是否被置 0；② MCLK 线是否接对
  （本工程默认输出 MCLK，若模块 SCK 实为接地版则必须关闭该开关并断开连线）；
  ③ BCK/LRCK/DIN 是否接反。

### 7.1 v1.0 实测记录（2026-09-28）

已在实机（COM3，MAC `90:DA:72:4A:E0:B0`）验证通过的关键日志：

| 日志 | 证明 |
|---|---|
| `esp_psram: Adding pool of 1778K of PSRAM memory to heap allocator` | PSRAM Quad 配置正确，且外部 BSS 段已生效 |
| `status led on GPIO48` | LED 初始化成功（此前恒失败） |
| `DNS hijack up: all A queries -> AP IP` / `Socket bound, port 53` | 门户的 DNS 半边起来了 |
| `Received 34 bytes from 192.168.4.2` / `DNS reply with len: 50` | 真实客户端（手机）的 DNS 查询被应答 |
| `scan found 2 ap(s)` / `scan served: 2 ap(s)` | 扫描接口通 |
| `404 -> portal: /favicon.ico`、`404 -> portal: /mmtls/180f82a1` | 404 兜底生效（`/mmtls/...` 是手机厂商自有的探测路径，正好证明"逐条列举必漏"） |
| `MiPlay-Amp-E0B1` 出现在 `netsh wlan show networks` | AP 广播正常，MAC 后缀与设备一致 |

二进制最终占用 `0x114450 / 0x2F0000`（**63% 余量**）。
音频输出经用户确认：**"有声音，完美"** → 定为 v1.0。

---

## 8. 已知风险

| 风险 | 影响 | 应对 | 状态 |
|---|---|---|---|
| Quad PSRAM 带宽低于 Octal | 256KB 缓冲拷贝较慢 | 实测；必要时降缓冲尺寸 | 待实测 |
| PSRAM 峰值 / 2MB | 余量不宽裕 | 已备降级预案（第 5 节） | 实测 1778K 入堆，够用 |
| GY-PCM5102 SCK 版本不确定 | 接错则无声或短路 | 万用表量 SCK 对 GND 阻值判定；Kconfig 开关切换 | **已验证**：悬空版接线出声正常 |
| FH4R2 具体 PSRAM 型号未实测 | 理论依据来自官方文档 | 第 1 步验证会立刻暴露 | **已验证** |
| 长时间播放稳定性 | 未做老化测试 | — | 待实测（v1.0 只验短时播放） |

---

## 9. 环境

| 项 | 值 |
|---|---|
| ESP-IDF | v5.5.4，`E:\ESP\.espressif\v5.5.4\esp-idf` |
| 环境脚本 | `E:\ESP\esp_env.ps1`（必须 PowerShell，不用 Git Bash） |
| 构建命令 | `& $PYTHON $IDF_PY <args>` —— **`idf.py` 不在 PATH 上** |
| 构建目录 | 必须纯 ASCII 路径（ccache 4.x + 中文路径必崩） |

---

## 10. 参考工程

| 用途 | 路径 |
|---|---|
| **组件来源**（照搬） | `E:\ESP\ESP32-S3-RLCD-4.2-main\ESP32-S3-RLCD-4.2-main\RLCD_CLOCK\components\miplay_remote\` |
| **门户 DNS 组件来源**（照搬） | `<IDF>/examples/protocols/http_server/captive_portal/components/dns_server/` |
| 音频输出层参考 | 同上工程 `main/audio/audio_services.cpp`（`write_stereo_speaker`） |
| 板级初始化参考 | 同上工程 `main/core/main.cpp`（`app_main` 编排顺序） |
| 原始单体实现 | `E:\ESP\esp-miply-1.85touch\components\miplay\`（205KB，仅作协议疑难时对照） |
