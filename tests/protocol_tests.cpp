#include "rtl_tcp_protocol.h"
#include <cassert>
#include <cstdint>
#include <iostream>

int main() {
    const auto h = duotcp::rtl_tcp_handshake();
    assert(h[0] == 'R' && h[1] == 'T' && h[2] == 'L' && h[3] == '0');
    assert(duotcp::read_be32(h.data() + 4) == duotcp::kRtlTunerTypeR820T);
    assert(duotcp::read_be32(h.data() + 8) == 0);
    const std::uint8_t cmd[] = {0x01, 0x05, 0xF5, 0xE1, 0x00};
    assert(duotcp::read_be32(cmd + 1) == 100000000u);
    assert(duotcp::sample_to_u8(0) == 128);
    assert(duotcp::sample_to_u8(8191) == 255);
    assert(duotcp::sample_to_u8(-8192) == 0);
    std::cout << "protocol tests passed\n";
    return 0;
}
