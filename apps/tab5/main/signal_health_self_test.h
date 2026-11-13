#pragma once
#include "signal_health.h"
#include <cassert>
namespace signal_health {
inline Reference test_reference() {
    Reference r;
    r.mhz = 5917;
    r.gain = 30;
    r.offset = 10;
    r.bw = 1;
    r.samples = 256;
    std::strcpy(r.serial, "test-device");
    std::strcpy(r.firmware, "1.2.5+xiaoc5");
    r.off_median = r.off_p95 = -300;
    r.median = 2000;
    r.p10 = 1990;
    r.p90 = 2010;
    r.minimum = 1980;
    seal(r);
    return r;
}
inline void self_test() {
    auto ref = test_reference();
    assert(valid(ref));
    assert(compatible(ref, 5917, "test-device", "1.2.5+xiaoc5"));
    assert(!compatible(ref, 5880, "test-device", "1.2.5+xiaoc5"));
    assert(!compatible(ref, 5917, "other", "1.2.5+xiaoc5"));
    auto corrupted = ref;
    corrupted.median++;
    assert(!valid(corrupted));
    Capture cap;
    Reference context = ref;
    context.off_median = context.off_p95 = context.median = context.p10 = context.p90 = context.minimum =
        missing;
    context.checksum = 0;
    cap.prepare(3, context, 0);
    assert(cap.busy());
    cap.start_off(0);
    cap.push(-300, true);
    for (unsigned i = 0; i < OFF_BINS; i++)
        cap.push(-300, true);
    assert(cap.phase == Phase::AwaitOn && cap.candidate.off_p95 == -300);
    assert(cap.start_on(4000000));
    cap.push(2000, true);
    for (unsigned i = 0; i < WINDOW; i++)
        cap.push(int16_t(2000 + int(i % 3) * 10 - 10), true);
    assert(cap.phase == Phase::Ready && valid(cap.candidate));
    assert(!cap.start_on(0));
    cap.prepare(3, context, 0);
    cap.start_off(0);
    cap.push(-300, true);
    cap.push(missing, false);
    assert(cap.phase == Phase::Failed && cap.error == Error::InvalidData && !cap.busy());
    for (int scenario = 0; scenario < 2; scenario++) {
        cap.prepare(3, context, 0);
        cap.start_off(0);
        cap.push(-300, true);
        for (unsigned i = 0; i < OFF_BINS; i++)
            cap.push(-300, true);
        cap.start_on(0);
        cap.push(2000, true);
        for (unsigned i = 0; i < WINDOW; i++)
            cap.push(scenario ? int16_t(2000 + int(i % 2) * 600) : 500, true);
        assert(cap.phase == Phase::Failed && cap.error == (scenario ? Error::Unstable : Error::TooWeak));
    }
    Monitor m;
    Result r;
    int64_t bin = 0;
    for (unsigned i = 0; i < WINDOW - 1; i++)
        r = m.push(bin++, 2000, true, ref);
    assert(r.level == Grade::Gray && r.count == 99);
    r = m.push(bin++, 2000, true, ref);
    assert(r.level == Grade::Green && r.stability == Grade::Green);
    for (unsigned i = 0; i < 110; i++)
        r = m.push(bin++, 1000, true, ref);
    assert(r.level == Grade::Yellow && r.level_reason == Reason::LevelDrop && r.stability == Grade::Green);
    for (unsigned i = 0; i < 110; i++)
        r = m.push(bin++, 500, true, ref);
    assert(r.level == Grade::Red);
    for (unsigned i = 0; i < 110; i++)
        r = m.push(bin++, -250, true, ref);
    assert(r.level == Grade::Red && r.level_reason == Reason::LowRise);
    m.reset();
    bin = 0;
    for (unsigned i = 0; i < WINDOW; i++)
        r = m.push(bin++, i == 20 || i == 50 || i == 80 ? 1100 : 2000, true, ref);
    assert(r.stability == Grade::Red && r.dips == 3 && r.longest_dip == 1 && r.level == Grade::Green);
    m.reset();
    bin = 0;
    for (unsigned i = 0; i < WINDOW; i++)
        r = m.push(bin++, int16_t(2000 + int(i % 2) * 400), true, ref);
    assert(r.stability == Grade::Yellow);
    m.reset();
    bin = 0;
    for (unsigned i = 0; i < WINDOW; i++)
        r = m.push(bin++, int16_t(2000 + int(i % 2) * 800), true, ref);
    assert(r.stability == Grade::Red);
    r = m.push(bin++, missing, false, ref);
    assert(r.level == Grade::Gray && m.count == 0);
    r = m.push(bin++, 2000, true, ref);
    assert(r.level == Grade::Gray && r.count == 1);
    r = m.push(bin + 2, 2000, true, ref);
    assert(r.level == Grade::Gray && r.count == 1);
    // A sustained weak signal can be stable; do not conflate level and stability.
    m.reset();
    bin = 0;
    for (unsigned i = 0; i < WINDOW; i++)
        r = m.push(bin++, 500, true, ref);
    assert(r.level == Grade::Red && r.stability == Grade::Green);
    // Recovery is held for one second (10 bins), then releases.
    Latch latch;
    assert(latch.update(Grade::Red) == Grade::Red);
    for (unsigned i = 0; i < 9; i++)
        assert(latch.update(Grade::Green) == Grade::Red);
    assert(latch.update(Grade::Green) == Grade::Green);
}
} // namespace signal_health
