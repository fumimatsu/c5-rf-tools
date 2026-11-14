// Software-only layout/navigation test. Synthetic RF values are never sent to hardware.
#include "app_api.h"
#include "heat_palette.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
int64_t ui_smoke_time = 30000000;
static auto data = std::make_unique<rf::Data>();
static unsigned brightness = 10;
static bool brightness_failure = false;
static std::vector<Action> actions;
static uint16_t framebuffer[720 * 1280], drawbuffer[720 * 100];
bool app_post(ActionType type, unsigned value) {
    actions.push_back({type, value, ui_smoke_time});
    if (type == ActionType::Search) data->mode = rf::Mode::Search;
    if (type == ActionType::Monitor) data->mode = rf::Mode::Monitor;
    if (type == ActionType::SetChannel) data->plan.ids[value / rf::PRESETS] = value % rf::PRESETS;
    return true;
}
void app_copy_data(rf::Data *out) { *out = *data; }
unsigned app_brightness() { return brightness; }
bool app_set_brightness(unsigned value) {
    if (value < 5 || value > 100 || brightness_failure) return false;
    brightness = value;
    return true;
}
static void tick() { lv_tick_inc(100); lv_timer_handler(); lv_refr_now(nullptr); }
static bool visible(lv_obj_t *o) {
    for (auto *p = o; p; p = lv_obj_get_parent(p))
        if (lv_obj_has_flag(p, LV_OBJ_FLAG_HIDDEN)) return false;
    return true;
}
static lv_obj_t *find(lv_obj_t *root, const char *name) {
    if (!visible(root)) return nullptr;
    if (lv_obj_check_type(root, &lv_label_class) && !std::strcmp(lv_label_get_text(root), name)) return root;
    for (unsigned i = 0; i < lv_obj_get_child_count(root); i++)
        if (auto *o = find(lv_obj_get_child(root, i), name)) return o;
    return nullptr;
}
static void click(const char *name) {
    auto *l = find(lv_screen_active(), name);
    if (!l) { std::fprintf(stderr, "Missing visible button: %s\n", name); std::abort(); }
    auto *b = lv_obj_get_parent(l);
    assert(lv_obj_check_type(b, &lv_button_class));
    assert(!lv_obj_has_state(b, LV_STATE_DISABLED));
    lv_obj_send_event(b, LV_EVENT_CLICKED, nullptr);
    tick();
}
static void bounds(lv_obj_t *o) {
    if (!visible(o)) return;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    if (a.x1 < 0 || a.y1 < 0 || a.x2 >= 720 || a.y2 >= 1280) {
        std::fprintf(stderr, "Outside screen: (%ld,%ld)-(%ld,%ld) %s\n", (long)a.x1, (long)a.y1,
                     (long)a.x2, (long)a.y2, lv_obj_check_type(o, &lv_label_class) ? lv_label_get_text(o) : "object");
        std::abort();
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(o); i++) bounds(lv_obj_get_child(o, i));
}
static void snapshot(const char *name) {
    tick();
    tick();
    bounds(lv_screen_active());
    char path[120]; std::snprintf(path, sizeof(path), "%s.ppm", name);
    auto *f = std::fopen(path, "wb"); assert(f);
    std::fprintf(f, "P6\n720 1280\n255\n");
    for (uint16_t p : framebuffer) {
        unsigned char rgb[] = {static_cast<unsigned char>((p >> 11) * 255 / 31),
                               static_cast<unsigned char>(((p >> 5) & 63) * 255 / 63),
                               static_cast<unsigned char>((p & 31) * 255 / 31)};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
}
int main() {
    assert(heat_palette::jet(-1) == 0x0000ff);
    assert(heat_palette::jet(0.25f) == 0x00ffff);
    assert(heat_palette::jet(0.5f) == 0x00ff00);
    assert(heat_palette::jet(0.75f) == 0xffff00);
    assert(heat_palette::jet(2) == 0xff0000);
    assert(heat_palette::jet(NAN) == 0x24292d);
    lv_init();
    auto *display = lv_display_create(720, 1280);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, drawbuffer, nullptr, sizeof(drawbuffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, [](lv_display_t *d, const lv_area_t *a, uint8_t *bytes) {
        auto *p = reinterpret_cast<uint16_t *>(bytes);
        for (int y = a->y1; y <= a->y2; y++)
            for (int x = a->x1; x <= a->x2; x++) framebuffer[y * 720 + x] = *p++;
        lv_display_flush_ready(d);
    });
    data->mode = rf::Mode::Monitor;
    data->usb = data->profile_ok = data->firmware_ok = data->radio = true;
    data->last_record_us = ui_smoke_time;
    std::strcpy(data->notice, "HOST TEST / synthetic RF / not measurements");
    for (unsigned i = 0; i < 4; i++) {
        auto &c = data->channel[i];
        c.raw = c.filtered = c.peak = -350 + int(i) * 450;
        c.host_us = ui_smoke_time;
        c.valid = true;
    }
    for (int64_t index = 0; index < ui_smoke_time / rf::bin_us; index++) {
        auto &b = data->history[index % rf::HISTORY];
        b.index = index;
        b.valid_mask = 15;
        for (unsigned i = 0; i < 4; i++) {
            b.mean[i] = int16_t(-350 + i * 450 + 100 * std::sin(index * .1f));
            b.peak[i] = b.mean[i] + 100;
        }
    }
    app_build_ui(display);
    snapshot("monitor");
    click("明るさ"); snapshot("brightness");
    click("5 %"); assert(brightness == 5);
    click("-5%"); assert(brightness == 5);
    click("100 %"); assert(brightness == 100);
    click("+5%"); assert(brightness == 100);
    brightness_failure = true;
    click("25 %"); assert(brightness == 100);
    brightness_failure = false;
    click("起動値 10%"); assert(brightness == 10);
    click("閉じる");
    click("背景記録"); snapshot("background"); click("閉じる");
    click("正常基準"); snapshot("normal"); click("閉じる");
    click("1  E2  5685 MHz"); snapshot("picker");
    click("R"); snapshot("picker-r"); click("R8  5917 MHz");
    assert(data->plan.ids[0] == 7);
    click("帯域 / WF");
    data->sweep_completed_us = ui_smoke_time;
    data->sweeps = data->row_count = rf::WATER_ROWS;
    data->row_head = 0;
    for (unsigned i = 0; i < rf::SWEEP; i += 2) {
        data->sweep[i] = data->sweep_peak[i] = data->rows[0].level[i] = -800 + 1000 * std::sin(i * .15f);
        data->sweep_time[i] = ui_smoke_time;
    }
    for (unsigned row = 0; row < rf::WATER_ROWS; row++) {
        data->rows[row].completed_us = ui_smoke_time - (rf::WATER_ROWS - 1 - row) * 800000;
        for (unsigned i = 0; i < rf::SWEEP; i += 2)
            data->rows[row].level[i] = int16_t(500 + 2000 * std::sin(i * .15f + row * .07f));
    }
    snapshot("search");
    assert(find(lv_screen_active(), ">= +45 dB"));
    data->background_active = true;
    std::fill_n(data->background.sweep, rf::SWEEP, -300);
    data->rows[10].level[20] = rf::missing;
    tick();
    click("表示:受信値");
    snapshot("search-delta");
    assert(find(lv_screen_active(), ">= +30 dB"));
    click("高速 4MHz / 詳細へ");
    assert(actions.back().type == ActionType::SearchStep && actions.back().value == 2);
    click("4ch履歴");
    assert(data->mode == rf::Mode::Monitor);
    app_ui_diagnostics();
    std::puts("UI_SMOKE_PASSED: bounds, dialogs, channel, modes, brightness limits/failure (host only)");
}
