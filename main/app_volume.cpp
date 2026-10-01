#include "app_volume.h"

#include "esp_log.h"

namespace {
constexpr char kTag[] = "volume";
int s_volume = 70;
}  // namespace

void app_volume_init(int default_percent)
{
    app_volume_set(default_percent);
    ESP_LOGI(kTag, "software volume init: %d%%", s_volume);
}

void app_volume_set(int percent)
{
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    s_volume = percent;
}

int app_volume_get(void)
{
    return s_volume;
}

void app_volume_apply(int16_t *samples, size_t sample_count)
{
    const int vol = s_volume;
    if (vol >= 100) {
        return;  // 满音量，原样输出
    }
    // 中间量必须是 int32：int16 的 -32768 乘 100 会溢出。
    // 不做四舍五入——音频里这点误差不可闻，省一次加法和判断。
    for (size_t i = 0; i < sample_count; i++) {
        samples[i] = (int16_t)(((int32_t)samples[i] * vol) / 100);
    }
}
