#pragma once
#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace signal_health {
constexpr unsigned WINDOW = 100, OFF_BINS = 30;
constexpr int16_t missing = INT16_MIN;
enum class Grade : uint8_t { Gray, Green, Yellow, Red };
enum class Reason : uint8_t {
    NoReference,
    Inactive,
    Conditions,
    Paused,
    Invalid,
    Warming,
    Recording,
    PlannedOff,
    Duplicate,
    LevelDrop,
    LowRise,
    Range,
    Dips,
    Within,
    Recovering
};
enum class Phase : uint8_t { Idle, Preparing, Off, AwaitOn, Normal, Ready, Saved, Failed, Cancelled };
enum class Error : uint8_t {
    None,
    InvalidData,
    TooWeak,
    Unstable,
    Conditions,
    Save,
    Cancelled,
    Disconnected,
    Timeout
};
inline const char *grade_name(Grade g) {
    switch (g) {
    case Grade::Green:
        return "green";
    case Grade::Yellow:
        return "yellow";
    case Grade::Red:
        return "red";
    default:
        return "gray";
    }
}
inline const char *phase_name(Phase p) {
    switch (p) {
    case Phase::Preparing:
        return "preparing";
    case Phase::Off:
        return "off";
    case Phase::AwaitOn:
        return "await-on";
    case Phase::Normal:
        return "normal";
    case Phase::Ready:
        return "ready";
    case Phase::Saved:
        return "saved";
    case Phase::Failed:
        return "failed";
    case Phase::Cancelled:
        return "cancelled";
    default:
        return "idle";
    }
}
inline uint32_t hash(const void *data, size_t n) {
    const auto *p = static_cast<const uint8_t *>(data);
    uint32_t h = 2166136261u;
    while (n--)
        h = (h ^ *p++) * 16777619u;
    return h;
}
struct Reference {
    uint32_t magic = 0, version = 0, size = 0;
    uint16_t mhz = 0, reserved = 0;
    int32_t gain = 0, offset = 0, bw = 0, samples = 0;
    char serial[40]{}, firmware[32]{};
    int16_t off_median = missing, off_p95 = missing, median = missing, p10 = missing, p90 = missing,
            minimum = missing;
    uint32_t checksum = 0;
};
inline void seal(Reference &r) {
    r.magic = 0x4e524631;
    r.version = 1;
    r.size = sizeof(r);
    r.checksum = hash(&r, offsetof(Reference, checksum));
}
inline bool valid(const Reference &r) {
    return r.magic == 0x4e524631 && r.version == 1 && r.size == sizeof(r) &&
           r.checksum == hash(&r, offsetof(Reference, checksum)) && r.mhz >= 5645 && r.mhz <= 5917 &&
           std::memchr(r.serial, 0, sizeof(r.serial)) && std::memchr(r.firmware, 0, sizeof(r.firmware)) &&
           r.serial[0] && r.firmware[0] && r.gain == 30 && r.offset == 10 && r.bw == 1 && r.samples == 256 &&
           r.off_median != missing && r.off_p95 >= r.off_median && r.minimum != missing &&
           r.minimum <= r.p10 && r.p10 <= r.median && r.median <= r.p90 && r.p90 - r.p10 <= 300 &&
           r.median - r.minimum <= 600 && r.median - r.off_p95 >= 1000;
}
inline bool compatible(const Reference &r, uint16_t mhz, const char *serial, const char *firmware) {
    return valid(r) && r.mhz == mhz && !std::strcmp(r.serial, serial) && !std::strcmp(r.firmware, firmware);
}
inline int16_t quantile(const int16_t *sorted, unsigned n, unsigned percentile) {
    return sorted[(n - 1) * percentile / 100];
}
struct Result {
    Grade level = Grade::Gray, stability = Grade::Gray;
    Reason level_reason = Reason::NoReference, stability_reason = Reason::NoReference;
    int32_t drop = 0, rise = 0, spread = 0;
    unsigned dips = 0, longest_dip = 0, count = 0;
};
inline Result unknown(Reason why, unsigned count = 0) {
    Result r;
    r.level_reason = r.stability_reason = why;
    r.count = count;
    return r;
}
struct Latch {
    Grade value = Grade::Gray;
    unsigned recovery = 0;
    Grade update(Grade next) {
        if (next >= value || value == Grade::Gray) {
            value = next;
            recovery = 0;
        } else if (++recovery >= 10) {
            value = next;
            recovery = 0;
        }
        return value;
    }
};
struct Monitor {
    int16_t values[WINDOW]{};
    unsigned count = 0, head = 0;
    int64_t last_bin = -1;
    Latch level, stability;
    void reset() {
        count = head = 0;
        last_bin = -1;
        level = Latch{};
        stability = Latch{};
    }
    Result push(int64_t index, int16_t value, bool clean, const Reference &ref) {
        if (!clean || value == missing) {
            reset();
            return unknown(Reason::Invalid);
        }
        if (last_bin >= 0 && index != last_bin + 1)
            reset();
        last_bin = index;
        values[head] = value;
        head = (head + 1) % WINDOW;
        count = std::min(count + 1, WINDOW);
        if (count < WINDOW)
            return unknown(Reason::Warming, count);
        std::array<int16_t, WINDOW> sorted;
        std::copy_n(values, WINDOW, sorted.begin());
        std::sort(sorted.begin(), sorted.end());
        std::array<int16_t, 10> recent;
        for (unsigned i = 0; i < 10; i++)
            recent[i] = values[(head + WINDOW - 10 + i) % WINDOW];
        std::sort(recent.begin(), recent.end());
        Result out;
        out.count = count;
        const int med = quantile(sorted.data(), WINDOW, 50), now = quantile(recent.data(), 10, 50);
        out.drop = ref.median - now;
        out.rise = now - ref.off_p95;
        out.spread = quantile(sorted.data(), WINDOW, 90) - quantile(sorted.data(), WINDOW, 10);
        const int dip_depth = std::max(600, ref.median - ref.p10 + 300);
        unsigned run = 0;
        for (unsigned i = 0; i < WINDOW; i++) {
            const bool low = values[(head + i) % WINDOW] < med - dip_depth;
            if (low) {
                if (!run)
                    ++out.dips;
                out.longest_dip = std::max(out.longest_dip, ++run);
            } else
                run = 0;
        }
        Grade lg = Grade::Green;
        out.level_reason = Reason::Within;
        if (out.drop >= 1200 || out.rise < 600) {
            lg = Grade::Red;
            out.level_reason = out.rise < 600 ? Reason::LowRise : Reason::LevelDrop;
        } else if (out.drop >= 600 || out.rise < 1000) {
            lg = Grade::Yellow;
            out.level_reason = out.rise < 1000 ? Reason::LowRise : Reason::LevelDrop;
        }
        Grade sg = Grade::Green;
        out.stability_reason = Reason::Within;
        if (out.dips >= 3 || out.longest_dip >= 5) {
            sg = Grade::Red;
            out.stability_reason = Reason::Dips;
        } else if (out.dips) {
            sg = Grade::Yellow;
            out.stability_reason = Reason::Dips;
        }
        const int ref_spread = ref.p90 - ref.p10;
        const Grade range_grade = out.spread >= std::max(600, ref_spread + 400)   ? Grade::Red
                                  : out.spread >= std::max(300, ref_spread + 200) ? Grade::Yellow
                                                                                  : Grade::Green;
        if (range_grade > sg) {
            sg = range_grade;
            out.stability_reason = Reason::Range;
        }
        out.level = level.update(lg);
        out.stability = stability.update(sg);
        if (out.level > lg)
            out.level_reason = Reason::Recovering;
        if (out.stability > sg)
            out.stability_reason = Reason::Recovering;
        return out;
    }
};
struct Capture {
    Phase phase = Phase::Idle;
    Error error = Error::None;
    unsigned slot = 0, count = 0;
    bool skip = true;
    int64_t started_us = 0;
    Reference candidate;
    int16_t values[WINDOW]{};
    bool busy() const {
        return phase == Phase::Preparing || phase == Phase::Off || phase == Phase::AwaitOn ||
               phase == Phase::Normal || phase == Phase::Ready;
    }
    void fail(Error why) {
        phase = Phase::Failed;
        error = why;
    }
    void prepare(unsigned target, const Reference &context, int64_t now) {
        slot = target;
        candidate = context;
        phase = Phase::Preparing;
        error = Error::None;
        count = 0;
        skip = true;
        started_us = now;
    }
    void start_off(int64_t now) {
        phase = Phase::Off;
        count = 0;
        skip = true;
        started_us = now;
    }
    bool start_on(int64_t now) {
        if (phase != Phase::AwaitOn)
            return false;
        phase = Phase::Normal;
        count = 0;
        skip = true;
        started_us = now;
        return true;
    }
    void push(int16_t value, bool clean) {
        if (phase != Phase::Off && phase != Phase::Normal)
            return;
        if (skip) {
            skip = false;
            return;
        }
        if (!clean || value == missing) {
            fail(Error::InvalidData);
            return;
        }
        values[count++] = value;
        const unsigned needed = phase == Phase::Off ? OFF_BINS : WINDOW;
        if (count < needed)
            return;
        std::sort(values, values + needed);
        if (phase == Phase::Off) {
            candidate.off_median = quantile(values, needed, 50);
            candidate.off_p95 = quantile(values, needed, 95);
            phase = Phase::AwaitOn;
            count = 0;
            return;
        }
        candidate.minimum = values[0];
        candidate.median = quantile(values, needed, 50);
        candidate.p10 = quantile(values, needed, 10);
        candidate.p90 = quantile(values, needed, 90);
        if (candidate.median - candidate.off_p95 < 1000) {
            fail(Error::TooWeak);
            return;
        }
        if (candidate.p90 - candidate.p10 > 300 || candidate.median - candidate.minimum > 600) {
            fail(Error::Unstable);
            return;
        }
        seal(candidate);
        if (!valid(candidate)) {
            fail(Error::Conditions);
            return;
        }
        phase = Phase::Ready;
    }
};
} // namespace signal_health
