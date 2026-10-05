#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t auto_poweroff_delay_ms;
    uint8_t charge_supress_percent;
    // 0 = OFF. Otherwise power off (with the low-battery warning + countdown)
    // once the gauge reports this charge percentage or lower while running on
    // the internal battery (not charging) -- e.g. an external powerbank ran dry.
    uint8_t shutdown_battery_percent;
    // 0 = OFF. Otherwise an absolute power-off timer: the device powers off this
    // many milliseconds after boot regardless of activity or running app.
    uint32_t shutdown_timer_ms;
} PowerSettings;

#ifdef __cplusplus
extern "C" {
#endif

void power_settings_load(PowerSettings* settings);
void power_settings_save(const PowerSettings* settings);

#ifdef __cplusplus
}
#endif
