#include "app_api.h"
#include "bsp/display.h"
#include "bsp/m5stack_tab5.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
extern "C" {
LV_FONT_DECLARE(rf_jp20);
LV_FONT_DECLARE(rf_jp24);
}
namespace {
constexpr uint32_t BG = 0x111315, PANEL = 0x191c1f, EDGE = 0x3b4146, WHITE = 0xe2e5e7, DIM = 0x969da3,
                   AMBER = 0xd2a660;
constexpr uint32_t traces[4] = {0x639bc0, 0x80ab83, 0xc4a25f, 0xb57c76};
constexpr int SW = 720, SCREEN_H = 1280, PW = 608, HH = 224, SH = 168, WH = 300;
rf::Data *view;
lv_obj_t *brightness_modal, *brightness_value, *brightness_message;
bool brightness_open = false;
lv_obj_t *screen, *status, *health, *notice_label, *hint_label, *monitor, *search, *modal;
lv_obj_t *monitor_nav, *search_nav, *imd_button, *background_nav, *peak_nav, *scale_button, *step_button;
lv_obj_t *values[4], *units[4], *raw_labels[4], *states[4], *plan_buttons[4];
lv_obj_t *channel_buttons[4], *search_channel_buttons[4], *history_legend;
lv_obj_t *picker, *picker_title, *band_buttons[5], *choice_buttons[8];
lv_obj_t *card_panels[4], *level_labels[4], *stability_labels[4], *normal_buttons[4];
lv_obj_t *normal_modal, *normal_title, *normal_message, *normal_primary, *normal_adopt, *normal_close;
bool normal_open = false;
unsigned normal_slot = 0;
int64_t normal_request_us = 0;
bool picker_open = false;
unsigned picker_slot = 0, picker_band = 0;
lv_obj_t *history_canvas, *spectrum_canvas, *water_canvas, *axis[4], *history_title;
lv_obj_t *sweep_status, *cursor_label, *water_time, *modal_message, *adopt_button, *search_title;
uint16_t *history_pixels, *spectrum_pixels, *water_pixels;
lv_draw_buf_t history_buf, spectrum_buf, water_buf;

bool show_search = false, show_imd = false, modal_open = false, show_delta = false;
int selected = 20;
unsigned ticks = 0;
uint32_t last_draw_us = 0, max_draw_us = 0, last_ui_us = 0;
int64_t rate_started_us = 0;
uint32_t rate_records = 0, records_per_second = 0;
unsigned last_water_sweeps = UINT32_MAX, last_water_count = UINT32_MAX, last_water_revision = UINT32_MAX,
         last_water_stride = UINT32_MAX;
bool last_water_delta = false, last_water_active = false;
uint16_t rgb(uint32_t c) {
    return uint16_t(((c >> 19) & 31) << 11 | ((c >> 10) & 63) << 5 | ((c >> 3) & 31));
}
lv_obj_t *label(lv_obj_t *p, const char *t, int x, int y, int width, const lv_font_t *font = &rf_jp20,
                uint32_t color = WHITE) {
    auto *o = lv_label_create(p);
    lv_label_set_text(o, t);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_width(o, width);
    lv_label_set_long_mode(o, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(o, font, 0);
    lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
    return o;
}
void text(lv_obj_t *o, const char *s) {
    if (std::strcmp(lv_label_get_text(o), s))
        lv_label_set_text(o, s);
}
lv_obj_t *panel(lv_obj_t *p, int x, int y, int w, int h) {
    auto *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(o, lv_color_hex(PANEL), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(EDGE), 0);
    lv_obj_set_style_radius(o, 0, 0);
    return o;
}
lv_obj_t *button(lv_obj_t *p, const char *t, int x, int y, int w, int h, lv_event_cb_t cb,
                 void *user = nullptr) {
    auto *b = lv_button_create(p);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(PANEL), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(EDGE), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(EDGE), LV_STATE_PRESSED);
    auto *l = lv_label_create(b);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, &rf_jp20, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(WHITE), 0);
    lv_obj_set_width(l, w - 20);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    if (cb)
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    return b;
}
void button_text(lv_obj_t *b, const char *t) {
    text(lv_obj_get_child(b, 0), t);
}
void pixel(uint16_t *p, int w, int h, int x, int y, uint16_t c) {
    if (x >= 0 && x < w && y >= 0 && y < h)
        p[y * w + x] = c;
}
void segment(uint16_t *p, int w, int h, int x0, int y0, int x1, int y1, uint16_t c, unsigned pattern = 0) {
    const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        const bool draw = pattern == 0 || (pattern == 1   ? (x0 % 12 < 7)
                                           : pattern == 2 ? (x0 % 5 < 2)
                                                          : (x0 % 18 < 10 || x0 % 18 == 14));
        if (draw) {
            pixel(p, w, h, x0, y0, c);
            pixel(p, w, h, x0, y0 + 1, c);
        }
        if (x0 == x1 && y0 == y1)
            break;
        const int e = 2 * err;
        if (e >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}
int plot_y(int16_t value, int base, bool relative, int h) {
    const float db = (value - base) / 100.f;
    const float lo = -10.f, hi = 50.f;
    (void)relative;
    return std::clamp(int(std::lround((hi - db) / (hi - lo) * (h - 1))), 0, h - 1);
}
void grid(uint16_t *p, int h, bool relative) {
    std::fill_n(p, PW * h, rgb(BG));
    for (int i = 0; i <= 6; i++) {
        const int y = (h - 1) * i / 6;
        segment(p, PW, h, 0, y, PW - 1, y, rgb(0x30353a));
    }
    for (int i = 0; i <= 6; i++) {
        const int x = (PW - 1) * i / 6;
        for (int y = 0; y < h; y += 5)
            pixel(p, PW, h, x, y, rgb(0x25292d));
    }
    (void)relative;
}
void history_plot(int64_t now) {
    const bool relative = view->background_active;
    grid(history_pixels, HH, relative);
    const int64_t current = now / rf::bin_us;
    for (unsigned channel = 0; channel < 4; channel++) {
        bool have = false;
        int lastx = 0, lasty = 0;
        const int base = relative ? view->background.monitor[channel] : 0;
        const auto color = rgb(traces[channel]);
        for (unsigned t = 0; t < rf::HISTORY; t++) {
            const int64_t index = current - int64_t(rf::HISTORY) + t;
            const int x = int(t) * (PW - 1) / (rf::HISTORY - 1);
            if (index < 0) {
                have = false;
                continue;
            }
            const auto &bin = view->history[unsigned(index % rf::HISTORY)];
            const bool valid =
                bin.index == index && (bin.valid_mask & (1u << channel)) && !(bin.bad_mask & (1u << channel));
            if (!valid) {
                have = false;
                continue;
            }
            const int y = plot_y(bin.mean[channel], base, relative, HH);
            if (have)
                segment(history_pixels, PW, HH, lastx, lasty, x, y, color, channel);
            else
                pixel(history_pixels, PW, HH, x, y, color);
            if (bin.peak[channel] != rf::missing && t % 3 == 0) {
                const int py = plot_y(bin.peak[channel], base, relative, HH);
                pixel(history_pixels, PW, HH, x, py, color);
            }
            lastx = x;
            lasty = y;
            have = true;
        }
    }
    for (int i = 0; i < 4; i++) {
        char s[20];
        const int val = 50 - i * 20;
        std::snprintf(s, sizeof(s), "%+d", val);
        text(axis[i], s);
    }
    text(history_title, relative ? "60秒 RAW-背景 dB / 100ms電力平均・PK"
                                 : "60秒 RAW dB / 100ms電力平均・PK");
    lv_obj_invalidate(history_canvas);
}
void spectrum_plot() {
    const bool relative = view->background_active && show_delta;
    grid(spectrum_pixels, SH, relative);
    bool old = false, old_peak = false;
    int oldx = 0, oldy = 0, peakx = 0, peaky = 0;
    for (unsigned i = 0; i < rf::SWEEP; i += view->sweep_stride) {
        const int x = int(i) * (PW - 1) / (rf::SWEEP - 1), base = relative ? view->background.sweep[i] : 0;
        if (view->sweep[i] != rf::missing && (!relative || base != rf::missing)) {
            const int y = plot_y(view->sweep[i], base, relative, SH);
            if (old)
                segment(spectrum_pixels, PW, SH, oldx, oldy, x, y, rgb(0x7e8992));
            for (int dx = -1; dx <= 1; dx++)
                for (int dy = -1; dy <= 1; dy++)
                    pixel(spectrum_pixels, PW, SH, x + dx, y + dy, rgb(WHITE));
            old = true;
            oldx = x;
            oldy = y;
        } else
            old = false;
        if (view->sweep_peak[i] != rf::missing && (!relative || base != rf::missing)) {
            const int y = plot_y(view->sweep_peak[i], base, relative, SH);
            if (old_peak)
                segment(spectrum_pixels, PW, SH, peakx, peaky, x, y, rgb(0x7c858d), 1);
            old_peak = true;
            peakx = x;
            peaky = y;
        } else
            old_peak = false;
    }
    if (relative) {
        const int y = plot_y(0, 0, true, SH);
        for (int x = 0; x < PW; x += 4)
            pixel(spectrum_pixels, PW, SH, x, y, rgb(0x879099));
    }
    if (view->background_active && !relative) {
        bool have = false;
        int px = 0, py = 0;
        for (unsigned i = 0; i < rf::SWEEP; i++) {
            if (view->background.sweep[i] == rf::missing) {
                have = false;
                continue;
            }
            const int x = int(i) * (PW - 1) / (rf::SWEEP - 1),
                      y = plot_y(view->background.sweep[i], 0, false, SH);
            if (have)
                segment(spectrum_pixels, PW, SH, px, py, x, y, rgb(0x66717b), 2);
            px = x;
            py = y;
            have = true;
        }
    }
    for (unsigned channel = 0; channel < 4; channel++) {
        const int x = int(rf::presets[view->plan.ids[channel]].mhz - 5645) * (PW - 1) / 272;
        for (int y = 0; y < SH; y += 5)
            pixel(spectrum_pixels, PW, SH, x, y, rgb(traces[channel]));
    }
    if (show_imd) {
        bool plans[4];
        for (unsigned i = 0; i < 4; i++)
            plans[i] = view->channel[i].planned;
        uint16_t candidates[rf::SWEEP];
        uint16_t selected_frequencies[rf::N];
        rf::plan_frequencies(view->plan, selected_frequencies);
        const unsigned n = rf::imd_candidates(plans, candidates, rf::SWEEP, selected_frequencies);
        for (unsigned i = 0; i < n; i++) {
            const int x = int(candidates[i] - 5645) * (PW - 1) / 272;
            for (int y = 0; y < SH; y += 7)
                pixel(spectrum_pixels, PW, SH, x, y, rgb(AMBER));
        }
    }
    const int cursor = selected * (PW - 1) / (rf::SWEEP - 1);
    segment(spectrum_pixels, PW, SH, cursor, 0, cursor, SH - 1, rgb(0xaaa174), 2);
    lv_obj_invalidate(spectrum_canvas);
}
void waterfall_plot(int64_t now) {
    std::fill_n(water_pixels, PW * WH, rgb(BG));
    for (unsigned row = 0; row < view->row_count; row++) {
        const unsigned source = (view->row_head + rf::WATER_ROWS - view->row_count + row) % rf::WATER_ROWS;
        const auto &r = view->rows[source];
        const unsigned draw_row = rf::WATER_ROWS - view->row_count + row;
        const int y0 = int(draw_row) * WH / rf::WATER_ROWS, y1 = int(draw_row + 1) * WH / rf::WATER_ROWS;
        for (unsigned f = 0; f < rf::SWEEP; f += view->sweep_stride) {
            // Each acquired point owns the cell between neighbouring frequency midpoints.
            // No synthetic samples are inserted into skipped 2 MHz storage slots.
            const int stride = int(view->sweep_stride);
            const int x0 = f == 0 ? 0 : (2 * int(f) - stride) * PW / (2 * (rf::SWEEP - 1));
            const int x1 = f == rf::SWEEP - 1 ? PW : (2 * int(f) + stride) * PW / (2 * (rf::SWEEP - 1));
            uint16_t color = rgb(0x24292d);
            if (r.level[f] != rf::missing) {
                const int base = view->background_active && show_delta ? view->background.sweep[f] : 0;
                const float db = (r.level[f] - base) / 100.f;
                const float normalized =
                    view->background_active && show_delta ? db / 30.f : (db + 10.f) / 55.f;
                const unsigned grey = unsigned(std::clamp(28 + int(normalized * 190), 20, 220));
                color = rgb((grey << 16) | (grey << 8) | grey);
            }
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++)
                    water_pixels[y * PW + x] =
                        r.level[f] == rf::missing && ((x + y) % 8 < 2) ? rgb(0x50565c) : color;
        }
    }
    for (unsigned channel = 0; channel < 4; channel++) {
        const int x = int(rf::presets[view->plan.ids[channel]].mhz - 5645) * (PW - 1) / 272;
        for (int y = 0; y < WH; y += 6)
            pixel(water_pixels, PW, WH, x, y, rgb(traces[channel]));
    }
    char s[72];
    if (view->row_count) {
        const auto &oldest = view->rows[(view->row_head + rf::WATER_ROWS - view->row_count) % rf::WATER_ROWS];
        std::snprintf(s, sizeof(s), "履歴 %u掃引 / 最古 %.0f秒前", view->row_count,
                      (now - oldest.completed_us) / 1e6);
    } else
        std::snprintf(s, sizeof(s), "掃引完了後に履歴を表示");
    text(water_time, s);
    lv_obj_invalidate(water_canvas);
}
void modal_close() {
    lv_obj_add_flag(modal, LV_OBJ_FLAG_HIDDEN);
    modal_open = false;
}
void build_modal() {
    modal = lv_obj_create(screen);
    lv_obj_remove_style_all(modal);
    lv_obj_set_size(modal, SW, SCREEN_H);
    lv_obj_set_style_bg_color(modal, lv_color_hex(0x08090a), 0);
    lv_obj_set_style_bg_opa(modal, 230, 0);
    lv_obj_remove_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    auto *box = panel(modal, 20, 300, 680, 650);
    label(box, "背景記録", 24, 24, 632, &rf_jp24);
    modal_message = label(box, "", 24, 80, 632);
    lv_obj_set_height(modal_message, 130);
    lv_label_set_long_mode(modal_message, LV_LABEL_LONG_WRAP);
    button(box, "全機OFFを確認して記録開始", 24, 230, 632, 72, [](lv_event_t *) {
        app_post(ActionType::Background);
        modal_close();
    });
    adopt_button = button(box, "設置条件が同じ：保存背景を使う", 24, 318, 632, 72, [](lv_event_t *) {
        app_post(ActionType::AdoptBackground);
        modal_close();
    });
    button(box, "背景を削除", 24, 434, 300, 60, [](lv_event_t *) {
        app_post(ActionType::ClearBackground);
        modal_close();
    });
    button(box, "閉じる", 360, 434, 296, 60, [](lv_event_t *) { modal_close(); });
    modal_close();
}
void open_modal() {
    char s[300];
    std::snprintf(s, sizeof(s),
                  "ご自身の全VTXをOFFにしてください。4chを5秒と広域を1掃引記録します。\nアンテナ・位置・向き"
                  "が変わったら、背景を取り直してください。");
    text(modal_message, s);
    lv_obj_remove_flag(modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(modal);
    modal_open = true;
}
void close_picker() {
    lv_obj_add_flag(picker, LV_OBJ_FLAG_HIDDEN);
    picker_open = false;
}
void refresh_picker() {
    char s[128];
    const auto &current = rf::presets[view->plan.ids[picker_slot]];
    std::snprintf(s, sizeof(s), "枠 %u / 現在 %s %u MHz / チャンネル選択", picker_slot + 1, current.name,
                  current.mhz);
    text(picker_title, s);
    for (unsigned band = 0; band < 5; band++)
        lv_obj_set_style_border_color(band_buttons[band], lv_color_hex(band == picker_band ? WHITE : EDGE),
                                      0);
    for (unsigned channel = 0; channel < 8; channel++) {
        const unsigned id = picker_band * 8 + channel;
        const auto &preset = rf::presets[id];
        std::snprintf(s, sizeof(s), rf::preset_enabled(id) ? "%s  %u MHz" : "%s  %u MHz / 範囲外",
                      preset.name, preset.mhz);
        button_text(choice_buttons[channel], s);
        if (rf::preset_enabled(id))
            lv_obj_remove_state(choice_buttons[channel], LV_STATE_DISABLED);
        else
            lv_obj_add_state(choice_buttons[channel], LV_STATE_DISABLED);
        lv_obj_set_style_border_color(
            choice_buttons[channel],
            lv_color_hex(id == view->plan.ids[picker_slot] ? traces[picker_slot] : EDGE), 0);
        lv_obj_set_style_text_color(lv_obj_get_child(choice_buttons[channel], 0),
                                    lv_color_hex(rf::preset_enabled(id) ? WHITE : DIM), 0);
    }
}
void open_picker(unsigned slot) {
    if (view->background_busy || view->normal_capture.busy() || slot >= rf::N)
        return;
    picker_slot = slot;
    picker_band = view->plan.ids[slot] / 8;
    refresh_picker();
    lv_obj_remove_flag(picker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(picker);
    picker_open = true;
}
void build_picker() {
    picker = lv_obj_create(screen);
    lv_obj_remove_style_all(picker);
    lv_obj_set_size(picker, SW, SCREEN_H);
    lv_obj_set_style_bg_color(picker, lv_color_hex(0x08090a), 0);
    lv_obj_set_style_bg_opa(picker, 240, 0);
    lv_obj_remove_flag(picker, LV_OBJ_FLAG_SCROLLABLE);
    auto *box = panel(picker, 20, 170, 680, 940);
    picker_title = label(box, "チャンネル選択", 24, 24, 632, &rf_jp24);
    const char *bands[] = {"R", "A", "B", "E", "F"};
    for (unsigned band = 0; band < 5; band++) {
        band_buttons[band] = button(
            box, bands[band], 24 + int(band) * 128, 80, 120, 54,
            [](lv_event_t *e) {
                picker_band = unsigned(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
                refresh_picker();
            },
            reinterpret_cast<void *>(uintptr_t(band)));
    }
    for (unsigned channel = 0; channel < 8; channel++) {
        choice_buttons[channel] = button(
            box, "", 24 + int(channel % 2) * 324, 160 + int(channel / 2) * 84, 308, 70,
            [](lv_event_t *e) {
                const unsigned channel = unsigned(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
                const unsigned id = picker_band * 8 + channel;
                if (rf::preset_enabled(id) && !view->background_busy &&
                    app_post(ActionType::SetChannel, picker_slot * rf::PRESETS + id))
                    close_picker();
            },
            reinterpret_cast<void *>(uintptr_t(channel)));
    }
    label(box, "5645-5917 MHz / E7・E8は測定範囲外", 24, 508, 632, &rf_jp20, DIM);
    button(box, "セット E2 / E1 / F3 / F5", 24, 602, 632, 64, [](lv_event_t *) {
        if (!view->background_busy && app_post(ActionType::ChannelSet, 0))
            close_picker();
    });
    button(box, "セット R2 / R3 / R4 / R5", 24, 680, 632, 64, [](lv_event_t *) {
        if (!view->background_busy && app_post(ActionType::ChannelSet, 1))
            close_picker();
    });
    button(box, "閉じる", 360, 820, 296, 60, [](lv_event_t *) { close_picker(); });
    close_picker();
}
bool reference_busy() {
    return view->background_busy || view->normal_capture.busy();
}
uint32_t grade_color(signal_health::Grade g) {
    switch (g) {
    case signal_health::Grade::Green:
        return 0x79a780;
    case signal_health::Grade::Yellow:
        return AMBER;
    case signal_health::Grade::Red:
        return 0xcc7774;
    default:
        return DIM;
    }
}
const char *grade_jp(signal_health::Grade g) {
    switch (g) {
    case signal_health::Grade::Green:
        return "緑";
    case signal_health::Grade::Yellow:
        return "黄";
    case signal_health::Grade::Red:
        return "赤";
    default:
        return "灰";
    }
}
const char *unknown_jp(signal_health::Reason why) {
    using R = signal_health::Reason;
    switch (why) {
    case R::Inactive:
        return "保存基準未採用";
    case R::Conditions:
        return "条件不一致";
    case R::Paused:
        return "監視中断";
    case R::Invalid:
        return "測定無効";
    case R::Transport:
        return "通信欠落";
    case R::Warming:
        return "評価中";
    case R::Recording:
        return "基準記録中";
    case R::PlannedOff:
        return "予定OFF";
    case R::Duplicate:
        return "周波数重複";
    default:
        return "基準なし";
    }
}
void health_line(char *s, size_t size, const signal_health::Result &r, bool stability) {
    using G = signal_health::Grade;
    using R = signal_health::Reason;
    const auto g = stability ? r.stability : r.level;
    const auto why = stability ? r.stability_reason : r.level_reason;
    const char *name = stability ? "安定性" : "レベル";
    if (g == G::Gray) {
        if (why == R::Warming)
            std::snprintf(s, size, "%s 灰:評価 %u/100", name, r.count);
        else
            std::snprintf(s, size, "%s 灰:%s", name, unknown_jp(why));
    } else if (why == R::Recovering)
        std::snprintf(s, size, "%s %s:復帰確認", name, grade_jp(g));
    else if (stability && why == R::Dips)
        std::snprintf(s, size, "安定性 %s:低下%u回", grade_jp(g), r.dips);
    else if (stability)
        std::snprintf(s, size, "安定性 %s:変動%.1fdB", grade_jp(g), r.spread / 100.f);
    else if (why == R::LowRise)
        std::snprintf(s, size, "レベル %s:無送信比%+.1fdB", grade_jp(g), r.rise / 100.f);
    else
        std::snprintf(s, size, "レベル %s:正常比%+.1fdB", grade_jp(g), -r.drop / 100.f);
}
void close_normal() {
    lv_obj_add_flag(normal_modal, LV_OBJ_FLAG_HIDDEN);
    normal_open = false;
    normal_request_us = 0;
}
void refresh_normal() {
    using P = signal_health::Phase;
    using E = signal_health::Error;
    char s[900];
    const auto &preset = rf::presets[view->plan.ids[normal_slot]];
    const auto &ref = rf::normal_reference(*view, normal_slot);
    const auto &cap = view->normal_capture;
    const bool own = cap.slot == normal_slot;
    const P phase = own ? cap.phase : P::Idle;
    if (own && phase == P::Saved && normal_request_us && cap.started_us >= normal_request_us) {
        close_normal();
        return;
    }
    std::snprintf(s, sizeof(s), "枠%u / %s %u MHz / 正常基準", normal_slot + 1, preset.name, preset.mhz);
    text(normal_title, s);
    const char *primary = "全機OFFで無送信を記録（3秒）";
    bool enabled = view->usb && view->settings.valid() && view->firmware_ok && !view->background_busy &&
                   !rf::duplicate_channel(*view, normal_slot);
    if (own && phase == P::Preparing) {
        std::snprintf(s, sizeof(s), "C5の4ch設定を確認中です。全VTXをOFFのままお待ちください。");
        enabled = false;
    } else if (own && phase == P::Off) {
        std::snprintf(s, sizeof(s), "無送信を記録中 %u/30枠。全VTXをOFFのままお待ちください。", cap.count);
        enabled = false;
    } else if (own && phase == P::AwaitOn) {
        std::snprintf(s, sizeof(s),
                      "無送信を記録しました。\n%"
                      "sの正常な機体1台だけをONにし、他のVTXはOFFのまま、次のボタンを押してください。位置・向"
                      "き・出力を固定します。",
                      preset.name);
        primary = "対象1台ONで正常基準を記録（10秒）";
    } else if (own && phase == P::Normal) {
        std::snprintf(
            s, sizeof(s),
            "正常時を記録中 %u/100枠。\n対象1台だけONのまま、位置・向き・出力を変えないでください。",
            cap.count);
        enabled = false;
    } else if (own && phase == P::Ready) {
        std::snprintf(s, sizeof(s), "基準を本体に保存しています。");
        enabled = false;
    } else if (own && phase == P::Failed) {
        if (cap.error == E::TooWeak)
            std::snprintf(s, sizeof(s),
                          "保存しませんでした。無送信比%+."
                          "1fdBで、記録条件の10dBに届きません。対象ch・出力・位置を確認してください。\n以前の"
                          "保存基準は保持されています。",
                          (cap.candidate.median - cap.candidate.off_p95) / 100.f);
        else if (cap.error == E::Unstable)
            std::snprintf(s, sizeof(s),
                          "保存しませんでした。変動幅%.1fdB / "
                          "最大低下%."
                          "1fdB。記録条件は幅3dB以下・低下6dB以下です。\n機体とTab5を置いて固定し、ON後に数秒"
                          "待って取り直してください。以前の保存基準は保持されています。",
                          (cap.candidate.p90 - cap.candidate.p10) / 100.f,
                          (cap.candidate.median - cap.candidate.minimum) / 100.f);
        else if (cap.error == E::Transport)
            std::snprintf(s, sizeof(s),
                          "保存しませんでした。USB通信の欠落・CRC・C5の取りこぼしを検出しました。\n"
                          "接続と給電を確認し、全機OFFからやり直してください。以前の保存基準は保持されています。");
        else
            std::snprintf(s, sizeof(s),
                          "保存しませんでした。欠測・飽和・受信条件・保存状態を確認し、全機OFFからやり直して"
                          "ください。\n以前の保存基準は保持されています。");
    } else
        std::snprintf(
            s, sizeof(s),
            "背景記録とは別の正常基準です。\n1. 全VTX OFFで3秒記録。\n2. "
            "対象の正常な1台だけONで10秒記録。\n%s同じ機体・アンテナ・位置・向き・出力で使用してください。",
            signal_health::valid(ref) ? "保存基準あり。" : "保存基準なし。");
    text(normal_message, s);
    button_text(normal_primary, primary);
    if (enabled)
        lv_obj_remove_state(normal_primary, LV_STATE_DISABLED);
    else
        lv_obj_add_state(normal_primary, LV_STATE_DISABLED);
    if (!reference_busy() && rf::normal_matches(*view, normal_slot, ref) &&
        !rf::duplicate_channel(*view, normal_slot))
        lv_obj_remove_state(normal_adopt, LV_STATE_DISABLED);
    else
        lv_obj_add_state(normal_adopt, LV_STATE_DISABLED);
    button_text(normal_close, view->normal_capture.busy() ? "記録中断して閉じる" : "閉じる");
}
void open_normal(unsigned slot) {
    if (slot >= rf::N || reference_busy())
        return;
    normal_slot = slot;
    normal_request_us = 0;
    refresh_normal();
    lv_obj_remove_flag(normal_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(normal_modal);
    normal_open = true;
}
void build_normal() {
    normal_modal = lv_obj_create(screen);
    lv_obj_remove_style_all(normal_modal);
    lv_obj_set_size(normal_modal, SW, SCREEN_H);
    lv_obj_remove_flag(normal_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(normal_modal, lv_color_hex(0x08090a), 0);
    lv_obj_set_style_bg_opa(normal_modal, 240, 0);
    auto *box = panel(normal_modal, 20, 240, 680, 800);
    normal_title = label(box, "正常基準", 24, 24, 632, &rf_jp24);
    normal_message = label(box, "", 24, 80, 632);
    lv_obj_set_height(normal_message, 260);
    lv_label_set_long_mode(normal_message, LV_LABEL_LONG_WRAP);
    normal_primary = button(box, "全機OFFで無送信を記録（3秒）", 24, 370, 632, 80, [](lv_event_t *) {
        const bool on = view->normal_capture.slot == normal_slot &&
                        view->normal_capture.phase == signal_health::Phase::AwaitOn;
        const int64_t now = esp_timer_get_time();
        if (app_post(on ? ActionType::NormalOn : ActionType::NormalOff, normal_slot))
            normal_request_us = now;
    });
    normal_adopt =
        button(box, "同じ機体・設置・出力：保存正常基準を使う", 24, 474, 632, 80, [](lv_event_t *) {
            if (app_post(ActionType::AdoptNormal, normal_slot))
                close_normal();
        });
    label(box, "暫定判定 / 緑は基準内\n映像品質・無干渉を保証しません", 24, 584, 632, &rf_jp20, DIM);
    normal_close = button(box, "閉じる", 360, 704, 296, 60, [](lv_event_t *) {
        if (view->normal_capture.busy())
            app_post(ActionType::CancelNormal);
        close_normal();
    });
    close_normal();
}
void refresh_brightness() {
    char s[32];
    std::snprintf(s, sizeof(s), "%u %%", app_brightness());
    text(brightness_value, s);
}
void set_brightness(unsigned percent) {
    const bool ok = app_set_brightness(percent);
    text(brightness_message, ok ? "設定は保存しません / 起動時10%" : "変更できませんでした / 明るさは変更前のまま");
    refresh_brightness();
}
void build_brightness() {
    brightness_modal = lv_obj_create(screen);
    lv_obj_remove_style_all(brightness_modal);
    lv_obj_set_size(brightness_modal, SW, SCREEN_H);
    lv_obj_remove_flag(brightness_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(brightness_modal, lv_color_hex(0x08090a), 0);
    lv_obj_set_style_bg_opa(brightness_modal, 240, 0);
    auto *box = panel(brightness_modal, 20, 300, 680, 680);
    label(box, "画面の明るさ", 24, 24, 632, &rf_jp24);
    brightness_value = label(box, "10 %", 24, 84, 632, &lv_font_montserrat_40);
    auto *info = label(box, "5〜100%。高輝度は給電によって再起動する場合があります。\n設定は保存しません。再起動時は10%に戻ります。", 24, 150, 632);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(info, 74);
    auto *preview = panel(box, 24, 238, 632, 42);
    lv_obj_set_style_bg_color(preview, lv_color_hex(WHITE), 0);
    label(preview, "明るさ確認", 16, 7, 600, &rf_jp20, BG);
    button(box, "-5%", 24, 300, 150, 64, [](lv_event_t *) {
        set_brightness(std::max(5, int(app_brightness()) - 5));
    });
    button(box, "+5%", 182, 300, 150, 64, [](lv_event_t *) {
        set_brightness(std::min(100u, app_brightness() + 5));
    });
    button(box, "起動値 10%", 398, 300, 258, 64, [](lv_event_t *) { set_brightness(10); });
    const unsigned presets[] = {5, 10, 25, 50, 75, 100};
    for (unsigned i = 0; i < 6; i++) {
        char name[16];
        std::snprintf(name, sizeof(name), "%u %%", presets[i]);
        button(box, name, 24 + 216 * (i % 3), 386 + 74 * (i / 3), 200, 60,
               [](lv_event_t *e) { set_brightness(unsigned(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)))); },
               reinterpret_cast<void *>(uintptr_t(presets[i])));
    }
    brightness_message = label(box, "設定は保存しません / 起動時10%", 24, 538, 632);
    button(box, "閉じる", 360, 578, 296, 60, [](lv_event_t *) {
        lv_obj_add_flag(brightness_modal, LV_OBJ_FLAG_HIDDEN);
        brightness_open = false;
    });
    lv_obj_add_flag(brightness_modal, LV_OBJ_FLAG_HIDDEN);
}
void bind_canvas(lv_obj_t *parent, lv_obj_t *&canvas, lv_draw_buf_t &descriptor, uint16_t *&pixels, int x,
                 int y, int height) {
    const size_t bytes = PW * height * 2;
    pixels =
        static_cast<uint16_t *>(heap_caps_aligned_alloc(128, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    assert(pixels);
    std::fill_n(pixels, PW * height, rgb(BG));
    assert(lv_draw_buf_init(&descriptor, PW, height, LV_COLOR_FORMAT_RGB565, PW * 2, pixels, bytes) ==
           LV_RESULT_OK);
    canvas = lv_canvas_create(parent);
    lv_canvas_set_draw_buf(canvas, &descriptor);
    lv_obj_set_pos(canvas, x, y);
    lv_obj_remove_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
}
void update(lv_timer_t *) {
    const int64_t update_started = esp_timer_get_time();
    app_copy_data(view);
    const int64_t now = esp_timer_get_time();
    if (!rate_started_us) { rate_started_us = now; rate_records = view->records; }
    if (now - rate_started_us >= 1000000) {
        records_per_second = uint64_t(uint32_t(view->records - rate_records)) * 1000000 / (now - rate_started_us);
        rate_records = view->records; rate_started_us = now;
    }
    const bool transport_bad = view->transport_us && now - view->transport_us < 1000000;
    const bool next_search = view->mode == rf::Mode::Search || view->mode == rf::Mode::BackgroundSweep;
    if (next_search != show_search) {
        show_search = next_search;
        if (show_search) {
            lv_obj_add_flag(monitor, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(search, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(imd_button, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(scale_button, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(monitor, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(search, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(imd_button, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(scale_button, LV_OBJ_FLAG_HIDDEN);
        }
    }
    lv_obj_set_style_border_color(monitor_nav, lv_color_hex(show_search ? EDGE : WHITE), 0);
    lv_obj_set_style_border_color(search_nav, lv_color_hex(show_search ? WHITE : EDGE), 0);
    char s[320];
    unsigned live = 0;
    bool any_alert = false, any_bad = false;
    for (unsigned i = 0; i < 4; i++) {
        const auto &c = view->channel[i];
        const auto &preset = rf::presets[view->plan.ids[i]];
        std::snprintf(s, sizeof(s), "%u  %s  %u MHz", i + 1, preset.name, preset.mhz);
        button_text(channel_buttons[i], s);
        button_text(search_channel_buttons[i], s);
        if (reference_busy()) {
            lv_obj_add_state(channel_buttons[i], LV_STATE_DISABLED);
            lv_obj_add_state(search_channel_buttons[i], LV_STATE_DISABLED);
        } else {
            lv_obj_remove_state(channel_buttons[i], LV_STATE_DISABLED);
            lv_obj_remove_state(search_channel_buttons[i], LV_STATE_DISABLED);
        }
        const bool current =
            view->mode == rf::Mode::Monitor && view->profile_ok && c.valid && now - c.host_us < rf::fresh_us;
        const bool recent_bad = !show_search && c.bad_us && now - c.bad_us < 1000000;
        if (current && !recent_bad) {
            ++live;
            const int base = view->background_active ? view->background.monitor[i] : 0;
            std::snprintf(s, sizeof(s), "%+.1f", (c.filtered - base) / 100.f);
            text(values[i], s);
            text(units[i], view->background_active ? "F-BG dB" : "FILT dB");
            char peak_text[24];
            if (c.peak == rf::missing)
                std::snprintf(peak_text, sizeof(peak_text), "--.-");
            else
                std::snprintf(peak_text, sizeof(peak_text), "%+.1f", c.peak / 100.f);
            std::snprintf(s, sizeof(s), "RAW %+.1f   PK %s", c.raw / 100.f, peak_text);
            text(raw_labels[i], s);
            text(states[i], c.alert ? "予定外上昇" : view->background_active ? "測定中" : "背景未取得");
        } else {
            text(values[i], "--.-");
            text(units[i], view->background_active ? "F-BG dB" : "FILT dB");
            text(raw_labels[i], "RAW --.-   PK --.-");
            text(states[i], recent_bad              ? "測定注意"
                            : view->background_busy ? "背景記録中"
                            : show_search           ? "監視中断"
                                                    : "未取得");
        }
        if (!show_search && recent_bad) {
            if (c.last_issues & rf::Clipped)
                std::snprintf(s, sizeof(s), "飽和 CLIP%u F%02X", c.last_bad_clips, c.last_bad_flags);
            else if (c.last_issues & rf::GainMoved)
                std::snprintf(s, sizeof(s), "GAIN不一致 %u F%02X", c.last_bad_gain, c.last_bad_flags);
            else if (c.last_issues & rf::CaptureFailed)
                std::snprintf(s, sizeof(s), "RF取得失敗 F%02X", c.last_bad_flags);
            else if (c.last_issues & rf::Fallback)
                std::snprintf(s, sizeof(s), "代替取得 F%02X", c.last_bad_flags);
            else
                std::snprintf(s, sizeof(s), "RAW/FILT 欠測");
            text(states[i], s);
        } else if (!show_search && transport_bad) {
            const auto f = view->transport_flags;
            text(states[i], f & rf::CrcError ? "USB CRC" : f & rf::Malformed ? "FRAME ERROR" :
                 f & (rf::StreamReset | rf::ClockReset) ? "C5 RESET" : f & rf::SequenceGap ? "SEQ GAP" :
                 f & rf::ChipDrop ? "C5 DROP" : "DUPLICATE");
        }
        lv_obj_set_style_text_color(states[i], lv_color_hex(c.alert || recent_bad || transport_bad ? AMBER : DIM), 0);
        const auto evaluation = rf::visible_normal(*view, i, now);
        health_line(s, sizeof(s), evaluation, false);
        text(level_labels[i], s);
        lv_obj_set_style_text_color(level_labels[i], lv_color_hex(grade_color(evaluation.level)), 0);
        health_line(s, sizeof(s), evaluation, true);
        text(stability_labels[i], s);
        lv_obj_set_style_text_color(stability_labels[i], lv_color_hex(grade_color(evaluation.stability)), 0);
        const auto worst = std::max(evaluation.level, evaluation.stability);
        lv_obj_set_style_border_color(
            card_panels[i], lv_color_hex(worst == signal_health::Grade::Gray ? EDGE : grade_color(worst)), 0);
        text(states[i], c.alert                                 ? "予定外上昇：別途確認"
                        : worst == signal_health::Grade::Red    ? "優先確認：受信状態"
                        : worst == signal_health::Grade::Yellow ? "要確認：正常時から変化"
                        : worst == signal_health::Grade::Green  ? "基準と同条件での目安"
                                                                : "正常基準・受信条件を確認");
        if (reference_busy())
            lv_obj_add_state(normal_buttons[i], LV_STATE_DISABLED);
        else
            lv_obj_remove_state(normal_buttons[i], LV_STATE_DISABLED);
        any_alert |= c.alert;
        any_bad |= recent_bad;
        button_text(plan_buttons[i], c.planned ? "予定ON" : "予定OFF");
        lv_obj_set_style_border_color(plan_buttons[i], lv_color_hex(c.planned ? DIM : EDGE), 0);
    }
    std::snprintf(s, sizeof(s), "%s ___  %s ---  %s ...  %s -.-", rf::presets[view->plan.ids[0]].name,
                  rf::presets[view->plan.ids[1]].name, rf::presets[view->plan.ids[2]].name,
                  rf::presets[view->plan.ids[3]].name);
    text(history_legend, s);
    if (!view->usb)
        std::snprintf(s, sizeof(s), "C5 接続待ち");
    else if (view->mode == rf::Mode::Fault)
        std::snprintf(s, sizeof(s), "処理停止 / 要確認");
    else if (view->mode == rf::Mode::Restore)
        std::snprintf(s, sizeof(s), "4ch設定を確認中");
    else if (view->normal_capture.busy())
        std::snprintf(s, sizeof(s), "正常基準 / 枠%u", view->normal_capture.slot + 1);
    else if (view->background_busy)
        std::snprintf(s, sizeof(s), "背景記録中 %s", show_search ? "広域" : "4ch");
    else if (show_search)
        std::snprintf(s, sizeof(s), "広域サーチ / 4ch中断");
    else
        std::snprintf(s, sizeof(s), "LIVE %u/4%s", live,
                      any_alert ? " / 要確認"
                      : any_bad ? " / 測定注意"
                                : "");
    text(status, s);
    if (view->error[0])
        text(notice_label, view->error);
    else if (any_alert)
        text(notice_label, "送信予定なしで受信上昇。別送信・受信機の像も確認してください");
    else if (any_bad)
        text(notice_label, "飽和・ゲイン逸脱・取得失敗を観測。受信入力とゲインを確認してください");
    else if (view->mode == rf::Mode::BackgroundMonitor) {
        std::snprintf(s, sizeof(s), "背景記録：4ch %u/%u枠（全機OFFのまま）", view->background_progress,
                      rf::BG_BINS);
        text(notice_label, s);
    } else if (view->mode == rf::Mode::BackgroundSweep) {
        std::snprintf(s, sizeof(s), "背景記録：広域 %u/%u点（全機OFFのまま）", view->sweep_done, rf::SWEEP);
        text(notice_label, s);
    } else if (show_search && view->sweep_invalid) {
        std::snprintf(s, sizeof(s), "直近の掃引で%u点が測定無効。飽和・取得状態を確認してください",
                      view->sweep_invalid);
        text(notice_label, s);
    } else {
        text(notice_label, view->notice);
    }
    lv_obj_set_style_text_color(notice_label,
                                lv_color_hex(view->error[0] || any_alert || any_bad ? AMBER : WHITE), 0);
    text(hint_label, view->background_active ? "相対値・映像品質は未判定 / 近接チャンネルの像に注意"
                                             : "未校正の受信値 dB / 背景は全機OFFで記録してください");
    std::snprintf(s, sizeof(s), "R %lu/s  CRC %lu  SEQ %lu  C5+ %lu  DUP %lu\nUSB %s  CLIP %lu  G! %lu  BG %s",
                  (unsigned long)records_per_second, (unsigned long)view->crc,
                  (unsigned long)view->gaps, (unsigned long)view->drop_increase, (unsigned long)view->duplicates,
                  view->usb ? "ON" : "OFF", (unsigned long)view->clip_samples, (unsigned long)view->gain_samples,
                  view->background_active   ? (view->background_saved ? "SAVED" : "RAM")
                  : view->background_stored ? "STORED"
                                            : "NONE");
    text(health, s);
    if (brightness_open)
        refresh_brightness();
    if (reference_busy()) {
        lv_obj_add_state(background_nav, LV_STATE_DISABLED);
        lv_obj_add_state(monitor_nav, LV_STATE_DISABLED);
        lv_obj_add_state(search_nav, LV_STATE_DISABLED);
        button_text(peak_nav, view->normal_capture.busy() ? "基準記録中断" : "記録中断");
    } else {
        lv_obj_remove_state(background_nav, LV_STATE_DISABLED);
        lv_obj_remove_state(monitor_nav, LV_STATE_DISABLED);
        lv_obj_remove_state(search_nav, LV_STATE_DISABLED);
        button_text(peak_nav, "ピーク消去");
    }
    if (modal_open) {
        uint16_t selected_frequencies[rf::N];
        rf::plan_frequencies(view->plan, selected_frequencies);
        if (view->background_stored && !view->background_busy &&
            rf::compatible(view->background, view->serial, view->firmware, view->settings,
                           selected_frequencies))
            lv_obj_remove_state(adopt_button, LV_STATE_DISABLED);
        else
            lv_obj_add_state(adopt_button, LV_STATE_DISABLED);
    }
    if (show_search)
        text(hint_label, "各枠をタップしてch選択 / WFは1行1掃引（非同時）/ 4ch中断");
    else
        text(hint_label, "数値:FILT平滑値 / 履歴:RAWの100ms電力平均・PK / 判定:同じ機体・設置・出力");
    if (!view->background_active)
        show_delta = false;
    button_text(scale_button, show_delta ? "表示:背景差" : "表示:受信値");
    if (view->background_active)
        lv_obj_remove_state(scale_button, LV_STATE_DISABLED);
    else
        lv_obj_add_state(scale_button, LV_STATE_DISABLED);
    if (show_search) {
        selected = rf::nearest_sweep_index(selected, view->sweep_stride);
        std::snprintf(s, sizeof(s), "5645-5917 MHz / %s dB / %u MHz刻み",
                      view->background_active && show_delta ? "基準比" : "受信値", 2 * view->sweep_stride);
        text(search_title, s);
        button_text(step_button, view->background_busy      ? "背景記録中 2MHz"
                                 : view->search_stride == 2 ? "高速 4MHz / 詳細へ"
                                                            : "詳細 2MHz / 高速へ");
        if (view->background_busy)
            lv_obj_add_state(step_button, LV_STATE_DISABLED);
        else
            lv_obj_remove_state(step_button, LV_STATE_DISABLED);
        const float age = view->sweep_completed_us ? (now - view->sweep_completed_us) / 1e6f : -1;
        std::snprintf(s, sizeof(s), "%u/%u点  #%u  %.2fs  age %.1fs", view->sweep_done,
                      rf::sweep_count(view->sweep_stride), view->sweeps, view->sweep_seconds, age);
        text(sweep_status, s);
        const auto v = view->sweep[selected], p = view->sweep_peak[selected];
        if (v != rf::missing) {
            char peak_text[24];
            if (p == rf::missing)
                std::snprintf(peak_text, sizeof(peak_text), "--.-");
            else
                std::snprintf(peak_text, sizeof(peak_text), "%+.2f", p / 100.f);
            const float agep = (now - view->sweep_time[selected]) / 1e6f;
            if (view->background_active)
                std::snprintf(s, sizeof(s), "%u MHz  RAW %+.2f dB  %.1f秒前\n基準比 %+.2f dB  PK %s",
                              rf::sweep_frequency(selected), v / 100.f, agep,
                              (v - view->background.sweep[selected]) / 100.f, peak_text);
            else
                std::snprintf(s, sizeof(s), "%u MHz  RAW %+.2f dB  PK %s\n%.1f秒前 / 背景未取得",
                              rf::sweep_frequency(selected), v / 100.f, peak_text, agep);
        } else
            std::snprintf(s, sizeof(s), "%u MHz / 未取得・測定無効", rf::sweep_frequency(selected));
        text(cursor_label, s);
    }
    if (normal_open)
        refresh_normal();
    if (++ticks % 2 == 0) {
        const int64_t draw_started = esp_timer_get_time();
        if (show_search) {
            spectrum_plot();
            if (view->sweeps != last_water_sweeps || view->row_count != last_water_count ||
                view->background.revision != last_water_revision || show_delta != last_water_delta ||
                view->background_active != last_water_active || view->sweep_stride != last_water_stride) {
                waterfall_plot(now);
                last_water_sweeps = view->sweeps;
                last_water_count = view->row_count;
                last_water_revision = view->background.revision;
                last_water_delta = show_delta;
                last_water_active = view->background_active;
                last_water_stride = view->sweep_stride;
            }
        } else
            history_plot(now);
        last_draw_us = unsigned(esp_timer_get_time() - draw_started);
        max_draw_us = std::max(max_draw_us, last_draw_us);
    }
    last_ui_us = unsigned(esp_timer_get_time() - update_started);
}
} // namespace
void app_build_ui(lv_display_t *display) {
    view = new rf::Data;
    assert(view);
    bsp_display_rotate(display, LV_DISPLAY_ROTATION_0);
    assert(lv_display_get_horizontal_resolution(display) == SW);
    assert(lv_display_get_vertical_resolution(display) == SCREEN_H);
    screen = lv_screen_active();
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    char title[80];
    std::snprintf(title, sizeof(title), "RF FIELD MONITOR / %s", esp_app_get_description()->version);
    label(screen, title, 20, 16, 680, &lv_font_montserrat_28);
    status = label(screen, "C5 接続待ち", 20, 56, 680, &rf_jp20);
    monitor_nav =
        button(screen, "4ch履歴", 192, 94, 160, 48, [](lv_event_t *) { app_post(ActionType::Monitor); });
    search_nav =
        button(screen, "帯域 / WF", 20, 94, 160, 48, [](lv_event_t *) { app_post(ActionType::Search); });
    background_nav = button(screen, "背景記録", 364, 94, 160, 48, [](lv_event_t *) { open_modal(); });
    peak_nav = button(screen, "ピーク消去", 20, 154, 210, 48, [](lv_event_t *) {
        app_post(view->normal_capture.busy() ? ActionType::CancelNormal
                 : view->background_busy     ? ActionType::Monitor
                                             : ActionType::ResetPeaks);
    });
    imd_button = button(screen, "IMD候補：OFF", 242, 154, 220, 48, [](lv_event_t *) {
        show_imd = !show_imd;
        button_text(imd_button, show_imd ? "IMD候補：ON" : "IMD候補：OFF");
    });
    lv_obj_add_flag(imd_button, LV_OBJ_FLAG_HIDDEN);
    scale_button = button(screen, "表示:受信値", 474, 154, 226, 48, [](lv_event_t *) {
        if (view->background_active)
            show_delta = !show_delta;
    });
    lv_obj_add_flag(scale_button, LV_OBJ_FLAG_HIDDEN);
    button(screen, "明るさ", 536, 94, 164, 48, [](lv_event_t *) {
        refresh_brightness();
        lv_obj_remove_flag(brightness_modal, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(brightness_modal);
        brightness_open = true;
    });
    monitor = lv_obj_create(screen);
    lv_obj_remove_style_all(monitor);
    lv_obj_set_pos(monitor, 0, 218);
    lv_obj_set_size(monitor, SW, 910);
    lv_obj_remove_flag(monitor, LV_OBJ_FLAG_SCROLLABLE);
    for (unsigned i = 0; i < 4; i++) {
        auto *card = panel(monitor, 20 + 350 * (i % 2), 280 * (i / 2), 330, 260);
        card_panels[i] = card;
        channel_buttons[i] = button(
            card, "チャンネル選択", 14, 6, 302, 40,
            [](lv_event_t *e) {
                open_picker(unsigned(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e))));
            },
            reinterpret_cast<void *>(uintptr_t(i)));
        lv_obj_set_style_text_color(lv_obj_get_child(channel_buttons[i], 0), lv_color_hex(traces[i]), 0);
        values[i] = label(card, "--.-", 14, 50, 180, &lv_font_montserrat_40);
        units[i] = label(card, "FILT dB", 184, 76, 100, &rf_jp20, DIM);
        raw_labels[i] = label(card, "RAW --.-   PK --.-", 14, 102, 302, &lv_font_montserrat_20, DIM);
        level_labels[i] = label(card, "レベル 灰:基準なし", 14, 132, 302, &rf_jp20, DIM);
        stability_labels[i] = label(card, "安定性 灰:基準なし", 14, 160, 302, &rf_jp20, DIM);
        states[i] = label(card, "正常基準を記録してください", 14, 188, 302, &rf_jp20, DIM);
        normal_buttons[i] = button(
            card, "正常基準", 14, 218, 150, 34,
            [](lv_event_t *e) {
                open_normal(unsigned(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e))));
            },
            reinterpret_cast<void *>(uintptr_t(i)));
        plan_buttons[i] = button(
            card, "予定ON", 186, 218, 130, 34,
            [](lv_event_t *e) {
                const unsigned index = unsigned(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
                const unsigned v = !view->channel[index].planned;
                app_post(ActionType::Plan, index * 2 + v);
            },
            reinterpret_cast<void *>(uintptr_t(i)));
    }
    history_title = label(monitor, "60秒履歴 / 受信値 dB", 20, 562, 680, &rf_jp20, DIM);
    history_legend = label(monitor, "", 84, 592, 608, &lv_font_montserrat_20, DIM);
    bind_canvas(monitor, history_canvas, history_buf, history_pixels, 84, 628, HH);
    for (unsigned i = 0; i < 4; i++)
        axis[i] = label(monitor, "", 20, 620 + int(i) * HH / 3, 60, &lv_font_montserrat_20, DIM);
    label(monitor, "-60 s", 84, 858, 150, &lv_font_montserrat_20, DIM);
    label(monitor, "-30", 360, 858, 100, &lv_font_montserrat_20, DIM);
    label(monitor, "NOW", 620, 858, 80, &lv_font_montserrat_20, DIM);
    search = lv_obj_create(screen);
    lv_obj_remove_style_all(search);
    lv_obj_set_pos(search, 0, 218);
    lv_obj_set_size(search, SW, 910);
    lv_obj_remove_flag(search, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(search, LV_OBJ_FLAG_HIDDEN);
    search_title = label(search, "5645-5917 MHz / 約10 MHz受信窓", 20, 0, 680, &rf_jp20);
    step_button = button(search, "高速 4MHz / 詳細へ", 20, 40, 680, 48, [](lv_event_t *) {
        if (!view->background_busy)
            app_post(ActionType::SearchStep, view->search_stride == 2 ? 2 : 4);
    });
    for (unsigned i = 0; i < rf::N; i++) {
        search_channel_buttons[i] = button(
            search, "チャンネル選択", 20 + int(i % 2) * 350, 100 + int(i / 2) * 54, 330, 42,
            [](lv_event_t *e) {
                open_picker(unsigned(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e))));
            },
            reinterpret_cast<void *>(uintptr_t(i)));
        lv_obj_set_style_text_color(lv_obj_get_child(search_channel_buttons[i], 0), lv_color_hex(traces[i]),
                                    0);
    }
    bind_canvas(search, spectrum_canvas, spectrum_buf, spectrum_pixels, 84, 220, SH);
    lv_obj_add_flag(spectrum_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        spectrum_canvas,
        [](lv_event_t *) {
            auto *input = lv_indev_active();
            if (!input)
                return;
            lv_point_t p;
            lv_indev_get_point(input, &p);
            lv_area_t a;
            lv_obj_get_coords(spectrum_canvas, &a);
            selected = rf::nearest_sweep_index(
                std::clamp<int>((p.x - a.x1) * int(rf::SWEEP - 1) / (PW - 1), 0, rf::SWEEP - 1),
                view->sweep_stride);
        },
        LV_EVENT_PRESSED, nullptr);
    for (unsigned i = 0; i < 4; i++) {
        char value[20];
        std::snprintf(value, sizeof(value), "%+d", 50 - int(i) * 20);
        label(search, value, 20, 212 + int(i) * SH / 3, 60, &lv_font_montserrat_20, DIM);
    }
    label(search, "5645", 84, 394, 120, &lv_font_montserrat_20, DIM);
    label(search, "5780", 350, 394, 100, &lv_font_montserrat_20, DIM);
    label(search, "5917 MHz", 580, 394, 112, &lv_font_montserrat_20, DIM);
    cursor_label = label(search, "未取得", 20, 430, 680, &rf_jp20);
    label(search, "WF / 1行=1掃引 / 色線:ch・濃淡:受信", 20, 494, 680, &rf_jp20, DIM);
    bind_canvas(search, water_canvas, water_buf, water_pixels, 84, 530, WH);
    water_time = label(search, "掃引完了後に履歴を表示", 20, 846, 680, &rf_jp20, DIM);
    sweep_status = label(search, "", 20, 878, 680, &rf_jp20, DIM);
    notice_label = label(screen, "C5の接続を待っています", 20, 1130, 680, &rf_jp20);
    hint_label = label(screen, "未校正の受信値 dB", 20, 1180, 680, &rf_jp20, DIM);
    health = label(screen, "USB OFF", 20, 1230, 680, &lv_font_montserrat_20, DIM);
    lv_label_set_long_mode(cursor_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(cursor_label, 56);
    lv_label_set_long_mode(notice_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(notice_label, 48);
    lv_label_set_long_mode(hint_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_height(hint_label, 48);
    lv_obj_set_height(health, 48);
    build_modal();
    build_picker();
    build_normal();
    build_brightness();
    lv_timer_create(update, 100, nullptr);
    ESP_LOGI("RF4_UI",
             "UI ready: industrial square layout, Japanese subset, 720x1280 native portrait; plot buffers in PSRAM");
}
void app_ui_diagnostics() {
    bsp_display_lock(0);
    ESP_LOGI("RF4_UI", "screen=%ldx%ld rotation=%d brightness=%u brightness_open=%d canvas_bytes=%u",
             (long)lv_display_get_horizontal_resolution(nullptr), (long)lv_display_get_vertical_resolution(nullptr),
             int(lv_display_get_rotation(nullptr)), app_brightness(), brightness_open, PW * (HH + SH + WH) * 2);
    lv_mem_monitor_t m{};
    lv_mem_monitor(&m);
    ESP_LOGI(
        "RF4_UI",
        "heap total=%u free=%u biggest=%u search=%d modal=%d ticks=%u draw_us=%lu max_draw_us=%lu ui_us=%lu",
        (unsigned)m.total_size, (unsigned)m.free_size, (unsigned)m.free_biggest_size, show_search, modal_open,
        ticks, (unsigned long)last_draw_us, (unsigned long)max_draw_us, (unsigned long)last_ui_us);
    ESP_LOGI("RF4_UI", "picker_open=%d slot=%u band=%u normal_open=%d normal_slot=%u", picker_open,
             picker_slot, picker_band, normal_open, normal_slot);
    for (unsigned i = 0; i < 4; i++)
        ESP_LOGI("RF4_UI", "card%u channel=%s value=%s state=%s", i,
                 lv_label_get_text(lv_obj_get_child(channel_buttons[i], 0)), lv_label_get_text(values[i]),
                 lv_label_get_text(states[i]));
    for (unsigned i = 0; i < 4; i++)
        ESP_LOGI("RF4_UI", "health%u level=%s stability=%s", i, lv_label_get_text(level_labels[i]),
                 lv_label_get_text(stability_labels[i]));
    ESP_LOGI("RF4_UI", "unit0=%s history=%s diagnostics=%s", lv_label_get_text(units[0]),
             lv_label_get_text(history_title), lv_label_get_text(health));
    bsp_display_unlock();
}
