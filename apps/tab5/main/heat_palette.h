#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// Display scale only: receiver dB is not dBm or a quality/fault classification.
namespace heat_palette {
inline uint32_t jet(float normalized) {
    constexpr uint32_t stops[] = {0x0000ff, 0x00ffff, 0x00ff00, 0xffff00, 0xff0000};
    if (!std::isfinite(normalized))
        return 0x24292d;
    const float z = std::clamp(normalized, 0.f, 1.f) * 4.f;
    const unsigned i = std::min(3u, unsigned(z));
    const float f = z - float(i);
    uint32_t result = 0;
    for (unsigned shift : {16u, 8u, 0u}) {
        const unsigned a = (stops[i] >> shift) & 255, b = (stops[i + 1] >> shift) & 255;
        result |= uint32_t(std::lround(a * (1.f - f) + b * f)) << shift;
    }
    return result;
}
}
