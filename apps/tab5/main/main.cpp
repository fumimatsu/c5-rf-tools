#include "app_api.h"
#include "bsp/display.h"
#include "bsp/m5stack_tab5.h"
#include "decoder_self_test.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "usb/cdc_acm_host.h"
#include "usb/usb_host.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

static const char *TAG = "RF4";
static rf::Model *model;
static SemaphoreHandle_t mutex, disconnected;
static QueueHandle_t actions;
static c5::Decoder decoder;
static bool have_sequence = false, nvs_ok = false;
static uint16_t sequence = 0;
struct Lock {
    Lock() {
        xSemaphoreTake(mutex, portMAX_DELAY);
    }
    ~Lock() {
        xSemaphoreGive(mutex);
    }
};
bool app_post(ActionType type, unsigned value) {
    const Action a{type, value, esp_timer_get_time()};
    return xQueueSend(actions, &a, 0) == pdTRUE;
}
void app_copy_data(rf::Data *out) {
    Lock lock;
    *out = model->data;
}
static void notice(const char *s) {
    std::snprintf(model->data.notice, sizeof(model->data.notice), "%s", s);
}
static void error_text(const char *s) {
    std::snprintf(model->data.error, sizeof(model->data.error), "%s", s);
}
static const char *line(const char *text, const char *prefix) {
    const size_t n = std::strlen(prefix);
    const char *p = text;
    while (*p) {
        if (!std::strncmp(p, prefix, n))
            return p;
        p = std::strchr(p, '\n');
        if (!p)
            break;
        ++p;
    }
    return nullptr;
}
static void text_frame(const uint8_t *bytes, size_t n) {
    char text[2048];
    const size_t len = std::min(n, sizeof(text) - 1);
    std::memcpy(text, bytes, len);
    text[len] = 0;
    auto &d = model->data;
    if (const char *v = std::strstr(text, "firmware ")) {
        char version[32]{};
        std::sscanf(v, "firmware %31s", version);
        d.firmware_ok = std::strstr(version, "+xiaoc5") || std::strstr(version, "+c5zero");
        if (d.firmware_ok)
            std::snprintf(d.firmware, sizeof(d.firmware), "%s", version);
    }
    if (std::strstr(text, "wifi="))
        d.radio =
            std::strstr(text, "wifi=1") && std::strstr(text, "-> live") && !std::strstr(text, "NOT live");
    if (const char *p = line(text, "pilots ")) {
        p += 7;
        unsigned count = 0;
        uint16_t values[8]{};
        bool good = true;
        while (*p && *p != '\n' && *p != '\r') {
            if (count == 8) {
                good = false;
                break;
            }
            char *end = nullptr;
            const long f = std::strtol(p, &end, 10);
            if (end == p || f < 5180 || f > 5917) {
                good = false;
                break;
            }
            values[count++] = uint16_t(f);
            p = end;
            if (*p == ',')
                ++p;
            else if (*p && *p != '\n' && *p != '\r') {
                good = false;
                break;
            }
        }
        if (good && count) {
            std::copy_n(values, count, d.applied);
            d.applied_count = count;
            ++d.settings_revision;
        }
    }
    if (const char *p = line(text, "gain ")) {
        int g, o;
        if (std::sscanf(p, "gain %d (max %*d)  offset %d", &g, &o) == 2) {
            if (d.settings.gain >= 0 && (d.settings.gain != g || d.settings.offset != o))
                model->invalidate_background("受信条件が変化。背景を取り直してください");
            d.settings.gain = g;
            d.settings.offset = o;
        }
    }
    if (const char *p = line(text, "scanmode ")) {
        char scanmode[12]{}, tune[12]{}, dump[12]{}, scan[12]{};
        int bw = -1;
        if (std::sscanf(p, "scanmode %11s  tune %11s  dump %11s  bw %d  scan %11s", scanmode, tune, dump, &bw,
                        scan) == 5) {
            if (d.settings.bw >= 0 && d.settings.bw != bw)
                model->invalidate_background("帯域設定が変化。背景を再確認してください");
            d.settings.fast = !std::strcmp(scanmode, "fast");
            d.settings.bw = bw;
            d.settings.scan = !std::strcmp(scan, "on");
        }
    }
    if (const char *p = line(text, "node scan n ")) {
        int n = 0;
        if (std::sscanf(p, "node scan n %d", &n) == 1)
            d.settings.noden = n;
    }
}
static void on_frame(uint8_t type, const uint8_t *p, size_t n, void *) {
    Lock lock;
    auto &d = model->data;
    if (type == 'T') {
        text_frame(p, n);
        return;
    }
    if (type != 'R')
        return;
    c5::Record record{};
    if (!c5::read_record(p, n, record)) {
        ++d.malformed;
        model->bg_failed = true;
        return;
    }
    if (have_sequence) {
        const uint16_t step = uint16_t(record.seq - sequence);
        if (step && step < 32768) {
            d.gaps += step - 1;
            if (step > 1)
                model->bg_failed = true;
        } else {
            ++d.resync;
            model->bg_failed = true;
        }
    }
    sequence = record.seq;
    have_sequence = true;
    model->ingest(record, esp_timer_get_time());
}
static bool receive(const uint8_t *p, size_t n, void *) {
    {
        Lock lock;
        model->data.bytes += n;
    }
    decoder.feed(p, n, on_frame, nullptr);
    {
        Lock lock;
        auto &d = model->data;
        if (d.crc != decoder.crc_errors || decoder.bad_lengths)
            model->bg_failed = true;
        d.crc = decoder.crc_errors;
        d.malformed += decoder.bad_lengths;
        decoder.bad_lengths = 0;
    }
    return true;
}
static void usb_event(const cdc_acm_host_dev_event_data_t *e, void *) {
    if (e->type == CDC_ACM_HOST_DEVICE_DISCONNECTED || e->type == CDC_ACM_HOST_ERROR) {
        {
            Lock lock;
            auto &d = model->data;
            d.usb = false;
            d.profile_ok = false;
            for (auto &c : d.channel)
                c.valid = false;
            model->invalidate_background("USB切断。設置条件と背景を再確認してください");
            error_text("C5とのUSB接続が切れました");
        }
        xSemaphoreGive(disconnected);
    }
}
static void new_device(usb_device_handle_t device) {
    const usb_device_desc_t *desc = nullptr;
    usb_device_info_t info{};
    if (usb_host_get_device_descriptor(device, &desc) != ESP_OK || !desc || desc->idVendor != 0x303a ||
        desc->idProduct != 0x1001)
        return;
    if (usb_host_device_info(device, &info) != ESP_OK || !info.str_desc_serial_num)
        return;
    char serial[40]{};
    const unsigned count =
        info.str_desc_serial_num->bLength >= 2
            ? std::min<unsigned>((info.str_desc_serial_num->bLength - 2) / 2, sizeof(serial) - 1)
            : 0;
    for (unsigned i = 0; i < count; i++) {
        const auto c = info.str_desc_serial_num->wData[i];
        serial[i] = c >= 32 && c < 127 ? char(c) : '?';
    }
    {
        Lock lock;
        std::snprintf(model->data.serial, sizeof(model->data.serial), "%s", serial);
    }
    ESP_LOGI(TAG, "C5 USB serial=%s", serial);
}
static esp_err_t command(cdc_acm_dev_hdl_t device, const char *text) {
    return cdc_acm_host_data_tx_blocking(device, reinterpret_cast<const uint8_t *>(text), std::strlen(text),
                                         1000);
}
static esp_err_t profile(cdc_acm_dev_hdl_t device, const uint16_t *frequencies, unsigned count,
                         int64_t settle_us = 120000) {
    char text[180];
    size_t used = std::snprintf(text, sizeof(text), "pilots ");
    for (unsigned i = 0; i < count; i++)
        used += std::snprintf(text + used, sizeof(text) - used, "%s%u", i ? "," : "", frequencies[i]);
    std::snprintf(text + used, sizeof(text) - used, "\nstatus\n");
    {
        Lock lock;
        model->set_profile(frequencies, count, esp_timer_get_time(), settle_us);
    }
    ESP_LOGI(TAG, "SET_PROFILE count=%u first=%u last=%u", count, frequencies[0], frequencies[count - 1]);
    return command(device, text);
}
static bool store_background(const rf::Background &background) {
    if (!nvs_ok)
        return false;
    nvs_handle_t handle;
    esp_err_t e = nvs_open("rf4", NVS_READWRITE, &handle);
    if (e != ESP_OK)
        return false;
    e = nvs_set_blob(handle, "background", &background, sizeof(background));
    if (e == ESP_OK)
        e = nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "BACKGROUND_SAVE result=%s revision=%lu", esp_err_to_name(e),
             (unsigned long)background.revision);
    return e == ESP_OK;
}
static bool store_normal(const signal_health::Reference &ref) {
    if (!nvs_ok || !signal_health::valid(ref))
        return false;
    char key[16];
    std::snprintf(key, sizeof(key), "normal%u", ref.mhz);
    nvs_handle_t h;
    esp_err_t e = nvs_open("rf4", NVS_READWRITE, &h);
    if (e != ESP_OK)
        return false;
    e = nvs_set_blob(h, key, &ref, sizeof(ref));
    if (e == ESP_OK)
        e = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "NORMAL_SAVE mhz=%u result=%s", ref.mhz, esp_err_to_name(e));
    return e == ESP_OK;
}
static void load_normal() {
    if (!nvs_ok)
        return;
    nvs_handle_t h;
    if (nvs_open("rf4", NVS_READONLY, &h) != ESP_OK)
        return;
    unsigned count = 0;
    for (unsigned i = 0; i < rf::PRESETS; i++) {
        if (!rf::preset_enabled(i) || rf::normal_index(rf::presets[i].mhz) != i)
            continue;
        signal_health::Reference ref;
        size_t size = sizeof(ref);
        char key[16];
        std::snprintf(key, sizeof(key), "normal%u", rf::presets[i].mhz);
        const esp_err_t e = nvs_get_blob(h, key, &ref, &size);
        if (e == ESP_OK && size == sizeof(ref) && signal_health::valid(ref) &&
            ref.mhz == rf::presets[i].mhz) {
            model->data.normal_refs[i] = ref;
            ++count;
        } else if (e != ESP_ERR_NVS_NOT_FOUND)
            ESP_LOGE(TAG, "NORMAL_LOAD invalid key=%s error=%s", key, esp_err_to_name(e));
    }
    nvs_close(h);
    ESP_LOGI(TAG, "NORMAL_LOADED count=%u (inactive)", count);
}
static bool store_channels(const rf::ChannelPlan &plan) {
    if (!nvs_ok || !rf::plan_valid(plan))
        return false;
    nvs_handle_t h;
    esp_err_t e = nvs_open("rf4", NVS_READWRITE, &h);
    if (e != ESP_OK)
        return false;
    e = nvs_set_blob(h, "channels", &plan, sizeof(plan));
    if (e == ESP_OK)
        e = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "CHANNELS_SAVE result=%s", esp_err_to_name(e));
    return e == ESP_OK;
}
static void load_channels() {
    if (!nvs_ok)
        return;
    nvs_handle_t h;
    if (nvs_open("rf4", NVS_READONLY, &h) != ESP_OK)
        return;
    rf::ChannelPlan plan;
    size_t size = sizeof(plan);
    const esp_err_t e = nvs_get_blob(h, "channels", &plan, &size);
    nvs_close(h);
    if (e == ESP_OK && size == sizeof(plan) && rf::plan_valid(plan)) {
        model->data.plan = plan;
        ESP_LOGI(TAG, "CHANNELS_LOADED %s,%s,%s,%s", rf::presets[plan.ids[0]].name,
                 rf::presets[plan.ids[1]].name, rf::presets[plan.ids[2]].name, rf::presets[plan.ids[3]].name);
    } else if (e != ESP_ERR_NVS_NOT_FOUND) {
        notice("保存チャンネルが不正。初期設定を使用しています");
        ESP_LOGE(TAG, "CHANNELS_LOAD invalid result=%s", esp_err_to_name(e));
    }
}
static void load_background() {
    const esp_err_t e = nvs_flash_init();
    nvs_ok = e == ESP_OK;
    model->data.background_persistence_ok = nvs_ok;
    if (!nvs_ok) {
        ESP_LOGE(TAG, "NVS init %s; no erase performed", esp_err_to_name(e));
        return;
    }
    nvs_handle_t h;
    if (nvs_open("rf4", NVS_READONLY, &h) != ESP_OK)
        return;
    size_t size = 0;
    esp_err_t read = nvs_get_blob(h, "background", nullptr, &size);
    auto *b = new rf::Background{};
    bool valid = false;
    if (read == ESP_OK && size == sizeof(*b)) {
        read = nvs_get_blob(h, "background", b, &size);
        valid = read == ESP_OK && rf::background_valid(*b);
    } else if (read == ESP_OK && size == sizeof(rf::BackgroundV1)) {
        rf::BackgroundV1 old{};
        read = nvs_get_blob(h, "background", &old, &size);
        valid = read == ESP_OK && rf::upgrade_background(old, *b);
        if (valid)
            ESP_LOGI(TAG, "BACKGROUND_UPGRADED v1->v2 in RAM; original NVS retained");
    }
    nvs_close(h);
    if (valid) {
        model->data.background = *b;
        model->data.background_stored = true;
        model->data.background_saved = true;
        notice("保存背景あり。設置条件を確認して使用してください");
        ESP_LOGI(TAG, "BACKGROUND_LOADED revision=%lu serial=%s (inactive)", (unsigned long)b->revision,
                 b->serial);
    } else if (read != ESP_ERR_NVS_NOT_FOUND)
        notice("保存背景が未対応または破損。新しく記録してください");
    delete b;
}
static bool clear_background() {
    if (!nvs_ok)
        return false;
    nvs_handle_t h;
    esp_err_t e = nvs_open("rf4", NVS_READWRITE, &h);
    if (e != ESP_OK)
        return false;
    e = nvs_erase_key(h, "background");
    if (e == ESP_ERR_NVS_NOT_FOUND)
        e = ESP_OK;
    if (e == ESP_OK)
        e = nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}
enum class Stage { AwaitMonitor, Monitor, Background, AwaitGroup, CollectGroup, Fault };
struct Work {
    Stage stage = Stage::AwaitMonitor;
    bool calibrating = false, group_failed = false, search_after_restore = true, cancelled_background = false;
    unsigned group = 0, stride = 2, normal_after_restore = 0;
    bool normal_error_reported = false;
    int64_t requested_us = 0, collect_us = 0, sweep_us = 0;
    rf::Background candidate{};
};
static esp_err_t restore(cdc_acm_dev_hdl_t device, Work &w, bool calibration) {
    w.calibrating = calibration;
    w.stage = Stage::AwaitMonitor;
    w.requested_us = esp_timer_get_time();
    {
        Lock lock;
        auto &d = model->data;
        d.mode = rf::Mode::Restore;
        d.mode_started_us = w.requested_us;
        d.background_busy = calibration;
        for (auto &c : d.channel)
            c.alert = false;
    }
    uint16_t selected[rf::N];
    {
        Lock lock;
        rf::plan_frequencies(model->data.plan, selected);
    }
    return profile(device, selected, rf::N);
}
static esp_err_t group_start(cdc_acm_dev_hdl_t device, Work &w) {
    uint16_t list[8];
    for (unsigned i = 0; i < 8; i++)
        list[i] = rf::sweep_frequency(rf::sweep_index(w.group + i, w.stride));
    w.stage = Stage::AwaitGroup;
    w.requested_us = esp_timer_get_time();
    return profile(device, list, 8, w.calibrating ? 120000 : 25000);
}
static esp_err_t sweep_start(cdc_acm_dev_hdl_t device, Work &w, bool calibration) {
    w.calibrating = calibration;
    w.group = 0;
    w.group_failed = false;
    w.sweep_us = esp_timer_get_time();
    {
        Lock lock;
        auto &d = model->data;
        d.mode = calibration ? rf::Mode::BackgroundSweep : rf::Mode::Search;
        d.mode_started_us = w.sweep_us;
        w.stride = calibration ? 1 : d.search_stride;
        model->set_sweep_stride(w.stride);
        model->begin_sweep();
        for (auto &c : d.channel)
            c.alert = false;
    }
    return group_start(device, w);
}
static void fail(Work &w, const char *error) {
    w.stage = Stage::Fault;
    w.calibrating = false;
    Lock lock;
    auto &d = model->data;
    d.mode = rf::Mode::Fault;
    d.profile_ok = false;
    d.background_busy = false;
    if (d.normal_capture.busy())
        d.normal_capture.fail(signal_health::Error::Conditions);
    model->reset_normal();
    error_text(error);
    notice("処理を停止しました。4ch監視で再試行できます");
    ESP_LOGE(TAG, "FAULT %s", error);
}
static esp_err_t action(cdc_acm_dev_hdl_t device, Work &w, Action a) {
    ESP_LOGI(TAG, "ACTION type=%u value=%u age_ms=%lld", unsigned(a.type), a.value,
             (long long)((esp_timer_get_time() - a.issued_us) / 1000));
    if ((a.type == ActionType::Background || a.type == ActionType::AdoptBackground ||
         a.type == ActionType::NormalOff || a.type == ActionType::NormalOn ||
         a.type == ActionType::AdoptNormal) &&
        esp_timer_get_time() - a.issued_us > 2000000) {
        Lock lock;
        notice("接続状態が変わったため、背景の確認操作をやり直してください");
        ESP_LOGW(TAG, "Background confirmation expired");
        return ESP_OK;
    }
    if (a.type == ActionType::CancelNormal) {
        Lock lock;
        auto &d = model->data;
        d.normal_capture.phase = signal_health::Phase::Cancelled;
        d.normal_capture.error = signal_health::Error::Cancelled;
        w.normal_after_restore = 0;
        model->reset_normal();
        notice("正常基準の記録を中断しました。保存済み基準は保持されています");
        return ESP_OK;
    }
    {
        Lock lock;
        if (model->data.normal_capture.busy() && a.type != ActionType::NormalOn) {
            notice("正常基準の記録中です。先に基準記録を中断してください");
            return ESP_OK;
        }
    }
    if (a.type == ActionType::NormalOff || a.type == ActionType::NormalOn ||
        a.type == ActionType::AdoptNormal) {
        if (a.value >= rf::N)
            return ESP_OK;
        Lock lock;
        auto &d = model->data;
        const unsigned slot = a.value;
        if (d.background_busy || !d.usb || !d.firmware_ok || !d.settings.valid() ||
            rf::duplicate_channel(d, slot)) {
            notice("受信条件とチャンネルの重複を確認してください");
            return ESP_OK;
        }
        if (a.type == ActionType::NormalOn) {
            auto &cap = d.normal_capture;
            if (cap.slot != slot || cap.phase != signal_health::Phase::AwaitOn ||
                !rf::normal_context_matches(d, slot, cap.candidate)) {
                notice("無送信記録からやり直してください");
                return ESP_OK;
            }
            cap.start_on(esp_timer_get_time());
            model->reset_normal();
            notice("対象1台だけON確認済み：正常基準を10秒記録しています");
            return ESP_OK;
        }
        if (a.type == ActionType::AdoptNormal) {
            if (!rf::normal_matches(d, slot, rf::normal_reference(d, slot))) {
                notice("保存正常基準と現在の受信条件が一致しません");
                return ESP_OK;
            }
            d.normal_active[slot] = true;
            d.channel[slot].planned = true;
            model->reset_normal();
            notice("保存正常基準を採用。機体・設置・出力が同じことを確認済み");
        } else {
            signal_health::Reference ref;
            ref.mhz = rf::presets[d.plan.ids[slot]].mhz;
            ref.gain = rf::gain;
            ref.offset = rf::offset;
            ref.bw = rf::bandwidth;
            ref.samples = rf::capture_samples;
            std::snprintf(ref.serial, sizeof(ref.serial), "%s", d.serial);
            std::snprintf(ref.firmware, sizeof(ref.firmware), "%s", d.firmware);
            d.normal_active[slot] = false;
            d.normal_capture.prepare(slot, ref, esp_timer_get_time());
            w.normal_error_reported = false;
            model->reset_normal();
            w.normal_after_restore = slot + 1;
            notice("全機OFF確認済み：4ch設定を確認して無送信を3秒記録します");
        }
    }
    if (a.type == ActionType::NormalOff || a.type == ActionType::AdoptNormal) {
        w.search_after_restore = false;
        return restore(device, w, false);
    }
    if (a.type == ActionType::ClearBackground || a.type == ActionType::AdoptBackground) {
        Lock lock;
        if (model->data.background_busy) {
            notice("背景記録中です。先に4ch履歴で記録を中断してください");
            return ESP_OK;
        }
    }
    if (a.type == ActionType::SetChannel || a.type == ActionType::ChannelSet) {
        rf::ChannelPlan next;
        bool was_search;
        {
            Lock lock;
            if (model->data.background_busy) {
                notice("背景記録中はチャンネルを変更できません");
                return ESP_OK;
            }
            next = model->data.plan;
            was_search = model->data.mode == rf::Mode::Search;
        }
        if (a.type == ActionType::ChannelSet) {
            if (a.value > 1)
                return ESP_OK;
            next = rf::channel_set(a.value);
        } else {
            const unsigned slot = a.value / rf::PRESETS, id = a.value % rf::PRESETS;
            if (slot >= rf::N || !rf::preset_enabled(id))
                return ESP_OK;
            next.ids[slot] = uint8_t(id);
            rf::seal_plan(next);
        }
        {
            Lock lock;
            if (!std::memcmp(next.ids, model->data.plan.ids, rf::N))
                return ESP_OK;
        }
        if (!store_channels(next)) {
            Lock lock;
            notice("チャンネルの保存に失敗。設定は変更していません");
            return ESP_OK;
        }
        {
            Lock lock;
            model->select_channels(next);
            error_text("");
            notice(
                "チャンネルを変更しました。背景は全機OFFで再記録、または一致する保存背景を採用してください");
        }
        w.search_after_restore = was_search;
        return restore(device, w, false);
    }
    if (a.type == ActionType::SearchStep) {
        if (a.value != 2 && a.value != 4)
            return ESP_OK;
        {
            Lock lock;
            if (model->data.background_busy)
                return ESP_OK;
            model->data.search_stride = a.value / 2;
        }
        // Use the same path as the on-screen search button; restart at the left edge.
        a.type = ActionType::Search;
    }
    if (a.type == ActionType::Monitor) {
        w.search_after_restore = false;
        Lock lock;
        w.cancelled_background = model->data.background_busy;
        error_text("");
        notice("4ch設定へ復帰しています");
    }
    if (a.type == ActionType::Monitor)
        return restore(device, w, false);
    if (a.type == ActionType::Search) {
        {
            Lock lock;
            error_text("");
            model->data.background_busy = false;
            notice("広域走査中。4chの連続監視は一時停止");
        }
        return sweep_start(device, w, false);
    }
    if (a.type == ActionType::Background) {
        w.search_after_restore = true;
        {
            Lock lock;
            auto &d = model->data;
            if (!d.serial[0] || !d.firmware_ok || !d.settings.valid()) {
                notice("受信機の個体と設定を確認できるまで背景記録はできません");
                return ESP_OK;
            }
            d.background_active = false;
            error_text("");
            notice("全機OFF確認済み：背景記録を開始します");
            std::memset(&w.candidate, 0, sizeof(w.candidate));
            w.candidate.revision = d.background.revision + 1;
            w.candidate.gain = rf::gain;
            w.candidate.offset = rf::offset;
            w.candidate.bw = rf::bandwidth;
            w.candidate.samples = rf::capture_samples;
            rf::plan_frequencies(d.plan, w.candidate.monitor_mhz);
            std::snprintf(w.candidate.serial, sizeof(w.candidate.serial), "%s", d.serial);
            std::snprintf(w.candidate.firmware, sizeof(w.candidate.firmware), "%s", d.firmware);
        }
        return restore(device, w, true);
    }
    if (a.type == ActionType::AdoptBackground) {
        Lock lock;
        auto &d = model->data;
        uint16_t selected[rf::N];
        rf::plan_frequencies(d.plan, selected);
        if ((d.mode == rf::Mode::Monitor || d.mode == rf::Mode::Search) && d.background_stored &&
            rf::compatible(d.background, d.serial, d.firmware, d.settings, selected)) {
            d.background_active = true;
            model->clear_comparison_views();
            notice("保存背景を採用。設置条件が同じであることを確認済み");
            ESP_LOGI(TAG, "BACKGROUND_ADOPT revision=%lu", (unsigned long)d.background.revision);
        } else
            notice("保存背景と受信条件が一致しないか、4ch監視が未確認です");
    }
    if (a.type == ActionType::ClearBackground) {
        const bool ok = clear_background();
        Lock lock;
        if (ok) {
            auto &d = model->data;
            d.background_stored = false;
            d.background_active = false;
            d.background_saved = false;
            notice("背景を削除しました");
        } else
            notice("背景の削除に失敗しました。保存領域を確認してください");
    }
    if (a.type == ActionType::ResetPeaks) {
        Lock lock;
        for (auto &c : model->data.channel)
            c.peak = rf::missing;
        std::fill_n(model->data.sweep_peak, rf::SWEEP, rf::missing);
        notice("ピーク保持を消去しました");
    }
    if (a.type == ActionType::Plan && a.value < 8) {
        Lock lock;
        const unsigned i = a.value / 2;
        model->data.channel[i].planned = bool(a.value & 1);
        model->data.channel[i].alert = false;
        model->data.channel[i].votes = 0;
        model->normal_monitor[i].reset();
        model->data.normal_result[i] = signal_health::unknown(signal_health::Reason::Warming);
    }
    return ESP_OK;
}
static void heartbeat() {
    char out[900];
    {
        Lock lock;
        const auto &d = model->data;
        unsigned mask = 0;
        const int64_t now = esp_timer_get_time();
        for (unsigned i = 0; i < 4; i++)
            if (d.channel[i].valid && now - d.channel[i].host_us < rf::fresh_us)
                mask |= 1u << i;
        std::snprintf(
            out, sizeof(out),
            "mode=%s cfg=%d profile=%d count=%u mask=%x serial=%s bg=%d stored=%d saved=%d progress=%u "
            "sweep=%u/%u step_mhz=%u rows=%u rounds=%u crc=%lu gaps=%lu drops=%lu bad=%lu "
            "levels=[%.2f,%.2f,%.2f,%.2f]",
            rf::mode_name(d.mode), d.settings.valid(), d.profile_ok, d.expected_count, mask, d.serial,
            d.background_active, d.background_stored, d.background_saved, d.background_progress, d.sweep_done,
            rf::sweep_count(d.sweep_stride), 2 * d.sweep_stride, d.row_count, d.sweeps, (unsigned long)d.crc,
            (unsigned long)d.gaps, (unsigned long)d.drops, (unsigned long)d.bad_samples,
            d.channel[0].filtered / 100.f, d.channel[1].filtered / 100.f, d.channel[2].filtered / 100.f,
            d.channel[3].filtered / 100.f);
    }
    ESP_LOGI(TAG, "%s", out);
}
static void receiver_task(void *) {
    ESP_ERROR_CHECK(bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, false));
    cdc_acm_host_driver_config_t driver{};
    driver.driver_task_stack_size = 8192;
    driver.driver_task_priority = 12;
    driver.xCoreID = 0;
    driver.new_dev_cb = new_device;
    ESP_ERROR_CHECK(cdc_acm_host_install(&driver));
    cdc_acm_host_device_config_t config{};
    config.connection_timeout_ms = 1000;
    config.out_buffer_size = 512;
    config.in_buffer_size = 512;
    config.event_cb = usb_event;
    config.data_cb = receive;
    for (;;) {
        decoder.reset();
        have_sequence = false;
        while (xSemaphoreTake(disconnected, 0) == pdTRUE) {
        }
        cdc_acm_dev_hdl_t device = nullptr;
        if (cdc_acm_host_open(0x303a, 0x1001, 0, &config, &device) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        {
            Lock lock;
            auto &d = model->data;
            d.usb = true;
            d.firmware_ok = false;
            d.radio = false;
            d.settings = rf::Settings{};
            d.crc = 0;
            d.applied_count = 0;
            d.background_active = false;
            error_text("");
            notice("C5の測定設定を確認しています");
        }
        ESP_LOGI(TAG, "C5 connected; fixed gain=%d offset=%d bw=%d noden=%d", rf::gain, rf::offset,
                 rf::bandwidth, rf::capture_samples);
        esp_err_t err = command(
            device,
            "scan off\nscanmode fast\ngain 30\noffset 10\nbw 1\nnoden 256\nalpha 0.3\nscan on\nstatus\n");
        Work w;
        if (err == ESP_OK)
            err = restore(device, w, false);
        int64_t last_beat = 0, last_status = 0;
        while (err == ESP_OK && xSemaphoreTake(disconnected, 0) != pdTRUE) {
            Action a;
            while (xQueueReceive(actions, &a, 0) == pdTRUE && err == ESP_OK)
                err = action(device, w, a);
            if (err != ESP_OK)
                break;
            const int64_t now = esp_timer_get_time();
            bool ready = false, active = false, start_default_search = false;
            {
                Lock lock;
                model->expire_bin(now / rf::bin_us);
                ready = model->settings_and_profile() && model->data.profile_records >= 3 &&
                        now >= model->collect_after && now - model->data.last_record_us < rf::fresh_us;
                active = model->data.usb;
            }
            if (!active)
                break;
            if (w.stage == Stage::AwaitMonitor && ready) {
                Lock lock;
                auto &d = model->data;
                d.profile_ok = true;
                error_text("");
                if (w.calibrating) {
                    w.stage = Stage::Background;
                    d.mode = rf::Mode::BackgroundMonitor;
                    model->begin_background();
                    notice("背景記録中：全機OFFのままお待ちください");
                } else {
                    w.stage = Stage::Monitor;
                    d.mode = rf::Mode::Monitor;
                    start_default_search = w.search_after_restore;
                    w.search_after_restore = false;
                    if (w.cancelled_background) {
                        notice("背景記録を中断しました。保存前の背景は保持されています");
                        w.cancelled_background = false;
                    } else
                        notice(d.background_active ? "背景との差を表示しています"
                                                   : "背景未取得：受信値を表示しています");
                }
                if (w.normal_after_restore && d.normal_capture.phase == signal_health::Phase::Preparing) {
                    d.normal_capture.start_off(now);
                    w.normal_after_restore = 0;
                    notice("無送信を記録中：全VTXをOFFのままお待ちください");
                }
                d.mode_started_us = now;
                ESP_LOGI(TAG, "PROFILE_CONFIRMED mode=%s", rf::mode_name(d.mode));
            }
            signal_health::Reference normal_candidate;
            bool normal_ready = false;
            {
                Lock lock;
                auto &cap = model->data.normal_capture;
                if ((cap.phase == signal_health::Phase::Off && now - cap.started_us > 5000000) ||
                    (cap.phase == signal_health::Phase::Normal && now - cap.started_us > 12000000))
                    cap.fail(signal_health::Error::Timeout);
                if (cap.phase == signal_health::Phase::Failed && !w.normal_error_reported) {
                    notice("正常基準を保存しませんでした。正常基準ボタンで理由を確認してください");
                    ESP_LOGW(TAG, "NORMAL_FAILED error=%u", unsigned(cap.error));
                    w.normal_error_reported = true;
                }
                if (cap.phase == signal_health::Phase::Ready) {
                    normal_ready = rf::normal_context_matches(model->data, cap.slot, cap.candidate) &&
                                   signal_health::valid(cap.candidate);
                    if (normal_ready)
                        normal_candidate = cap.candidate;
                    else
                        cap.fail(signal_health::Error::Conditions);
                }
            }
            if (normal_ready) {
                const bool saved = store_normal(normal_candidate);
                Lock lock;
                auto &d = model->data;
                auto &cap = d.normal_capture;
                if (saved) {
                    d.normal_refs[rf::normal_index(normal_candidate.mhz)] = normal_candidate;
                    d.normal_active[cap.slot] = rf::normal_matches(d, cap.slot, normal_candidate);
                    d.channel[cap.slot].planned = true;
                    cap.phase = signal_health::Phase::Saved;
                    model->reset_normal();
                    notice("正常基準を保存しました。対象機ONのまま評価を10秒待ってください");
                    ESP_LOGI(TAG, "NORMAL_COMPLETE slot=%u mhz=%u median=%d off_p95=%d", cap.slot,
                             normal_candidate.mhz, normal_candidate.median, normal_candidate.off_p95);
                } else {
                    cap.fail(signal_health::Error::Save);
                    notice("正常基準の保存に失敗。以前の保存基準は保持しています");
                }
            }
            if (start_default_search) {
                err = sweep_start(device, w, false);
                continue;
            }
            if (w.stage == Stage::AwaitGroup && ready) {
                Lock lock;
                for (auto &b : model->sweep_bucket)
                    b = rf::Average{};
                model->collect_after = now;
                w.collect_us = now;
                w.stage = Stage::CollectGroup;
            }
            if ((w.stage == Stage::AwaitGroup || w.stage == Stage::AwaitMonitor) &&
                now - w.requested_us > 4000000)
                fail(w, "C5の周波数・固定設定を確認できません");
            if (w.stage == Stage::Background) {
                bool done = false, ok = false;
                {
                    Lock lock;
                    done = model->bg_count >= rf::BG_BINS;
                    ok = done && model->monitor_background(w.candidate);
                }
                if (done) {
                    if (!ok) {
                        fail(w, model->bg_unstable
                                    ? "背景の変動が大きいため保存しません。全機OFFで再記録してください"
                                    : "背景記録中に飽和・欠落・取得失敗がありました");
                    } else
                        err = sweep_start(device, w, true);
                }
            }
            if (w.stage == Stage::CollectGroup && now - w.collect_us >= (w.calibrating ? 250000 : 60000)) {
                bool clean;
                {
                    Lock lock;
                    clean = model->finish_group(w.group, now);
                }
                w.group_failed |= !clean;
                w.group += 8;
                if (w.group < rf::sweep_count(w.stride))
                    err = group_start(device, w);
                else {
                    bool good = true;
                    {
                        Lock lock;
                        model->finish_sweep(now, w.sweep_us);
                        good = model->data.sweep_invalid == 0;
                        if (w.calibrating && model->bg_failed)
                            good = false;
                        ESP_LOGI(TAG, "SWEEP_DONE points=%u step_mhz=%u valid=%d seconds=%.3f calibration=%d",
                                 rf::sweep_count(w.stride), 2 * w.stride, good, model->data.sweep_seconds,
                                 w.calibrating);
                    }
                    if (w.calibrating) {
                        if (!good || w.group_failed) {
                            fail(w, "広域背景に未取得点があります。前回の保存内容は保持します");
                        } else {
                            {
                                Lock lock;
                                std::copy_n(model->data.sweep, rf::SWEEP, w.candidate.sweep);
                            }
                            rf::seal(w.candidate);
                            const bool saved = store_background(w.candidate);
                            {
                                Lock lock;
                                auto &d = model->data;
                                d.background = w.candidate;
                                d.background_stored = true;
                                d.background_active = true;
                                d.background_saved = saved;
                                d.background_busy = false;
                                model->clear_comparison_views();
                                notice(saved ? "4chと広域の背景を保存しました"
                                             : "背景は本体保存に失敗。現在のRAM内だけで使用中");
                            }
                            ESP_LOGI(TAG, "BACKGROUND_COMPLETE saved=%d revision=%lu", saved,
                                     (unsigned long)w.candidate.revision);
                            err = restore(device, w, false);
                        }
                    } else
                        err = sweep_start(device, w, false);
                }
            }
            if (w.stage == Stage::Monitor && !ready) {
                Lock lock;
                model->data.profile_ok = false;
                for (auto &c : model->data.channel)
                    c.valid = false;
            }
            if (now - last_status >= 5000000) {
                err = command(device, "status\n");
                last_status = now;
            }
            if (now - last_beat >= 1000000) {
                heartbeat();
                last_beat = now;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        {
            Lock lock;
            auto &d = model->data;
            d.usb = false;
            d.profile_ok = false;
            d.background_busy = false;
            d.mode = rf::Mode::Offline;
            d.background_active = false;
            if (d.normal_capture.busy())
                d.normal_capture.fail(signal_health::Error::Disconnected);
            std::fill_n(d.normal_active, rf::N, false);
            model->reset_normal();
            for (auto &c : d.channel)
                c.valid = false;
            if (err != ESP_OK)
                error_text(esp_err_to_name(err));
        }
        cdc_acm_host_close(device);
        ESP_LOGW(TAG, "C5 closed: %s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(300));
    }
}
static void dump(const char *what) {
    auto *d = new rf::Data;
    app_copy_data(d);
    if (!std::strcmp(what, "sweep")) {
        printf("RF_SWEEP_BEGIN step_mhz=%u points=%u\nindex,mhz,level_cdb,peak_cdb,background_cdb,host_us\n",
               2 * d->sweep_stride, rf::sweep_count(d->sweep_stride));
        for (unsigned i = 0; i < rf::SWEEP; i += d->sweep_stride)
            printf("%u,%u,%d,%d,%d,%lld\n", i, rf::sweep_frequency(i), d->sweep[i], d->sweep_peak[i],
                   d->background_stored ? d->background.sweep[i] : rf::missing, (long long)d->sweep_time[i]);
        printf("RF_SWEEP_END\n");
    } else if (!std::strcmp(what, "history")) {
        printf("RF_HISTORY_BEGIN\nhost_bin,%s_mean,%s_mean,%s_mean,%s_mean,valid,bad\n",
               rf::presets[d->plan.ids[0]].name, rf::presets[d->plan.ids[1]].name,
               rf::presets[d->plan.ids[2]].name, rf::presets[d->plan.ids[3]].name);
        for (unsigned i = 0; i < rf::HISTORY; i++) {
            const auto &b = d->history[i];
            if (b.index >= 0)
                printf("%lld,%d,%d,%d,%d,%u,%u\n", (long long)b.index, b.mean[0], b.mean[1], b.mean[2],
                       b.mean[3], b.valid_mask, b.bad_mask);
        }
        printf("RF_HISTORY_END\n");
    } else if (!std::strcmp(what, "normal")) {
        const auto &cap = d->normal_capture;
        printf("RF_NORMAL_CAPTURE phase=%s slot=%u count=%u error=%u mhz=%u candidate_med=%d off_p95=%d "
               "p10=%d p90=%d min=%d\n",
               signal_health::phase_name(cap.phase), cap.slot, cap.count, unsigned(cap.error),
               cap.candidate.mhz, cap.candidate.median, cap.candidate.off_p95, cap.candidate.p10,
               cap.candidate.p90, cap.candidate.minimum);
        for (unsigned i = 0; i < rf::N; i++) {
            const auto &ref = rf::normal_reference(*d, i);
            const auto r = rf::visible_normal(*d, i, esp_timer_get_time());
            printf("RF_NORMAL slot=%u mhz=%u stored=%d active=%d level=%s stability=%s level_reason=%u "
                   "stability_reason=%u drop=%ld rise=%ld spread=%ld dips=%u longest=%u count=%u "
                   "ref_median=%d off_p95=%d checksum=%08lx\n",
                   i, rf::presets[d->plan.ids[i]].mhz, signal_health::valid(ref), d->normal_active[i],
                   signal_health::grade_name(r.level), signal_health::grade_name(r.stability),
                   unsigned(r.level_reason), unsigned(r.stability_reason), (long)r.drop, (long)r.rise,
                   (long)r.spread, r.dips, r.longest_dip, r.count, ref.median, ref.off_p95,
                   (unsigned long)ref.checksum);
        }
    } else if (!std::strcmp(what, "channels")) {
        uint16_t f[rf::N];
        rf::plan_frequencies(d->plan, f);
        printf("RF_CHANNELS names=%s,%s,%s,%s mhz=%u,%u,%u,%u valid=%d\n", rf::presets[d->plan.ids[0]].name,
               rf::presets[d->plan.ids[1]].name, rf::presets[d->plan.ids[2]].name,
               rf::presets[d->plan.ids[3]].name, f[0], f[1], f[2], f[3], rf::plan_valid(d->plan));
    } else if (!std::strcmp(what, "background")) {
        printf("RF_BACKGROUND active=%d stored=%d saved=%d checksum=%08lx valid=%d serial=%s revision=%lu "
               "monitor=[%d,%d,%d,%d] mhz=[%u,%u,%u,%u] version=%lu\n",
               d->background_active, d->background_stored, d->background_saved,
               (unsigned long)d->background.checksum, rf::background_valid(d->background),
               d->background.serial, (unsigned long)d->background.revision, d->background.monitor[0],
               d->background.monitor[1], d->background.monitor[2], d->background.monitor[3],
               d->background.monitor_mhz[0], d->background.monitor_mhz[1], d->background.monitor_mhz[2],
               d->background.monitor_mhz[3], (unsigned long)d->background.version);
    } else
        heartbeat();
    delete d;
}
static void console_task(void *) {
    setvbuf(stdin, nullptr, _IONBF, 0);
    char text[96];
    unsigned used = 0;
    bool overflow = false;
    for (;;) {
        const int c = std::getchar();
        if (c == EOF) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (c == '\r')
            continue;
        if (c != '\n') {
            if (used + 1 < sizeof(text))
                text[used++] = char(c);
            else
                overflow = true;
            continue;
        }
        text[used] = 0;
        used = 0;
        if (overflow) {
            overflow = false;
            printf("RF_CMD error=too-long\n");
            continue;
        }
        bool known = true;
        unsigned channel, value;
        char preset_name[8], extra;
        if (!std::strcmp(text, "rf monitor"))
            app_post(ActionType::Monitor);
        else if (!std::strcmp(text, "rf search"))
            app_post(ActionType::Search);
        else if (!std::strcmp(text, "rf search 4"))
            app_post(ActionType::SearchStep, 4);
        else if (!std::strcmp(text, "rf search 2"))
            app_post(ActionType::SearchStep, 2);
        else if (std::sscanf(text, "rf normal off %u %7s %c", &channel, preset_name, &extra) == 2 &&
                 channel < rf::N && !std::strcmp(preset_name, "YES"))
            app_post(ActionType::NormalOff, channel);
        else if (std::sscanf(text, "rf normal on %u %7s %c", &channel, preset_name, &extra) == 2 &&
                 channel < rf::N && !std::strcmp(preset_name, "YES"))
            app_post(ActionType::NormalOn, channel);
        else if (std::sscanf(text, "rf normal use %u %7s %c", &channel, preset_name, &extra) == 2 &&
                 channel < rf::N && !std::strcmp(preset_name, "YES"))
            app_post(ActionType::AdoptNormal, channel);
        else if (!std::strcmp(text, "rf normal cancel"))
            app_post(ActionType::CancelNormal);
        else if (!std::strcmp(text, "rf channels analog"))
            app_post(ActionType::ChannelSet, 0);
        else if (!std::strcmp(text, "rf channels race"))
            app_post(ActionType::ChannelSet, 1);
        else if (std::sscanf(text, "rf channel %u %7s %c", &channel, preset_name, &extra) == 2 &&
                 channel < rf::N && rf::preset_id(preset_name) >= 0 &&
                 rf::preset_enabled(unsigned(rf::preset_id(preset_name))))
            app_post(ActionType::SetChannel, channel * rf::PRESETS + unsigned(rf::preset_id(preset_name)));
        else if (!std::strcmp(text, "rf channels"))
            dump("channels");
        else if (!std::strcmp(text, "rf background YES"))
            app_post(ActionType::Background);
        else if (!std::strcmp(text, "rf bg-use YES"))
            app_post(ActionType::AdoptBackground);
        else if (!std::strcmp(text, "rf bg-clear YES"))
            app_post(ActionType::ClearBackground);
        else if (!std::strcmp(text, "rf peak-reset"))
            app_post(ActionType::ResetPeaks);
        else if (std::sscanf(text, "rf plan %u %u", &channel, &value) == 2 && channel < 4 && value < 2)
            app_post(ActionType::Plan, channel * 2 + value);
        else if (!std::strncmp(text, "rf dump ", 8))
            dump(text + 8);
        else if (!std::strcmp(text, "rf status"))
            dump("status");
        else if (!std::strcmp(text, "rf ui"))
            app_ui_diagnostics();
        else
            known = false;
        printf("RF_CMD %s command=%s\n", known ? "accepted" : "unknown", text);
    }
}
static void self_tests_task(void *waiting) {
    c5_decoder_self_test();
    rf_self_test();
    ESP_LOGI(TAG,
             "SELF_TESTS_PASSED decoder profiles linear-power background gaps flags CRC storage IMD normal "
             "level stability stack_hwm_raw=%u",
             unsigned(uxTaskGetStackHighWaterMark(nullptr)));
    xTaskNotifyGive(static_cast<TaskHandle_t>(waiting));
    vTaskDelete(nullptr);
}
extern "C" void app_main() {
    // Reference/monitor regression cases exceed the small IDF main-task stack.
    // Keep assertions enabled and run the complete suite before device/UI startup.
    const BaseType_t started =
        xTaskCreate(self_tests_task, "rf_self_tests", 16384, xTaskGetCurrentTaskHandle(), 5, nullptr);
    assert(started == pdPASS);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    model = new rf::Model;
    assert(model);
    mutex = xSemaphoreCreateMutex();
    disconnected = xSemaphoreCreateBinary();
    actions = xQueueCreate(16, sizeof(Action));
    assert(mutex && disconnected && actions);
    load_background();
    load_channels();
    load_normal();
    ESP_ERROR_CHECK(bsp_i2c_init());
    bsp_io_expander_pi4ioe_init(bsp_i2c_get_handle());
    bsp_set_usb_5v_en(false);
    vTaskDelay(pdMS_TO_TICKS(1000));
    lv_display_t *display = bsp_display_start();
    assert(display);
    bsp_display_lock(0);
    void *pool = heap_caps_malloc(256 * 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(pool);
    for (unsigned i = 0; i < 4; ++i) {
        assert(lv_mem_add_pool(static_cast<uint8_t *>(pool) + i * 64 * 1024, 64 * 1024));
    }
    app_build_ui(display);
    bsp_display_unlock();
    ESP_ERROR_CHECK(bsp_display_brightness_set(10));
    vTaskDelay(pdMS_TO_TICKS(250));
    bsp_set_usb_5v_en(true);
    xTaskCreatePinnedToCore(receiver_task, "rf_receiver", 10240, nullptr, 8, nullptr, 0);
    xTaskCreatePinnedToCore(console_task, "rf_console", 8192, nullptr, 2, nullptr, 1);
}
