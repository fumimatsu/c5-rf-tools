#pragma once
#include "lvgl.h"
#include "rf_core.h"
enum class ActionType : uint8_t {
    Monitor,
    Search,
    Background,
    AdoptBackground,
    ClearBackground,
    ResetPeaks,
    Plan,
    SearchStep,
    SetChannel,
    ChannelSet,
    NormalOff,
    NormalOn,
    AdoptNormal,
    CancelNormal
};
struct Action {
    ActionType type;
    unsigned value = 0;
    int64_t issued_us = 0;
};
bool app_post(ActionType type, unsigned value = 0);
void app_copy_data(rf::Data *out);
void app_build_ui(lv_display_t *display);
void app_ui_diagnostics();
void rf_self_test();
