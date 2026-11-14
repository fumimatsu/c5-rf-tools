#pragma once
#include <cstdint>
extern int64_t ui_smoke_time;
inline int64_t esp_timer_get_time() { return ui_smoke_time; }
