#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace c5 {
inline uint16_t u16(const uint8_t *p) {
    return uint16_t(p[0]) | uint16_t(p[1]) << 8;
}
inline uint32_t u32(const uint8_t *p) {
    return uint32_t(u16(p)) | uint32_t(u16(p + 2)) << 16;
}
inline uint16_t crc(const uint8_t *p, size_t n) {
    uint16_t v = 0xffff;
    while (n--) {
        v ^= *p++;
        for (int i = 0; i < 8; i++)
            v = (v & 1) ? (v >> 1) ^ 0x8408 : v >> 1;
    }
    return v ^ 0xffff;
}
class Decoder {
  public:
    using Callback = void (*)(uint8_t, const uint8_t *, size_t, void *);
    uint32_t crc_errors = 0, bad_lengths = 0;
    void reset() {
        used_ = 0;
        crc_errors = 0;
        bad_lengths = 0;
    }
    void feed(const uint8_t *data, size_t length, Callback callback, void *context) {
        while (length--) {
            if (used_ == sizeof(buffer_))
                discard(1);
            buffer_[used_++] = *data++;
            while (used_ >= 2) {
                if (buffer_[0] != 0xa5 || buffer_[1] != 0x5a) {
                    discard(1);
                    continue;
                }
                if (used_ < 5)
                    break;
                size_t n = u16(buffer_ + 3);
                if (n > 4096) {
                    ++bad_lengths;
                    discard(1);
                    continue;
                }
                if (used_ < n + 7)
                    break;
                if (crc(buffer_ + 2, n + 3) != u16(buffer_ + 5 + n)) {
                    ++crc_errors;
                    discard(1);
                    continue;
                }
                callback(buffer_[2], buffer_ + 5, n, context);
                discard(n + 7);
            }
        }
    }

  private:
    uint8_t buffer_[4103]{};
    size_t used_ = 0;
    void discard(size_t n) {
        used_ -= n;
        if (used_)
            std::memmove(buffer_, buffer_ + n, used_);
    }
};
constexpr uint16_t raceband[8] = {5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917};
struct Reading {
    uint32_t source_us;
    int16_t raw, filtered;
    uint16_t mhz;
    uint8_t flags, gain, clips;
};
struct Record {
    uint16_t seq;
    uint32_t dropped;
    uint8_t count;
    Reading channels[8];
};
inline bool read_record(const uint8_t *p, size_t n, Record &r) {
    if (n < 12 || p[2] < 1 || p[2] > 8 || n != 12 + 14u * p[2])
        return false;
    r.seq = u16(p);
    r.dropped = u32(p + 4);
    r.count = p[2];
    for (unsigned i = 0; i < r.count; i++) {
        const uint8_t *q = p + 12 + 14 * i;
        r.channels[i] = {u32(q), int16_t(u16(q + 4)), int16_t(u16(q + 6)), u16(q + 8), q[12], q[10], q[11]};
    }
    return true;
}
inline bool is_raceband(const Record &r) {
    if (r.count != 8)
        return false;
    for (unsigned i = 0; i < 8; i++)
        if (r.channels[i].mhz != raceband[i])
            return false;
    return true;
}
} // namespace c5