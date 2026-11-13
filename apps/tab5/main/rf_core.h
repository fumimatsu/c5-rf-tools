#pragma once
#include "c5_decoder.h"
#include "signal_health.h"
#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace rf {
constexpr unsigned N = 4, SWEEP = 137, HISTORY = 600, WATER_ROWS = 24, BG_BINS = 50;
constexpr uint16_t frequencies[N] = {5685, 5705, 5780, 5820};
struct Preset {
    const char *name;
    uint16_t mhz;
};
constexpr unsigned PRESETS = 40;
constexpr Preset presets[PRESETS] = {
    {"R1", 5658}, {"R2", 5695}, {"R3", 5732}, {"R4", 5769}, {"R5", 5806}, {"R6", 5843}, {"R7", 5880},
    {"R8", 5917}, {"A1", 5865}, {"A2", 5845}, {"A3", 5825}, {"A4", 5805}, {"A5", 5785}, {"A6", 5765},
    {"A7", 5745}, {"A8", 5725}, {"B1", 5733}, {"B2", 5752}, {"B3", 5771}, {"B4", 5790}, {"B5", 5809},
    {"B6", 5828}, {"B7", 5847}, {"B8", 5866}, {"E1", 5705}, {"E2", 5685}, {"E3", 5665}, {"E4", 5645},
    {"E5", 5885}, {"E6", 5905}, {"E7", 5925}, {"E8", 5945}, {"F1", 5740}, {"F2", 5760}, {"F3", 5780},
    {"F4", 5800}, {"F5", 5820}, {"F6", 5840}, {"F7", 5860}, {"F8", 5880},
};
inline bool preset_enabled(unsigned id) {
    return id < PRESETS && presets[id].mhz >= 5645 && presets[id].mhz <= 5917;
}
inline int preset_id(const char *name) {
    for (unsigned i = 0; i < PRESETS; i++)
        if (!std::strcmp(presets[i].name, name))
            return int(i);
    return -1;
}
struct ChannelPlan {
    uint32_t magic = 0, version = 0;
    uint8_t ids[N] = {25, 24, 34, 36};
    uint32_t checksum = 0;
};
inline void plan_frequencies(const ChannelPlan &p, uint16_t *out) {
    for (unsigned i = 0; i < N; i++)
        out[i] = presets[p.ids[i]].mhz;
}
constexpr int16_t missing = INT16_MIN;
constexpr int gain = 30, offset = 10, bandwidth = 1, capture_samples = 256;
constexpr int64_t bin_us = 100000, fresh_us = 500000;
enum Transport : uint8_t { CrcError = 1, SequenceGap = 2, ChipDrop = 4, Malformed = 8,
                           StreamReset = 16, ClockReset = 32, Duplicate = 64 };
enum SampleIssue : uint8_t { CaptureFailed = 1, GainMoved = 2, Clipped = 4, Fallback = 8, Missing = 16 };
inline uint8_t sample_issues(const c5::Reading &r) {
    return uint8_t((r.flags & 15) | (r.gain != gain ? GainMoved : 0) |
                   (r.clips ? Clipped : 0) |
                   (r.raw == missing || r.filtered == missing ? Missing : 0));
}
enum class Mode : uint8_t { Offline, Restore, Monitor, Search, BackgroundMonitor, BackgroundSweep, Fault };
inline const char *mode_name(Mode m) {
    switch (m) {
    case Mode::Offline:
        return "offline";
    case Mode::Restore:
        return "restore";
    case Mode::Monitor:
        return "monitor";
    case Mode::Search:
        return "search";
    case Mode::BackgroundMonitor:
        return "background-monitor";
    case Mode::BackgroundSweep:
        return "background-sweep";
    case Mode::Fault:
        return "fault";
    }
    return "invalid";
}
// Storage always uses the 2 MHz grid. Fast scans visit even indices only.
inline unsigned sweep_count(unsigned stride) {
    return (SWEEP - 1) / stride + 1;
}
inline unsigned sweep_index(unsigned point, unsigned stride) {
    return std::min<unsigned>(point * stride, SWEEP - 1);
}
inline unsigned nearest_sweep_index(unsigned index, unsigned stride) {
    return sweep_index((index + stride / 2) / stride, stride);
}
inline uint16_t sweep_frequency(unsigned i) {
    return uint16_t(5645 + 2 * i);
}
inline float power(int16_t db) {
    return std::pow(10.f, db / 1000.f);
}
inline int16_t cdb(float p) {
    return p > 0 ? int16_t(std::clamp<int>(int(std::lround(1000.f * std::log10(p))), -32767, 32767))
                 : missing;
}
inline bool valid(const c5::Reading &r) {
    return !(r.flags & 15) && r.gain == gain && !r.clips && r.raw != missing && r.filtered != missing;
}
inline bool matches(const c5::Record &r, const uint16_t *f, unsigned n) {
    if (r.count != n)
        return false;
    for (unsigned i = 0; i < n; i++)
        if (r.channels[i].mhz != f[i])
            return false;
    return true;
}
struct Average {
    float sum = 0;
    uint32_t count = 0, bad = 0;
    int16_t peak = missing;
    void add(const c5::Reading &r) {
        if (!valid(r)) {
            ++bad;
            return;
        }
        sum += power(r.raw);
        ++count;
        if (peak == missing || r.raw > peak)
            peak = r.raw;
    }
    int16_t mean() const {
        return count ? cdb(sum / count) : missing;
    }
    bool clean() const {
        return count >= 3 && bad == 0;
    }
};
struct Settings {
    int gain = -1, offset = 1000, bw = -1, noden = -1;
    bool fast = false, scan = false;
    bool valid() const {
        return gain == rf::gain && offset == rf::offset && bw == bandwidth && noden == capture_samples &&
               fast && scan;
    }
};
struct BackgroundV1 {
    uint32_t magic, version, size, revision;
    int32_t gain, offset, bw, samples;
    char serial[40], firmware[32];
    int16_t monitor[N], upper[N], sweep[SWEEP];
    uint32_t checksum;
};
inline uint32_t hash_bytes(const void *data, size_t n) {
    const auto *p = static_cast<const uint8_t *>(data);
    uint32_t h = 2166136261u;
    while (n--)
        h = (h ^ *p++) * 16777619u;
    return h;
}
inline void seal_plan(ChannelPlan &p) {
    p.magic = 0x43483450;
    p.version = 1;
    p.checksum = hash_bytes(&p, offsetof(ChannelPlan, checksum));
}
inline bool plan_valid(const ChannelPlan &p) {
    if (p.magic != 0x43483450 || p.version != 1 ||
        p.checksum != hash_bytes(&p, offsetof(ChannelPlan, checksum)))
        return false;
    for (auto id : p.ids)
        if (!preset_enabled(id))
            return false;
    return true;
}
inline ChannelPlan channel_set(unsigned set) {
    ChannelPlan p;
    if (set == 1)
        for (unsigned i = 0; i < N; i++)
            p.ids[i] = uint8_t(i + 1);
    seal_plan(p);
    return p;
}
struct Background {
    uint32_t magic, version, size, revision;
    int32_t gain, offset, bw, samples;
    char serial[40], firmware[32];
    int16_t monitor[N], upper[N], sweep[SWEEP];
    uint16_t monitor_mhz[N];
    uint32_t checksum;
};
inline void seal(Background &b) {
    b.magic = 0x52463442;
    b.version = 2;
    b.size = sizeof(b);
    b.checksum = hash_bytes(&b, offsetof(Background, checksum));
}
template <class B> inline bool background_payload_valid(const B &b, unsigned version) {
    if (b.magic != 0x52463442 || b.version != version || b.size != sizeof(b) ||
        b.checksum != hash_bytes(&b, offsetof(B, checksum)))
        return false;
    if (!std::memchr(b.serial, 0, sizeof(b.serial)) || !std::memchr(b.firmware, 0, sizeof(b.firmware)))
        return false;
    if (!b.serial[0] || !b.firmware[0] || b.gain != gain || b.offset != offset || b.bw != bandwidth ||
        b.samples != capture_samples)
        return false;
    for (auto x : b.monitor)
        if (x == missing)
            return false;
    for (auto x : b.upper)
        if (x == missing)
            return false;
    for (auto x : b.sweep)
        if (x == missing)
            return false;
    return true;
}
inline bool background_valid(const Background &b) {
    if (!background_payload_valid(b, 2))
        return false;
    for (auto f : b.monitor_mhz)
        if (f < 5645 || f > 5917)
            return false;
    return true;
}
inline bool upgrade_background(const BackgroundV1 &old, Background &b) {
    if (!background_payload_valid(old, 1))
        return false;
    b = {};
    b.revision = old.revision;
    b.gain = old.gain;
    b.offset = old.offset;
    b.bw = old.bw;
    b.samples = old.samples;
    std::memcpy(b.serial, old.serial, sizeof(b.serial));
    std::memcpy(b.firmware, old.firmware, sizeof(b.firmware));
    std::copy_n(old.monitor, N, b.monitor);
    std::copy_n(old.upper, N, b.upper);
    std::copy_n(old.sweep, SWEEP, b.sweep);
    std::copy_n(frequencies, N, b.monitor_mhz);
    seal(b);
    return background_valid(b);
}
inline bool compatible(const Background &b, const char *serial, const char *firmware, const Settings &s,
                       const uint16_t *selected = frequencies) {
    return background_valid(b) && s.valid() && serial[0] && firmware[0] && !std::strcmp(b.serial, serial) &&
           !std::strcmp(b.firmware, firmware) && std::equal(selected, selected + N, b.monitor_mhz);
}
struct Channel {
    int16_t raw = missing, filtered = missing, peak = missing;
    uint8_t flags = 0, clips = 0, gain = 0;
    uint8_t last_issues = 0, last_bad_flags = 0, last_bad_clips = 0, last_bad_gain = 0;
    bool valid = false, planned = true, alert = false;
    uint8_t votes = 0;
    uint32_t source_us = 0;
    int64_t host_us = 0, bad_us = 0;
};
struct HistoryBin {
    int64_t index = -1;
    int16_t mean[N] = {missing, missing, missing, missing};
    int16_t peak[N] = {missing, missing, missing, missing};
    uint8_t valid_mask = 0, bad_mask = 0;
};
struct SweepRow {
    int64_t completed_us = 0;
    int16_t level[SWEEP];
};
struct Data {
    Mode mode = Mode::Offline;
    bool usb = false, firmware_ok = false, radio = false, profile_ok = false;
    Settings settings;
    char serial[40]{}, firmware[32]{}, notice[192]{}, error[120]{};
    uint32_t bytes = 0, records = 0, matched = 0, crc = 0, gaps = 0, drops = 0, malformed = 0, resync = 0,
             bad_samples = 0, clock_resets = 0;
    uint32_t drop_increase = 0, duplicates = 0, transport_faults = 0;
    uint32_t clip_samples = 0, gain_samples = 0, fail_samples = 0, fallback_samples = 0, missing_samples = 0;
    uint8_t transport_flags = 0;
    int64_t transport_us = 0;
    int64_t last_record_us = 0, mode_started_us = 0;
    signal_health::Reference normal_refs[PRESETS]{};
    bool normal_active[N]{};
    signal_health::Result normal_result[N];
    signal_health::Capture normal_capture;
    ChannelPlan plan;
    Channel channel[N];
    HistoryBin history[HISTORY];
    int64_t last_bin = -1;
    int16_t sweep[SWEEP], sweep_peak[SWEEP];
    int64_t sweep_time[SWEEP];
    SweepRow rows[WATER_ROWS];
    unsigned row_count = 0, row_head = 0, sweep_done = 0, sweeps = 0, sweep_invalid = 0;
    unsigned search_stride = 2, sweep_stride = 2; // 2 = 4 MHz fast, 1 = 2 MHz detail
    uint16_t expected[8]{}, applied[8]{};
    unsigned expected_count = 0, applied_count = 0;
    uint32_t profile_records = 0, settings_revision = 0;
    int64_t sweep_completed_us = 0;
    float sweep_seconds = 0;
    bool background_stored = false, background_active = false, background_saved = false;
    bool background_persistence_ok = false, background_busy = false;
    unsigned background_progress = 0;
    Background background{};
    Data() {
        seal_plan(plan);
        std::fill_n(sweep, SWEEP, missing);
        std::fill_n(sweep_peak, SWEEP, missing);
        std::fill_n(sweep_time, SWEEP, 0);
        for (auto &r : rows)
            std::fill_n(r.level, SWEEP, missing);
    }
};
inline unsigned normal_index(uint16_t mhz) {
    for (unsigned i = 0; i < PRESETS; i++)
        if (preset_enabled(i) && presets[i].mhz == mhz)
            return i;
    return PRESETS;
}
inline const signal_health::Reference &normal_reference(const Data &d, unsigned slot) {
    return d.normal_refs[normal_index(presets[d.plan.ids[slot]].mhz)];
}
inline bool normal_context_matches(const Data &d, unsigned slot, const signal_health::Reference &ref) {
    return d.usb && d.firmware_ok && d.radio && d.settings.valid() &&
           ref.mhz == presets[d.plan.ids[slot]].mhz && !std::strcmp(ref.serial, d.serial) &&
           !std::strcmp(ref.firmware, d.firmware);
}
inline bool normal_matches(const Data &d, unsigned slot, const signal_health::Reference &ref) {
    return d.usb && d.firmware_ok && d.radio && d.settings.valid() &&
           signal_health::compatible(ref, presets[d.plan.ids[slot]].mhz, d.serial, d.firmware);
}
inline bool duplicate_channel(const Data &d, unsigned slot) {
    for (unsigned i = 0; i < N; i++)
        if (i != slot && presets[d.plan.ids[i]].mhz == presets[d.plan.ids[slot]].mhz)
            return true;
    return false;
}
inline signal_health::Result visible_normal(const Data &d, unsigned slot, int64_t now) {
    using signal_health::Reason;
    using signal_health::unknown;
    if (d.normal_capture.busy() || d.background_busy)
        return unknown(Reason::Recording);
    if (d.mode != Mode::Monitor || !d.profile_ok || !d.usb || now - d.last_record_us >= fresh_us)
        return unknown(Reason::Paused);
    const auto &c = d.channel[slot];
    if (!c.planned)
        return unknown(Reason::PlannedOff);
    if (duplicate_channel(d, slot))
        return unknown(Reason::Duplicate);
    if (d.transport_us && now - d.transport_us < 1000000)
        return unknown(Reason::Transport);
    if (!c.valid || (c.bad_us && now - c.bad_us < 1000000))
        return unknown(Reason::Invalid);
    const auto &ref = normal_reference(d, slot);
    if (!signal_health::valid(ref))
        return unknown(Reason::NoReference);
    if (!normal_matches(d, slot, ref))
        return unknown(Reason::Conditions);
    if (!d.normal_active[slot])
        return unknown(Reason::Inactive);
    return d.normal_result[slot];
}
struct Model {
    Data data;
    signal_health::Monitor normal_monitor[N];
    Average bucket[N];
    int64_t bucket_index = -1;
    int16_t bg_bins[N][BG_BINS];
    unsigned bg_count = 0;
    bool bg_failed = false, bg_skip_first = false, bg_unstable = false;
    Average sweep_bucket[8];
    int64_t collect_after = 0;
    Model() {
        std::memset(bg_bins, 0, sizeof(bg_bins));
    }
    void reset_normal() {
        for (unsigned i = 0; i < N; i++) {
            normal_monitor[i].reset();
            data.normal_result[i] = signal_health::unknown(signal_health::Reason::Warming);
        }
    }
    void transport_fault(uint8_t reason, int64_t now) {
        // Mark the affected host bin before later valid frames can complete it.
        // Saved references/backgrounds remain unchanged; only this capture/window is invalid.
        if (data.mode == Mode::Monitor || data.mode == Mode::BackgroundMonitor) {
            expire_bin(now / bin_us);
            for (auto &b : bucket) ++b.bad;
        }
        if (data.mode == Mode::Search || data.mode == Mode::BackgroundSweep)
            for (auto &b : sweep_bucket) ++b.bad;
        if (!data.transport_us || now - data.transport_us >= 1000000) data.transport_flags = 0;
        data.transport_flags |= reason;
        data.transport_us = now;
        ++data.transport_faults;
        bg_failed = true;
        if (data.normal_capture.phase == signal_health::Phase::Off ||
            data.normal_capture.phase == signal_health::Phase::Normal)
            data.normal_capture.fail(signal_health::Error::Transport);
        reset_normal();
    }
    void count_sample(const c5::Reading &r) {
        const auto issues = sample_issues(r);
        data.clip_samples += bool(issues & Clipped);
        data.gain_samples += bool(issues & GainMoved);
        data.fail_samples += bool(issues & CaptureFailed);
        data.fallback_samples += bool(issues & Fallback);
        data.missing_samples += bool(issues & Missing);
    }
    void finish_normal_bin(const HistoryBin &bin, bool consecutive) {
        auto &cap = data.normal_capture;
        if (data.mode == Mode::Monitor &&
            (cap.phase == signal_health::Phase::Off || cap.phase == signal_health::Phase::Normal)) {
            const unsigned i = cap.slot;
            if (!consecutive)
                cap.fail(signal_health::Error::InvalidData);
            else
                cap.push(bin.mean[i], (bin.valid_mask & (1u << i)) && !(bin.bad_mask & (1u << i)));
        }
        for (unsigned i = 0; i < N; i++) {
            const auto &ref = normal_reference(data, i);
            if (data.mode == Mode::Monitor && !cap.busy() && !data.background_busy &&
                data.channel[i].planned && !duplicate_channel(data, i) && data.normal_active[i] &&
                normal_matches(data, i, ref)) {
                if (!consecutive)
                    normal_monitor[i].reset();
                data.normal_result[i] = normal_monitor[i].push(
                    bin.index, bin.mean[i], (bin.valid_mask & (1u << i)) && !(bin.bad_mask & (1u << i)), ref);
            } else {
                normal_monitor[i].reset();
                data.normal_result[i] = signal_health::unknown(signal_health::Reason::Warming);
            }
        }
    }
    void clear_history() {
        for (auto &x : data.history)
            x = HistoryBin{};
        for (auto &x : bucket)
            x = Average{};
        bucket_index = -1;
        data.last_bin = -1;
        for (auto &c : data.channel) {
            c.votes = 0;
            c.alert = false;
        }
    }
    void clear_comparison_views() {
        clear_history();
        data.row_count = 0;
        data.row_head = 0;
        for (auto &row : data.rows) {
            row.completed_us = 0;
            std::fill_n(row.level, SWEEP, missing);
        }
        std::fill_n(data.sweep_peak, SWEEP, missing);
    }
    bool select_channels(const ChannelPlan &plan) {
        if (!plan_valid(plan))
            return false;
        for (unsigned i = 0; i < N; i++)
            if (presets[data.plan.ids[i]].mhz != presets[plan.ids[i]].mhz)
                data.normal_active[i] = false;
        data.plan = plan;
        reset_normal();
        data.background_active = false;
        data.profile_ok = false;
        clear_comparison_views();
        std::fill_n(data.sweep, SWEEP, missing);
        std::fill_n(data.sweep_time, SWEEP, 0);
        data.sweep_done = data.sweep_invalid = 0;
        data.sweep_completed_us = 0;
        data.sweep_seconds = 0;
        for (auto &c : data.channel) {
            const bool planned = c.planned;
            c = Channel{};
            c.planned = planned;
        }
        return true;
    }
    void invalidate_background(const char *why) {
        if (data.background_active) {
            data.background_active = false;
            std::snprintf(data.notice, sizeof(data.notice), "%s", why);
        }
    }
    void expire_bin(int64_t next) {
        if (bucket_index < 0) {
            bucket_index = next;
            return;
        }
        if (next <= bucket_index)
            return;
        HistoryBin b;
        b.index = bucket_index;
        for (unsigned i = 0; i < N; i++) {
            b.mean[i] = bucket[i].mean();
            b.peak[i] = bucket[i].peak;
            if (bucket[i].clean())
                b.valid_mask |= 1u << i;
            if (bucket[i].bad)
                b.bad_mask |= 1u << i;
        }
        finish_normal_bin(b, next == bucket_index + 1);
        data.history[unsigned(bucket_index % HISTORY)] = b;
        data.last_bin = bucket_index;
        if (data.mode == Mode::BackgroundMonitor && bg_skip_first) {
            bg_skip_first = false;
        } else if (data.mode == Mode::BackgroundMonitor) {
            if (b.valid_mask != 15 || b.bad_mask)
                bg_failed = true;
            if (bg_count < BG_BINS) {
                for (unsigned i = 0; i < N; i++)
                    bg_bins[i][bg_count] = b.mean[i];
                ++bg_count;
                data.background_progress = bg_count;
            }
        }
        if (data.mode == Mode::Monitor && data.background_active) {
            for (unsigned i = 0; i < N; i++) {
                auto &c = data.channel[i];
                const int threshold =
                    std::max<int>(data.background.monitor[i] + 600, data.background.upper[i] + 300);
                const bool high = (b.valid_mask & (1u << i)) && b.mean[i] >= threshold;
                c.votes = uint8_t(((c.votes << 1) | (high ? 1 : 0)) & 31);
                const unsigned votes = unsigned(c.votes & 1) + unsigned((c.votes >> 1) & 1) +
                                       unsigned((c.votes >> 2) & 1) + unsigned((c.votes >> 3) & 1) +
                                       unsigned((c.votes >> 4) & 1);
                if (!c.planned && votes >= 3)
                    c.alert = true;
                if (c.planned || !(b.valid_mask & (1u << i)) || b.mean[i] < threshold - 200)
                    c.alert = false;
            }
        }
        // Real time is retained: missing bins stay absent, including during search.
        if (next - bucket_index > 1) {
            for (auto &c : data.channel) {
                c.alert = false;
                c.votes = 0;
            }
            if (data.mode == Mode::BackgroundMonitor)
                bg_failed = true;
        }
        for (auto &x : bucket)
            x = Average{};
        bucket_index = next;
    }
    void set_profile(const uint16_t *list, unsigned count, int64_t now, int64_t settle_us = 120000) {
        reset_normal();
        std::copy_n(list, count, data.expected);
        data.expected_count = count;
        data.profile_ok = false;
        data.profile_records = 0;
        collect_after = now + settle_us;
        for (auto &x : sweep_bucket)
            x = Average{};
        for (auto &c : data.channel) {
            c.valid = false;
            c.alert = false;
            c.votes = 0;
        }
    }
    bool settings_and_profile() const {
        if (!data.usb || !data.firmware_ok || !data.radio || !data.settings.valid() ||
            data.applied_count != data.expected_count)
            return false;
        for (unsigned i = 0; i < data.expected_count; i++)
            if (data.applied[i] != data.expected[i])
                return false;
        return true;
    }
    void ingest(const c5::Record &r, int64_t now) {
        ++data.records;
        data.drops = r.dropped;
        if (!matches(r, data.expected, data.expected_count))
            return;
        ++data.matched;
        ++data.profile_records;
        data.last_record_us = now;
        data.profile_ok = settings_and_profile();
        if (!data.profile_ok || now < collect_after)
            return;
        if (data.mode == Mode::Search || data.mode == Mode::BackgroundSweep) {
            for (unsigned i = 0; i < r.count; i++) {
                count_sample(r.channels[i]);
                sweep_bucket[i].add(r.channels[i]);
                if (!valid(r.channels[i]))
                    ++data.bad_samples;
            }
            return;
        }
        if (data.mode != Mode::Monitor && data.mode != Mode::BackgroundMonitor)
            return;
        expire_bin(now / bin_us);
        for (unsigned i = 0; i < N; i++) {
            const auto &x = r.channels[i];
            auto &c = data.channel[i];
            if (c.host_us) {
                const uint32_t advance = x.source_us - c.source_us;
                if (advance > 0x80000000u) {
                    ++data.clock_resets;
                    invalidate_background("受信機の時刻が変化。背景を再確認してください");
                    transport_fault(ClockReset, now);
                }
            }
            c.source_us = x.source_us;
            c.host_us = now;
            c.raw = x.raw;
            c.filtered = x.filtered;
            c.flags = x.flags;
            c.clips = x.clips;
            c.gain = x.gain;
            c.valid = valid(x);
            count_sample(x);
            if (!c.valid) {
                c.bad_us = now;
                c.last_issues = sample_issues(x);
                c.last_bad_flags = x.flags;
                c.last_bad_clips = x.clips;
                c.last_bad_gain = x.gain;
                normal_monitor[i].reset();
                data.normal_result[i] = signal_health::unknown(signal_health::Reason::Invalid);
                ++data.bad_samples;
            } else if (c.peak == missing || x.raw > c.peak)
                c.peak = x.raw;
            bucket[i].add(x);
        }
    }
    void begin_background() {
        reset_normal();
        bg_count = 0;
        bg_failed = false;
        bg_unstable = false;
        bg_skip_first = true;
        data.background_busy = true;
        data.background_progress = 0;
        // Start on the next complete 100 ms bin, not a partial leading bin.
        for (auto &b : bucket)
            b = Average{};
        bucket_index = -1;
    }
    bool monitor_background(Background &result) {
        if (bg_failed || bg_count < BG_BINS)
            return false;
        for (unsigned i = 0; i < N; i++) {
            float total = 0;
            for (auto x : bg_bins[i]) {
                if (x == missing)
                    return false;
                total += power(x);
            }
            result.monitor[i] = cdb(total / BG_BINS);
            auto v = std::array<int16_t, BG_BINS>{};
            std::copy_n(bg_bins[i], BG_BINS, v.begin());
            std::sort(v.begin(), v.end());
            if (v.back() - v[25] > 600 || v[25] - v.front() > 600) {
                bg_unstable = true;
                return false;
            }
            result.upper[i] = v[47];
        }
        return true;
    }
    void set_sweep_stride(unsigned stride) {
        if ((stride != 1 && stride != 2) || data.sweep_stride == stride)
            return;
        data.sweep_stride = stride;
        data.row_count = data.row_head = data.sweep_done = data.sweep_invalid = 0;
        data.sweep_completed_us = 0;
        data.sweep_seconds = 0;
        std::fill_n(data.sweep, SWEEP, missing);
        std::fill_n(data.sweep_peak, SWEEP, missing);
        std::fill_n(data.sweep_time, SWEEP, 0);
        for (auto &row : data.rows) {
            row.completed_us = 0;
            std::fill_n(row.level, SWEEP, missing);
        }
    }
    void begin_sweep() {
        data.sweep_done = 0;
    }
    bool finish_group(unsigned start, int64_t now) {
        bool clean = true;
        for (unsigned i = 0; i < 8; i++) {
            const unsigned index = sweep_index(start + i, data.sweep_stride);
            if (i && index == sweep_index(start + i - 1, data.sweep_stride))
                continue;
            // Duplicate padding is deliberate; only the first slot contributes to its point.
            const auto &b = sweep_bucket[i];
            const int16_t level = b.clean() ? b.mean() : missing;
            data.sweep[index] = level;
            data.sweep_time[index] = now;
            if (level == missing)
                clean = false;
            else if (data.sweep_peak[index] == missing || b.peak > data.sweep_peak[index])
                data.sweep_peak[index] = b.peak;
        }
        data.sweep_done = std::min<unsigned>(start + 8, sweep_count(data.sweep_stride));
        return clean;
    }
    void finish_sweep(int64_t now, int64_t started) {
        data.sweep_invalid = 0;
        for (unsigned i = 0; i < SWEEP; i += data.sweep_stride)
            if (data.sweep[i] == missing)
                ++data.sweep_invalid;
        auto &row = data.rows[data.row_head];
        std::copy_n(data.sweep, SWEEP, row.level);
        row.completed_us = now;
        data.row_head = (data.row_head + 1) % WATER_ROWS;
        data.row_count = std::min<unsigned>(data.row_count + 1, WATER_ROWS);
        ++data.sweeps;
        data.sweep_completed_us = now;
        data.sweep_seconds = (now - started) / 1e6f;
    }
};
inline unsigned imd_candidates(const bool *planned, uint16_t *out, unsigned cap,
                               const uint16_t *selected = frequencies) {
    unsigned n = 0;
    auto mark = [&](int f) {
        if (f < 5645 || f > 5917 || n == cap)
            return;
        for (unsigned i = 0; i < n; i++)
            if (out[i] == f)
                return;
        out[n++] = uint16_t(f);
    };
    for (unsigned a = 0; a < N; a++)
        if (planned[a])
            for (unsigned b = 0; b < N; b++)
                if (a != b && planned[b])
                    mark(2 * selected[a] - selected[b]);
    for (unsigned a = 0; a < N; a++)
        if (planned[a])
            for (unsigned b = a + 1; b < N; b++)
                if (planned[b])
                    for (unsigned c = 0; c < N; c++)
                        if (c != a && c != b && planned[c])
                            mark(selected[a] + selected[b] - selected[c]);
    std::sort(out, out + n);
    return n;
}
} // namespace rf
