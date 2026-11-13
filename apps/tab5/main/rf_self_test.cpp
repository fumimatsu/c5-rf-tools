#include "rf_core.h"
#include "signal_health_self_test.h"
#include <cassert>
void rf_self_test() {
    signal_health::self_test();
    // The power mean of 0 dB and 10 dB is 7.40 dB, never their 5 dB arithmetic mean.
    rf::Average a;
    c5::Reading r{};
    r.gain = 30;
    r.raw = r.filtered = 0;
    a.add(r);
    r.raw = r.filtered = 1000;
    a.add(r);
    assert(std::abs(a.mean() - 740) <= 1);
    r.flags = 4;
    a.add(r);
    assert(a.bad == 1 && a.count == 2);
    r.flags = 0;
    r.clips = 1;
    a.add(r);
    assert(a.bad == 2);
    r.clips = 0;
    r.gain = 31;
    a.add(r);
    assert(a.bad == 3);
    c5::Record record{};
    record.count = 4;
    for (unsigned i = 0; i < 4; i++)
        record.channels[i].mhz = rf::frequencies[i];
    assert(rf::matches(record, rf::frequencies, 4));
    record.channels[2].mhz++;
    assert(!rf::matches(record, rf::frequencies, 4));
    auto *model = new rf::Model;
    assert(model);
    auto &d = model->data;
    d.usb = d.firmware_ok = d.radio = true;
    d.settings.gain = 30;
    d.settings.offset = 10;
    d.settings.bw = 1;
    d.settings.noden = 256;
    d.settings.fast = d.settings.scan = true;
    d.applied_count = 4;
    std::copy_n(rf::frequencies, 4, d.applied);
    model->set_profile(rf::frequencies, 4, 0);
    d.mode = rf::Mode::Monitor;
    for (unsigned i = 0; i < 4; i++) {
        record.channels[i] = {1000, 100, 100, rf::frequencies[i], 0, 30, 0};
    }
    model->ingest(record, 200000);
    model->ingest(record, 210000);
    model->ingest(record, 220000);
    model->expire_bin(3);
    assert(d.history[2].valid_mask == 15);
    const auto last = model->data.last_bin;
    model->expire_bin(2);
    assert(model->bucket_index == 3 && model->data.last_bin == last);
    model->expire_bin(9);
    assert(d.history[8].index != 8); // real six-bin gap, not interpolated data
    d.mode = rf::Mode::BackgroundMonitor;
    model->begin_background();
    rf::Background bg{};
    for (unsigned bin = 20; bin < 72; bin++) {
        for (unsigned sample = 0; sample < 4; sample++) {
            for (auto &x : record.channels) {
                x.source_us += 100;
                x.raw = x.filtered = 100;
            }
            model->ingest(record, bin * rf::bin_us + sample * 1000);
        }
    }
    assert(model->bg_count == 50 && !model->bg_failed);
    assert(model->monitor_background(bg));
    for (auto x : bg.monitor)
        assert(x == 100);
    model->bg_bins[2][10] = 1800;
    assert(!model->monitor_background(bg) && model->bg_unstable);
    model->bg_bins[2][10] = 100;
    model->bg_unstable = false;
    assert(model->monitor_background(bg));
    bg.gain = 30;
    bg.offset = 10;
    bg.bw = 1;
    bg.samples = 256;
    std::strcpy(bg.serial, "test-device");
    std::strcpy(bg.firmware, "1.2.5+xiaoc5");
    std::fill_n(bg.sweep, rf::SWEEP, 100);
    std::copy_n(rf::frequencies, rf::N, bg.monitor_mhz);
    rf::seal(bg);
    assert(rf::background_valid(bg));
    assert(rf::compatible(bg, "test-device", "1.2.5+xiaoc5", d.settings));
    assert(!rf::compatible(bg, "other-device", "1.2.5+xiaoc5", d.settings));
    bg.sweep[8]++;
    assert(!rf::background_valid(bg));
    bg.sweep[8]--;
    rf::seal(bg);
    d.background = bg;
    d.background_active = true;
    d.mode = rf::Mode::Monitor;
    model->clear_history();
    d.channel[0].planned = false;
    for (unsigned bin = 100; bin < 106; bin++) {
        for (unsigned sample = 0; sample < 4; sample++) {
            for (auto &x : record.channels) {
                x.source_us += 100;
                x.raw = x.filtered = 1200;
            }
            model->ingest(record, bin * rf::bin_us + sample * 1000);
        }
    }
    assert(d.channel[0].alert);
    assert(!d.channel[1].alert);
    model->expire_bin(110);
    assert(!d.channel[0].alert);
    model->set_sweep_stride(1);
    model->begin_sweep();
    d.mode = rf::Mode::Search;
    for (auto &b : model->sweep_bucket) {
        r.gain = 30;
        r.clips = r.flags = 0;
        r.raw = r.filtered = 300;
        for (int i = 0; i < 4; i++)
            b.add(r);
    }
    assert(model->finish_group(136, 2000000));
    assert(d.sweep[136] == 300 && d.sweep_done == 137);
    model->finish_sweep(3000000, 1000000);
    assert(d.row_count == 1 && d.sweep_seconds == 2.f);
    d.sweep[100] = rf::missing;
    model->finish_sweep(4000000, 3000000);
    assert(d.rows[1].level[100] == rf::missing);
    // Fast scans preserve true frequency indices, never invent odd-grid samples.
    const auto saved_background = d.background;
    model->set_sweep_stride(2);
    assert(rf::sweep_count(2) == 69 && rf::sweep_count(1) == 137);
    assert(rf::sweep_frequency(rf::sweep_index(68, 2)) == 5917);
    assert(rf::nearest_sweep_index(135, 2) == 136);
    assert(d.row_count == 0 && d.sweep[136] == rf::missing && d.sweep_peak[136] == rf::missing);
    for (unsigned group = 0; group < rf::sweep_count(2); group += 8) {
        for (unsigned slot = 0; slot < 8; slot++) {
            auto &b = model->sweep_bucket[slot];
            b = rf::Average{};
            r.raw = r.filtered = int16_t(group + slot);
            for (int sample = 0; sample < 4; sample++)
                b.add(r);
        }
        assert(model->finish_group(group, 5000000));
    }
    assert(d.sweep_done == 69 && d.sweep[136] == 68); // padded duplicates ignored
    for (unsigned index = 0; index < rf::SWEEP; index++)
        assert(index % 2 ? d.sweep[index] == rf::missing : d.sweep[index] == int16_t(index / 2));
    model->finish_sweep(6000000, 5000000);
    assert(d.sweep_invalid == 0 && d.row_count == 1);
    d.sweep[4] = rf::missing;
    model->finish_sweep(7000000, 6000000);
    assert(d.sweep_invalid == 1); // actual failed point is counted; skipped odd indices are not
    model->set_sweep_stride(1);
    assert(d.row_count == 0 && d.sweep_time[136] == 0 && d.sweep_invalid == 0);
    assert(!std::memcmp(&saved_background, &d.background, sizeof(saved_background)));
    model->set_sweep_stride(7);
    assert(d.sweep_stride == 1); // invalid selector cannot break indexing
    bool planned[4] = {true, true, false, false};
    uint16_t candidates[rf::SWEEP];
    const unsigned n = rf::imd_candidates(planned, candidates, rf::SWEEP);
    assert(n == 2 && candidates[0] == 5665 && candidates[1] == 5725);
    d.channel[0].alert = true;
    d.channel[0].votes = 31;
    model->set_profile(rf::frequencies, 4, 0);
    assert(!d.channel[0].alert && d.channel[0].votes == 0);
    auto plan = rf::channel_set(0);
    assert(rf::plan_valid(plan));
    uint16_t selected[rf::N];
    rf::plan_frequencies(plan, selected);
    assert(std::equal(selected, selected + rf::N, rf::frequencies));
    assert(rf::presets[rf::preset_id("R8")].mhz == 5917);
    assert(rf::preset_enabled(7) && !rf::preset_enabled(30) && !rf::preset_enabled(31));
    assert(rf::preset_id("Z1") == -1);
    plan.ids[0] = 7;
    assert(!rf::plan_valid(plan)); // changing an unsealed selection is detected
    rf::seal_plan(plan);
    assert(rf::plan_valid(plan));
    plan.ids[1] = 30;
    rf::seal_plan(plan);
    assert(!rf::plan_valid(plan) && !model->select_channels(plan));
    plan = rf::channel_set(1);
    rf::plan_frequencies(plan, selected);
    assert(selected[0] == 5695 && selected[1] == 5732 && selected[2] == 5769 && selected[3] == 5806);
    assert(!rf::compatible(bg, "test-device", "1.2.5+xiaoc5", d.settings, selected));
    d.background_active = true;
    d.channel[0].peak = 1234;
    d.channel[0].alert = true;
    assert(model->select_channels(plan));
    assert(!d.background_active && d.row_count == 0 && d.channel[0].peak == rf::missing &&
           !d.channel[0].alert);
    assert(d.history[105 % rf::HISTORY].index == -1 && d.sweep[136] == rf::missing);
    bool first_two[4] = {true, true, false, false};
    assert(rf::imd_candidates(first_two, candidates, rf::SWEEP, selected) == 2);
    assert(candidates[0] == 5658 && candidates[1] == 5769);
    // Duplicate RF frequencies preserve the selected name, and remain a valid plan.
    plan.ids[0] = 6;
    plan.ids[1] = 39;
    rf::seal_plan(plan);
    assert(rf::plan_valid(plan));
    rf::plan_frequencies(plan, selected);
    assert(selected[0] == selected[1]);
    // Upgrade the original persisted format without changing its measured arrays.
    rf::BackgroundV1 old{};
    old.magic = 0x52463442;
    old.version = 1;
    old.size = sizeof(old);
    old.revision = 3;
    old.gain = bg.gain;
    old.offset = bg.offset;
    old.bw = bg.bw;
    old.samples = bg.samples;
    std::memcpy(old.serial, bg.serial, sizeof(old.serial));
    std::memcpy(old.firmware, bg.firmware, sizeof(old.firmware));
    std::copy_n(bg.monitor, rf::N, old.monitor);
    std::copy_n(bg.upper, rf::N, old.upper);
    std::copy_n(bg.sweep, rf::SWEEP, old.sweep);
    old.checksum = rf::hash_bytes(&old, offsetof(rf::BackgroundV1, checksum));
    rf::Background upgraded{};
    assert(rf::upgrade_background(old, upgraded));
    assert(upgraded.version == 2 && upgraded.revision == 3);
    assert(std::equal(upgraded.monitor, upgraded.monitor + rf::N, bg.monitor));
    assert(std::equal(upgraded.sweep, upgraded.sweep + rf::SWEEP, bg.sweep));
    assert(rf::compatible(upgraded, "test-device", "1.2.5+xiaoc5", d.settings));
    old.checksum++;
    assert(!rf::upgrade_background(old, upgraded));
    plan = rf::channel_set(0);
    plan.ids[3] = 7;
    rf::seal_plan(plan);
    assert(model->select_channels(plan));
    rf::plan_frequencies(plan, selected);
    model->set_profile(selected, rf::N, 0);
    d.applied_count = rf::N;
    std::copy_n(selected, rf::N, d.applied);
    d.mode = rf::Mode::Monitor;
    std::strcpy(d.serial, "test-device");
    std::strcpy(d.firmware, "1.2.5+xiaoc5");
    // End the earlier background-recording fixture before evaluating live health.
    d.background_busy = false;
    d.normal_refs[7] = signal_health::test_reference();
    d.normal_active[3] = true;
    for (unsigned bin = 200; bin < 305; bin++)
        for (unsigned sample = 0; sample < 4; sample++) {
            for (unsigned i = 0; i < rf::N; i++)
                record.channels[i] = {bin * 100000 + sample * 1000, 2000, 2000, selected[i], 0, 30, 0};
            model->ingest(record, bin * rf::bin_us + sample * 1000);
        }
    auto result = rf::visible_normal(d, 3, 304 * rf::bin_us + 3000);
    assert(result.level == signal_health::Grade::Green && result.stability == signal_health::Grade::Green);
    const auto reference_checksum = d.normal_refs[7].checksum;
    model->transport_fault(rf::CrcError, 304 * rf::bin_us + 4000);
    result = rf::visible_normal(d, 3, 304 * rf::bin_us + 4000);
    assert(result.level == signal_health::Grade::Gray && result.level_reason == signal_health::Reason::Transport);
    // A later valid sample in this same bin cannot make the incomplete bin clean.
    model->ingest(record, 304 * rf::bin_us + 5000);
    model->expire_bin(305);
    assert(d.history[304 % rf::HISTORY].valid_mask == 0 && d.history[304 % rf::HISTORY].bad_mask == 15);
    assert(d.normal_active[3] && d.normal_refs[7].checksum == reference_checksum);
    for (unsigned bin = 305; bin < 408; bin++)
        for (unsigned sample = 0; sample < 4; sample++) {
            for (unsigned i = 0; i < rf::N; i++)
                record.channels[i] = {bin * 100000 + sample * 1000, 2000, 2000, selected[i], 0, 30, 0};
            model->ingest(record, bin * rf::bin_us + sample * 1000);
        }
    result = rf::visible_normal(d, 3, 407 * rf::bin_us + 3000);
    assert(result.level == signal_health::Grade::Green && result.stability == signal_health::Grade::Green);
    d.normal_capture.phase = signal_health::Phase::Off;
    model->transport_fault(rf::SequenceGap, 407 * rf::bin_us + 4000);
    assert(d.normal_capture.phase == signal_health::Phase::Failed &&
           d.normal_capture.error == signal_health::Error::Transport);
    d.normal_capture.phase = signal_health::Phase::Normal;
    model->transport_fault(rf::ChipDrop, 407 * rf::bin_us + 5000);
    assert(d.normal_capture.phase == signal_health::Phase::Failed &&
           d.normal_capture.error == signal_health::Error::Transport);
    assert(d.normal_refs[7].checksum == reference_checksum && signal_health::valid(d.normal_refs[7]));
    d.normal_capture.phase = signal_health::Phase::Idle;
    d.background_busy = true;
    assert(rf::visible_normal(d, 3, 304 * rf::bin_us + 3000).level_reason ==
           signal_health::Reason::Recording);
    d.background_busy = false;
    d.mode = rf::Mode::Search;
    assert(rf::visible_normal(d, 3, 304 * rf::bin_us + 3000).level == signal_health::Grade::Gray);
    d.mode = rf::Mode::Monitor;
    d.channel[3].planned = false;
    assert(rf::visible_normal(d, 3, 304 * rf::bin_us + 3000).level == signal_health::Grade::Gray);
    d.channel[3].planned = true;
    d.channel[3].valid = false;
    assert(rf::visible_normal(d, 3, 304 * rf::bin_us + 3000).level == signal_health::Grade::Gray);
    // A C5 clock rollback must remain an incomplete bin even if invalidating
    // the adopted background clears the presentation/history buffers.
    d.background_active = true;
    d.normal_capture.phase = signal_health::Phase::Off;
    for (auto &x : record.channels) x.source_us = 100;
    model->ingest(record, 408 * rf::bin_us + 1000);
    model->expire_bin(409);
    assert(!d.background_active && d.history[408 % rf::HISTORY].valid_mask == 0);
    assert(d.history[408 % rf::HISTORY].bad_mask == 15);
    assert(d.normal_capture.error == signal_health::Error::Transport);
    assert(d.normal_refs[7].checksum == reference_checksum);
    delete model;
}
