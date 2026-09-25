#pragma once

#include <array>
#include <algorithm>
#include <cstdint>

namespace duotcp {

constexpr std::uint32_t kOutputSampleRate = 2'048'000;
constexpr double kAdcSampleRate = 8'192'000.0;
constexpr std::uint16_t kDefaultPortA = 1240;
constexpr std::uint16_t kDefaultPortB = 1241;
constexpr std::uint32_t kRtlTunerTypeR820T = 5;

enum class RtlCommand : std::uint8_t {
    SetFrequency = 0x01,
    SetSampleRate = 0x02,
    SetGainMode = 0x03,
    SetGain = 0x04,
    SetFrequencyCorrection = 0x05,
    SetBiasTee = 0x0e,
};

inline std::uint32_t read_be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

inline void write_be32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>((v >> 24) & 0xff);
    p[1] = static_cast<std::uint8_t>((v >> 16) & 0xff);
    p[2] = static_cast<std::uint8_t>((v >> 8) & 0xff);
    p[3] = static_cast<std::uint8_t>(v & 0xff);
}

inline std::array<std::uint8_t, 12> rtl_tcp_handshake() {
    std::array<std::uint8_t, 12> h{};
    h[0] = 'R'; h[1] = 'T'; h[2] = 'L'; h[3] = '0';
    write_be32(h.data() + 4, kRtlTunerTypeR820T);
    write_be32(h.data() + 8, 0);
    return h;
}

inline std::uint8_t sample_to_u8(std::int16_t sample) {
    std::int32_t v = (static_cast<std::int32_t>(sample) >> 6) + 128;
    return static_cast<std::uint8_t>(std::clamp(v, 0, 255));
}

} // namespace duotcp
