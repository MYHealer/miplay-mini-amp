#include "status_led.h"

#include "esp_log.h"
#include "sdkconfig.h"

#ifdef CONFIG_MIPLAY_LED_ENABLE

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

namespace {

constexpr char kTag[] = "status_led";

#ifndef CONFIG_MIPLAY_LED_GPIO
#define CONFIG_MIPLAY_LED_GPIO 48
#endif

constexpr int kLedGpio = CONFIG_MIPLAY_LED_GPIO;
constexpr uint32_t kTickMs = 40;  // 刷新周期，决定呼吸/闪烁的平滑度

// 总亮度百分比（0-100）。这是唯一的全局亮度旋钮：所有状态的最终亮度都要再乘它，
// 所以调它等于同时调暗/调亮全部状态，各状态之间的相对明暗关系
// （谁比谁亮、呼吸与闪烁的波形）完全不变。
//
// 注意 WS2812 的"亮度"是每通道 8 位 PWM 占空比：50 等于把占空比砍半，也就是
// 光通量砍半。但人眼对亮度是非线性感知的，占空比减半看起来大约只暗三成——
// 若仍嫌亮，往下调即可，这里没有下限约束（最暗的 IDLE 态会先变得不明显）。
constexpr uint32_t kBrightnessPct = 50;

led_strip_handle_t s_strip = nullptr;
volatile status_led_state_t s_state = STATUS_BOOT;
volatile bool s_running = false;

// 各状态的基色（0-255）。
struct Rgb {
    uint8_t r, g, b;
};

Rgb state_color(status_led_state_t st)
{
    switch (st) {
        // 白：中性色，表示"还没定论"。不用蓝，蓝已专属"已联网"。
        case STATUS_BOOT:      return {255, 255, 255};
        case STATUS_WIFI:      return {255, 140, 0};  // 橙：还没连上
        case STATUS_AP:        return {255, 140, 0};  // 橙：热点已开，还没连上
        case STATUS_IDLE:      return {0, 120, 255};  // 蓝：已联网
        case STATUS_CONNECTED: return {0, 255, 255};  // 青：手机连上了
        case STATUS_PLAYING:   return {180, 0, 255};  // 紫：播放中
        case STATUS_ERROR:     return {255, 0, 0};    // 红
        default:               return {0, 0, 0};
    }
}

// 亮度系数。呼吸/闪烁都由它调制。
// 常亮状态返回固定系数，避免"看起来在闪"。
uint32_t brightness_phase(status_led_state_t st, uint32_t tick)
{
    switch (st) {
        case STATUS_BOOT: {
            // 呼吸：三角波，周期 2 秒（50 tick × 40ms）。
            const uint32_t p = tick % 50;
            return (p < 25 ? p : 50 - p) * 4;  // 0..100
        }
        case STATUS_WIFI: {
            // 闪烁：半亮/全暗，周期 1 秒。
            return ((tick % 25) < 12) ? 100 : 0;
        }
        case STATUS_ERROR: {
            // 错误闪得更快，周期 0.5 秒。
            return ((tick % 12) < 6) ? 100 : 0;
        }
        case STATUS_IDLE:
            return 35;  // 低亮常亮，不刺眼
        case STATUS_AP:
            // 常亮但压到 60%：热点要能开很久（等人来配网），
            // 全亮的橙在暗处刺眼。
            return 60;
        default:
            return 100;  // 常亮
    }
}

void apply_led(status_led_state_t st, uint32_t tick)
{
    const Rgb c = state_color(st);
    // 状态自身的相位系数 × 全局亮度，再一次性缩放三个通道。
    // 先乘后除（而不是先除后乘）能保住小系数下的精度：
    // IDLE 的 35 × 50 = 1750，除 100 得 17，若反过来先算 35/100 会直接塌成 0。
    const uint32_t k = brightness_phase(st, tick) * kBrightnessPct / 100;
    const uint8_t r = (uint8_t)((uint32_t)c.r * k / 100);
    const uint8_t g = (uint8_t)((uint32_t)c.g * k / 100);
    const uint8_t b = (uint8_t)((uint32_t)c.b * k / 100);
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

void led_task(void *arg)
{
    (void)arg;
    uint32_t tick = 0;
    while (s_running) {
        apply_led(s_state, tick);
        tick++;
        vTaskDelay(pdMS_TO_TICKS(kTickMs));
    }
    // 退出前熄灭，避免留下一个亮着的残灯。
    led_strip_clear(s_strip);
    vTaskDelete(nullptr);
}

}  // namespace

void status_led_init(void)
{
    led_strip_config_t strip_cfg = {};
    strip_cfg.strip_gpio_num = kLedGpio;
    strip_cfg.max_leds = 1;
    strip_cfg.led_model = LED_MODEL_WS2812;
    // led_strip v3 起把颜色分量顺序从隐式默认改为显式字段。零初始化会让
    // r_pos/g_pos/b_pos 全为 0——三个通道都写进第 0 个位置，颜色错乱
    // （蓝色常亮会显示成红色）。WS2812 的线上顺序是 GRB，必须显式声明。
    //
    // ⚠ bytes_per_color / num_components 也必须显式给值，不能"留 0 等驱动回退"：
    // 驱动 led_strip_rmt_dev.c:130-139 只接受 num_components 为 3 或 4，
    // 其余一律 ESP_ERR_INVALID_ARG 直接失败，没有任何默认回退。
    // 实测症状（曾长期存在）：led_strip_new_rmt_device 返回
    //   "invalid number of color components: 0"
    // 状态灯整个不工作，而外层错误处理把它降级成一行 W 日志继续跑，
    // 所以表现是"灯不亮"而不是崩溃，很容易被误判成硬件问题。
    //
    // 逐个字段赋值而非用 LED_STRIP_COLOR_COMPONENT_FMT_GRB 宏：该宏展开为
    // C99 复合字面量，在 C++ 下是 GCC 扩展，会引入可移植性与告警问题。
    strip_cfg.color_component_format.format.r_pos = 1;
    strip_cfg.color_component_format.format.g_pos = 0;
    strip_cfg.color_component_format.format.b_pos = 2;
    strip_cfg.color_component_format.format.bytes_per_color = 1;  // WS2812 每通道 8 位
    strip_cfg.color_component_format.format.num_components = 3;   // RGB，无 W 通道
    strip_cfg.flags.invert_out = false;

    // RMT 后端：ESP32-S3 支持，不占用 SPI，且时序由硬件保证。
    led_strip_rmt_config_t rmt_cfg = {};
    rmt_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    rmt_cfg.resolution_hz = 10 * 1000 * 1000;  // 10MHz
    rmt_cfg.flags.with_dma = false;            // 单灯不需要 DMA

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK || s_strip == nullptr) {
        ESP_LOGW(kTag, "led init failed on GPIO%d: %s (continuing without LED)",
                 kLedGpio, esp_err_to_name(err));
        s_strip = nullptr;
        return;
    }

    led_strip_clear(s_strip);
    s_running = true;
    // 2KB 栈足够（本任务只做三角函数级别的整数运算）。
    // 放 core 0：core 1 留给音频解码链。
    if (xTaskCreatePinnedToCore(led_task, "status_led", 2048, nullptr, 2,
                                nullptr, 0) != pdPASS) {
        ESP_LOGW(kTag, "led task create failed (continuing without LED)");
        s_running = false;
        return;
    }
    ESP_LOGI(kTag, "status led on GPIO%d", kLedGpio);
}

void status_led_set(status_led_state_t state)
{
    if (s_strip == nullptr) {
        return;
    }
    s_state = state;
}

#else  // !CONFIG_MIPLAY_LED_ENABLE

void status_led_init(void) {}
void status_led_set(status_led_state_t state) { (void)state; }

#endif  // CONFIG_MIPLAY_LED_ENABLE
