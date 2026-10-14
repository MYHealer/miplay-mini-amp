# MiPlay Mini Amp — 小米妙播最小耳放

[![License](https://img.shields.io/badge/license-AGPL--3.0-blue.svg)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.5.4-red.svg)](https://docs.espressif.com/projects/esp-idf/)
[![Platform](https://img.shields.io/badge/chip-ESP32--S3-green.svg)](https://www.espressif.com/)
[![Cost](https://img.shields.io/badge/成本-约20元-orange.svg)](#2-快速上手)

**用小米妙播（MiPlay）协议做的最小音频耳放系统板。** 仅供开发者学习小米原生
协议使用——不是小米音箱、不是小爱音箱，**只有内核协议，没有功放电路和外壳**。

```
小米手机「妙播」
      │  Wi-Fi 局域网
      ▼
ESP32-S3 SuperMini ──I2S──▶ GY-PCM5102 ──▶ 3.5mm 耳机
```

**做**：网页配网（强制门户）→ 被手机发现 → 接收投屏音频 → 解码 → I2S 输出 → 耳机出声。外加一颗状态 LED。

**不做**：屏幕、DLNA、airkan 遥控、歌词封面、OTA、按键。

> **为什么要做这个**：仅需 **约 20 元成本**，就能学习开发小米妙播协议，
> 最终可移植到其他音频设备，得到属于极客玩家自己的"小米音箱"。
> 也方便现在和将来的小米工程师熟悉妙播协议的开发。
>
> **这是一次小小的尝试。** 感谢小米互通互联的协议，感谢乐鑫 ESP32 芯片的性能，
> 也感谢立创开源硬件平台的包容开放。希望三方在 AI 硬件的方向上通过不断交流沟通，
> 共同建立起更加生机蓬勃的 AI 硬件开发生态——让科技的乐趣落到每一个家庭、
> 每一个个人，让科技平权。

> ⚠ **本项目仅供学习与个人研究使用，禁止商用与售卖。** 与小米公司无隶属关系。
> 完整条款见文末 [免责声明](#免责声明) 与 [许可证](#许可证)。

---

## 项目亮点

| 特性 | 说明 |
|---|---|
| **零编译期凭据** | Wi-Fi 密码不进源码、不进 sdkconfig。首次开机开 AP + 强制门户，手机弹窗填密码（见 §4.1） |
| **协议完整** | 5355 legacy DNS-SD + mDNS 双服务宣告 + TCP 8899 控制口 + RTSP/RTP/TS/AES/AAC 全链路 |
| **Web 配网** | 仿 <https://mimo.xiaomi.com/> 视觉风格的单文件内嵌页面，零外部依赖 |
| **低 BOM** | 两块模块 + 几根杜邦线，约 20 元 |
| **双任务音频** | 解码与 I2S 输出解耦，128KB PSRAM 环形缓冲吸收网络抖动 |

---

## 1. 硬件

| 项 | 规格 |
|---|---|
| 主控 | **ESP32-S3FH4R2**（4MB Flash + 2MB Quad SPI PSRAM，均片内） |
| 音频 | **GY-PCM5102** 模块（紫色小板，带 3.5mm 座） |
| 供电 | USB-C（SuperMini 板载） |

### 接线

| 信号 | GPIO | 定义位置 | 说明 |
|---|---|---|---|
| BCLK | **12** | `main/Kconfig.projbuild` `MIPLAY_I2S_BCLK_GPIO` | 位时钟 → PCM5102 的 BCK |
| LRCK | **10** | `main/Kconfig.projbuild` `MIPLAY_I2S_LRCK_GPIO` | 左右声道时钟 → LRCK |
| DIN | **11** | `main/Kconfig.projbuild` `MIPLAY_I2S_DIN_GPIO` | 音频数据 → DIN |
| MCLK | **13** | `main/Kconfig.projbuild` `MIPLAY_I2S_MCLK_GPIO` | → 模块的 SCK 焊盘。**默认已启用**，见下方说明 |
| GND | GND | — | **必须共地** |
| 3V3 | 3V3 | — | 模块供电 |

> 引脚在 `menuconfig → MiPlay Mini Amp → I2S / PCM5102A` 可改，无需动代码。
> 下表 GPIO 号即 `sdkconfig` 里的固化值。

**MCLK 说明（重要）**

GY-PCM5102 在市面有两种流通版本，外观无法区分：

- **SCK 焊盘悬空** → **必须**由主控喂 MCLK，否则完全无声。**本工程按这一版接线**，
  `MIPLAY_I2S_ENABLE_MCLK` 默认已打开，GPIO13 接模块 SCK。
- **SCK 焊盘已接地** → PCM5102A 用内部 PLL 从 BCK 锁相，**不需要 MCLK**。
  此时若仍把 GPIO13 接上去，等于输出脚对地短路，必须关闭
  `MIPLAY_I2S_ENABLE_MCLK` 并断开这条线。

**怎么判断手上是哪一版**：万用表量 SCK 焊盘对 GND —— 开路是悬空版（保持默认），
接近 0Ω 是接地版（关掉 MCLK 开关、不接这根线）。

**避开**：GPIO19/20（USB-JTAG，日志就靠它）、GPIO0/3/45/46（strapping）、
GPIO26-32（片内 Flash/PSRAM 占用）。当前用的 10-13 全部落在 P2 区（无限制）。

---

## 2. 快速上手

### 元件清单

| 元件 | 说明 | 参考价 |
|---|---|---|
| ESP32-S3 SuperMini | 需 **FH4R2** 型号（4MB Flash + 2MB PSRAM） | ~15 元 |
| GY-PCM5102 | 紫色 DAC 小板，带 3.5mm 座 | ~5 元 |
| 杜邦线 | 6 根（BCLK/LRCK/DIN/MCLK/GND/3V3） | — |
| USB-C 线 | 供电 + 烧录 | — |

### 方式一：下载固件直接烧录

从 [Releases](https://github.com/MYHealer/miplay-mini-amp/releases) 下载最新版 `miplay-mini-amp.bin`。

```bash
pip install esptool

# 全量烧录（首次，COM 号按实际改）
esptool --chip esp32s3 -p COM18 -b 460800 \
  --before usb_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 4MB --flash_freq 80m \
  0x0     bootloader.bin \
  0x8000  partition-table.bin \
  0x10000 miplay-mini-amp.bin
```

**仅升级固件**（保留已配置的 Wi-Fi 凭据）：

```bash
esptool --chip esp32s3 -p COM18 -b 460800 \
  --before usb_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 4MB --flash_freq 80m \
  0x10000 miplay-mini-amp.bin
```

> ⚠️ 改代码后只烧 `0x10000` 一个地址即可，**不要全量烧**——那会覆盖 `nvs` 分区，
> 已配好的 Wi-Fi 密码丢失，需要重新配网。
>
> ⚠️ **`--flash_size 4MB`**：本板是 FH4R2（4MB Flash + 2MB PSRAM）。
> 别照抄其他 ESP32-S3 项目的 8MB/16MB。
>
> ⚠️ **`--before usb_reset`**：S3 内置 USB-Serial-JTAG 在普通复位下可能进不了下载模式，
> 用 `usb_reset` 最稳。副作用是 USB 会重枚举，**COM 口号可能变化**，烧完重新插一次即可。

### 方式二：从源码构建

```bash
# 1) 编译
cd E:\ESP && . .\esp_env.ps1
cd E:\ESP\miplay-mini-amp
& $PYTHON $IDF_PY set-target esp32s3
& $PYTHON $IDF_PY build

# 2) 烧录 + 监视
& $PYTHON $IDF_PY -p COMx flash monitor
```

### 上电后三步

**① 配网** → **② 手机找设备** → **③ 投屏播放**

1. 设备首次上电无凭据，开一个**开放热点** `MiPlay-Amp-XXXX`
2. 手机连上该热点，浏览器**自动弹出**配网页（强制门户），选 Wi-Fi 填密码
3. 连上后设备关热点、进正常待机（蓝灯）。手机打开「妙播」→ 找到设备 → 投屏

---

## 3. 构建详解

必须用 **PowerShell**（Git Bash 会踩 MSYSTEM 污染，`idf.py` 秒退）。

⚠ `idf.py` **不在 PATH 上**：`esp_env.ps1` 只导出 `$PYTHON` 与 `$IDF_PY` 两个变量，
必须显式 `& $PYTHON $IDF_PY <args>` 调用（命令见 §2 方式二）。

构建目录**必须是纯 ASCII 路径** —— ccache 4.x 遇到中文路径必崩，
所以本项目放在 `E:\ESP\miplay-mini-amp` 而不是中文目录。

**看构建日志**：PowerShell 的 `>` 重定向产出 UTF-16LE（字符间夹空格，无法读），
要落盘用 `| Out-File -Encoding utf8`。

**抓完整启动日志**：`idf.py monitor` 是**连上之后才开始收**的，而 ROM bootloader
与 `app_main` 早于它——会丢掉启动头。要抓全，用 `tools/capture_boot_log.py`：
它先持有串口、再用 RTS/DTR 触发复位，保证不丢任何一行。

```powershell
& $PYTHON tools/capture_boot_log.py COM18 14   # 端口 + 抓取秒数
```

> 脚本内含 20 次重试：`usb_reset` 会让 USB 短暂消失，端口要等它稳定。

---

## 4. 配置

`idf.py menuconfig` → `MiPlay Mini Amp`：

### 4.1 Wi-Fi 配网（无编译期凭据）

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

### 4.2 配网页与强制门户

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

### 4.3 其他

| 配置项 | 默认 | 说明 |
|---|---|---|
| `MIPLAY_WIFI_MAX_RETRY` | 3 | 连 STA 失败几次后回落开 AP。范围 1–20 |
| `MIPLAY_I2S_BCLK_GPIO` | 12 | |
| `MIPLAY_I2S_LRCK_GPIO` | 10 | |
| `MIPLAY_I2S_DIN_GPIO` | 11 | |
| `MIPLAY_I2S_ENABLE_MCLK` | **y** | 本工程按 SCK 悬空版接线。若模块 SCK 已接地必须关掉 |
| `MIPLAY_I2S_MCLK_GPIO` | 13 | 接模块 SCK 焊盘 |
| `MIPLAY_DEFAULT_VOLUME` | 70 | PCM5102A 无 I2C 口，音量是软件缩放 |
| `MIPLAY_LED_ENABLE` | y | 板子没灯就关掉 |
| `MIPLAY_LED_GPIO` | 48 | SuperMini 板载 WS2812 |

### 4.4 分区表（`partitions.csv`）

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

### 4.5 内存要点

**必须 `CONFIG_SPIRAM_MODE_QUAD=y`。** FH4R2 的片内 PSRAM 是 Quad SPI，
照抄参考工程的 `OCT` 会启动即 panic（`Virtual address not enough for PSRAM!`）。
第 1 步验证就会暴露这个问题。

**`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y` 已打开。** 这是必须的：
`miplay_session.cpp` 的 `s_encrypt_buf`（256 KB）标了 `EXT_RAM_BSS_ATTR`，而该宏
**只在打开此 Kconfig 时才展开**，否则悄悄留 0 字节，把 256 KB 全压在内部 SRAM，
构建期直接 `region 'dram0_0_seg' overflowed by 48280 bytes`。

打开前已逐项核实安全性：该缓冲是纯软件中转（`memcpy` → `mbedtls_aes_crypt_cbc`
→ `memcpy`），**无 DMA、无 ISR 访问**；且同函数里的中间缓冲 `padded` 本来就已
用 `heap_caps_calloc(..., MALLOC_CAP_SPIRAM)` 分配在 PSRAM。见 §8。

---

## 5. 状态 LED

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

## 6. 验证顺序

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
  `dns_server` 组件没链进来。门户的这两半是**与**关系，缺一不弹，见 §4.2。
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

## 7. 目录结构

```
miplay-mini-amp/
├── CMakeLists.txt
├── sdkconfig.defaults       # Quad PSRAM / 外部 BSS 段 / 自定义分区表
├── partitions.csv           # 单 factory，无 OTA
├── DESIGN.md                # 完整设计文档（决策依据）
├── README.md                # 本文
├── components/
│   ├── miplay_remote/       # 协议层（见 §8）
│   │                        #   设备标识符 / 发现通道的详解见 §9
│   │                        #   miplay_identity.cpp   身份派生（idHash/appsData/lyra AppData）
│   │                        #   miplay_discovery.cpp  5355 广播 + 5353 mDNS 宣告
│   │                        #   miplay_control.cpp    TCP 8899 控制口
│   │                        #   miplay_media.cpp      RTSP/RTP/TS/AES/AAC 投屏链路
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

## 8. 与上游的差异

协议层 `components/miplay_remote/` 照搬自：

```
E:\ESP\ESP32-S3-RLCD-4.2-main\ESP32-S3-RLCD-4.2-main\RLCD_CLOCK\components\miplay_remote\
```

选它是因为该组件已从单体 `esp-miply-1.85touch/components/miplay`（205KB）
剥离为独立组件，**零 LVGL、零 GMF、零 airkan**，且协议实现已在 RLCD 硬件上跑通。

上游硬件是 **N16R8**（16MB Flash + 8MB Octal PSRAM），本机是 **FH4R2**
（4MB Flash + 2MB Quad PSRAM），所以内存与 Flash 配置全部重做。

下表的改动清单已用 `diff -rq` 对上游目录逐个核对过：**共 5 个文件有差异**，
其余 12 个与上游字节一致。任何后续修改都应先跑一次这个 diff 确认范围。

### 已改（共 5 处，均附理由写入源码注释）

| 文件 | 改动 | 理由 |
|---|---|---|
| `miplay_session.h` | 会话栈 **256KB → 64KB** | 实测该任务栈上最大对象是 `body[512]`，调用链深度估算 <8KB，64KB 留 8 倍余量 |
| `miplay_media.cpp:1672` | rtsp 任务退出改用 `miplay_delete_current_task()` | **修 bug**：原为 `vTaskDeleteWithCaps()`，绕过追踪表清槽 → 槽位泄漏 → 后续任务 PSRAM 栈永不释放 |
| `miplay_remote_internal.h` | `MIPLAY_DEVICE_DISPLAY_NAME` → `"MiPlay Mini Amp"` | 设备在手机妙播列表里的显示名。硬约束只有两条：不含 `.`（会破坏 DNS label 压缩指针）、编码后 ≤63 字节（`dns_push_label` 用 `strlen` 截断）。见 §9.5 的 27 字节上限 |
| `miplay_proto.h` | 仅补注释（值未改） | 记录「256KB 为何不能下调」的核查结论 |
| `miplay_control.cpp:187` | **删除一条过期注释** | 原注释写「与 _mi-connect 的 dev=2 对齐」，但 `MIPLAY_DEV` 早已改为 `"4"`（音箱）。注释记录的是**当初快照值**，宏会变而注释不会——留着就是唯一会说谎的地方。删掉即可：两处用的是同一个宏，代码本身已说明对齐关系 |

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

## 9. 小米妙播协议：设备标识符

**这一节是排查"手机扫不到设备"的第一手资料。** 手机（妙播 / HyperOS 投屏）
识别一台接收端，靠的是下面三组**逐字节固定**的字段。任何一组改错，症状都不是
报错，而是**设备从列表里静默消失**——所以改动前务必对照本表。

三个发现通道按时间顺序：

```
① UDP 5355 广播/应答（legacy DNS-SD，手机开机即扫）
       │  命中 → 拿到 IP + 端口 8899
       ▼
② mDNS 5353 主动宣告（_mi-connect._udp + _lyra-mdns._udp 两条服务）
       │  手机比对两条路径的身份字段，不一致则整条记录作废
       ▼
③ TCP 8899 控制口（手机建立会话，下发 SetMirrorKey 启动投屏）
```

### 9.1 身份字段的派生（`miplay_identity.cpp`）

| 字段 | 值 | 派生方式 |
|---|---|---|
| `device_id` | 如 `92445368` | **MAC 后 4 字节的大写 hex**（8 字符）。既是 DNS 主机名前缀，也是后续所有哈希的种子 |
| `idHash` | 如 `MVo4` | **4 字符标准 base64**。三步：① `SHA256(device_id)` 前 3 字节 → base64url → `short_id`（3 字符）；② 把 `short_id` 这 3 个 **ASCII 字节**再走一遍标准 base64 |
| `inst_name` | `MiPlay Mini Amp(1Z8I7NJ9DJ)` | **`"<显示名>(<SHA256 前 10 个 base64url 字符>)"`**，后缀防同网内多台设备重名 |
| `s_appsdata` | `gQAEBIMiww==` | **硬编码，必须原样照抄**。解码后是 `81 00 04 04 83 22 C3`：`0x0483` 是 Lyra 能力位，`0x22C3` = 十进制 8899 是控制端口 |
| `s_mac_b64` | MAC 的 base64 | `mac[0..2]` 与 `mac[3..5]` 各编一段，拼成 8 字符 |

**⚠ `idHash` 最容易写错的地方**：把 `short_id`（3 字符）直接当 `idHash` 发出。
权威定义来自 `FusionPlay-Android discovery.rs:2055-2058`——
`idHash = base64(short_id)`，因为 `short_id` 恰好 3 字节，标准 base64 结果**恒为 4 字符**。
发出 3 字符会让手机的 `BonjourGovernor` 判定身份不一致。

**⚠ `idHash` 必须在两条路径上逐字节相同**：5355 的 `idhash` 与 mDNS 的 `idHash`
是同一个字符串的两个副本。手机侧会比对，任何不一致都会让**整条记录被拒**。
（旧实现曾在 5355 侧截断成 3 字符，症状正是"设备扫不到"。）

### 9.2 通道 ①：UDP 5355 的 TXT（`build_lan_packet`）

单条 TXT 记录，内容是一段 JSON：

```json
{"supportLyra":true,
 "mac":"AA:BB:CC:DD:EE:FF",
 "ID":<stable_id>,
 "type":3,
 "idhash":"<4 字符>",
 "extraAbility":0}
```

| 键 | 说明 |
|---|---|
| `type` | **`3` = 申报 TV**。这是关键字段，决定手机把它归入哪一类接收端 |
| `ID` | MAC 后 4 字节拼成的 int，**清掉最高位**（`& 0x7FFFFFFF`）避免负数 |
| `supportLyra` | `true`，表示支持 Lyra 遥控路径 |
| `idhash` | 必须与 8.3 的 `idHash` 逐字节相同 |

SRV 记录的端口固定 **8899**，主机名是 `miplay-<MAC后3字节>`。

### 9.3 通道 ②：mDNS 5353 的两条服务

**`_mi-connect._udp`** — 主发现服务，9 条 TXT：

| 键 | 值 | 说明 |
|---|---|---|
| `version` | `196608` | = `0x30000` |
| `apps` | `[5]` | |
| `flags` | `CgE=` | |
| `name` | `MiPlay Mini Amp` | **用户在手机列表里唯一能看到的标识** |
| `idHash` | `MVo4` | 见 8.1 |
| `dev` | **`4`** | **设备类型：`4` = 音箱**（`MIPLAY_DEV`）。取值表见下 |
| `sec` | `4` | 与 `dev` 取同值（`MIPLAY_SEC`） |
| `appsData` | `gQAEBIMiww==` | 硬编码，见 8.1 |
| `mac` | MAC 的 base64 | |

**`_lyra-mdns._udp`** — Lyra 遥控路径，5 条 TXT：

| 键 | 值 | 说明 |
|---|---|---|
| `AppData` | base64 二进制 | 见下方字节布局 |
| `MediumType` | `8192` | |
| `CH` | `0` | |
| `DebugInfo` | `{msg:..., ifname:STA, v4:...}` | IP 中间两段做 `数字→'#'+数字` 编码 |
| `TS` | Unix epoch **毫秒** | 手机据此判断宣告新鲜度 |

**`dev` 设备类型取值表**（`MIPLAY_DEV` / `MIPLAY_SEC`）——
以下全部经 **2026-09-28 实机逐个烧录确认**：

| 值 | 手机妙播列表显示 | 备注 |
|---|---|---|
| `1` | 手机 | 参考实现无此值，实测补出 |
| `2` | 电视 / Android TV | 与参考实现一致 |
| `3` | 电脑 | |
| **`4`** | **音箱** ← 本工程 | 与 FusionPlay `Speaker=4` 吻合 |
| `5` | 车 | 与 FusionPlay `Vehicle=5` 吻合 |
| `6` / `7` / `8` | 蓝牙喇叭 | 参考实现无此值；**三者显示完全相同** |

> `6`/`7`/`8` 都回落成「蓝牙喇叭」+ 普通图标——手机端对**未映射值统一回落**。
> 有区分度的只有 `1`–`5` 这五个连续锚点，继续试 `9`/`10`… 收益为零。

**⚠ 取 `dev=4`（音箱）的代价**：实机测得此时设备**网络层可发现**（`_miplay_lan`
有应答、mDNS 注册成功），但**可能不进妙播投屏列表**；`dev=2`（电视）实测可投屏。
本工程取 `4` 是**有意选择**（显示为音箱图标），若实测投屏列表里找不到设备，
改回 `"2"` 即可。

**`AppData` 的字节布局**（`appdata[]`，base64 前）：

```
偏移  值        含义
 0    0x00      固定
 1    0x40      固定
 2    0x03      设备类型：3 = TV
 3-6  MAC[2..5] 设备 MAC 后 4 字节
 7-22 kFixed[16] 固定序列 00 05 19 24 | 10 01 03 0a 03 01 da ae 01 01 80 02
 23   name_len  显示名长度（单字节前缀）
 24.. 显示名     ASCII
 末3  0x25 0x01 0x03  固定尾
```

### 9.4 通道 ③：TCP 8899 控制口

手机识别到设备后会连 8899 建立会话。`miplay_control.cpp:187` 的
`{"deviceType", MIPLAY_DEV}` 必须与通道 ②/① 的类型申报**对齐**。

真正启动音频投屏的是控制口的 **`SetMirrorKey(0x006C)`** 事件——
它由手机下发，`miplay_media.cpp` 收到后才开始 RTSP/RTP/TS/AES/AAC 全链路。
这也是为什么 `components/miplay_remote/include/miplay_remote.h` 顶部那句
「剥离了 RTSP 投屏、音频管线」是**过时的**（见 §10 末）。

### 9.5 显示名的两条硬约束

`MIPLAY_DEVICE_DISPLAY_NAME` 看似自由文本，实际受 DNS 层限制：

```
inst_name = "<显示名>(<10 字符哈希后缀>)"   → 存入 s_inst_name[40]
```

- **不能含 `.`**：点号是 DNS label 分隔符，手机解析 PTR 时会当成两个 label，
  后面 SRV/TXT 的压缩指针偏移随之错乱，整包被判畸形而丢弃——
  表现就是"设备明明应答了 5355，手机列表里却没有它"。
- **显示名最多 27 字节**：后缀固定 12 字节（`(` + 10 字符 + `)`），
  `s_inst_name` 是 40 字节，`snprintf` 会截断。`"MiPlay Mini Amp"` 是 15 字节 →
  实例名 27 字节，安全。且 27 < 63（单 label 上限），`dns_push_label` 不会截断。

---

## 10. 已知风险 / 待实测确认

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

## 11. 环境

| 项 | 值 |
|---|---|
| ESP-IDF | v5.5.4（`E:\ESP\.espressif\v5.5.4\esp-idf`） |
| 环境脚本 | `E:\ESP\esp_env.ps1`（**必须 PowerShell**） |
| 构建目录 | 必须纯 ASCII 路径（ccache 4.x + 中文路径必崩） |

---

## 参考项目及致谢

本项目建立在多个上游开源工作之上，特此致谢。

### 参考项目

| 项目 | 说明 |
|---|---|
| [MYHealer/esp-miply-1.85touch](https://github.com/MYHealer/esp-miply-1.85touch) | 同作者的 1.85" 触屏版本。本项目协议层组件即从其剥离，I2S 输出层的 DMA 配置（12 × 512）亦参考其板级定义 |
| [MYHealer/dlna-esp32](https://github.com/MYHealer/dlna-esp32) | 同作者的纯 DLNA 投屏实现，是协议层与音频管线的起点 |
| [MYHealer/airkan-sdk](https://github.com/MYHealer/airkan-sdk) | 小米妙享遥控器按键通道的独立实现（本工程未使用，供扩展参考） |
| [rosienosiesie/FusionPlay-Android](https://github.com/rosienosiesie/FusionPlay-Android) | Android 端 MiPlay 播放器，**本项目 MiPlay 协议实现的关键参考**：`idHash` 三步派生、`appsData` 硬编码值、lyra `AppData` 字节布局均据此对齐 |

### 第三方组件致谢

| 组件 | 作者 / 组织 | 用途 |
|---|---|---|
| [ESP-IDF](https://github.com/espressif/esp-idf) | Espressif Systems | 芯片 SDK 与构建系统 |
| [esp_audio_codec](https://components.espressif.com/components/espressif/esp_audio_codec) | Espressif Systems | AAC-LC 解码 |
| [led_strip](https://components.espressif.com/components/espressif/led_strip) | Espressif Systems | 板载 WS2812 状态灯（RMT 后端） |
| [mdns](https://components.espressif.com/components/espressif/mdns) | Espressif Systems | mDNS 服务注册与宣告 |
| [mbedtls](https://github.com/Mbed-TLS/mbedtls) | TrustedFirmware | SHA-256 与 AES-128-CBC |
| IDF `captive_portal` 示例 | Espressif Systems | 强制门户的 DNS 劫持组件（`components/dns_server/`） |

### 协议实现致谢

- **小米妙播（MiPlay）** 的协议细节来自对官方客户端与固件的逆向分析，
  以及社区项目 [FusionPlay](https://github.com/rosienosiesie/FusionPlay-Android)
  公开的 Rust 实现。感谢这些工作为设备标识符与发现流程提供了关键证据链。
- 感谢**小米互通互联的协议**与**乐鑫 ESP32 芯片**的性能，
  以及**立创开源硬件平台**的包容开放。

---

## 免责声明

本项目为独立的逆向工程实践，**仅供学习与个人研究使用**，与小米公司无任何隶属、认可或赞助关系。

- ⚠️ **仅供学习使用** — 本项目用于协议分析、嵌入式开发与音视频技术的学习交流，请勿用于任何商业用途。
- 🚫 **禁止售卖** — 不得将本项目（含源码、固件、衍生作品）用于销售、批量生产或任何形式的商业牟利。
- 📄 **责任自负** — 使用本项目即表示你自行承担全部责任，包括对小米服务条款的合规性。作者不提供任何担保，亦不承担任何责任。
- ™️ **商标归属** — 「小米」「妙播」「妙享」「米家」及相关标识均为小米公司商标，本项目对其的使用仅为技术说明性引用。

---

## 许可证

本项目采用 **[AGPL-3.0](LICENSE)**（GNU Affero General Public License v3.0）许可。

> ⚠️ AGPL-3.0 是**强 Copyleft** 许可：任何修改、分发，**以及通过网络提供服务**的行为，都必须
> 以同样的 AGPL-3.0 完整开源（第 13 条「Remote Network Interaction」）。闭源商用与售卖均不被允许。

### 第三方组件许可证

本项目使用的第三方组件均为宽松许可，与 AGPL-3.0 **兼容**（可被 AGPL 项目引用，各自仍保留原许可）：

| 组件 | 许可证 |
|---|---|
| ESP-IDF / esp_audio_codec / led_strip / mdns | Apache-2.0 |
| mbedtls | Apache-2.0 |
| IDF `captive_portal` 示例（含 `dns_server`） | Apache-2.0 |
