#include "gba/platform/rt1170_platform.hpp"

namespace gba {

PlatformInfo Rt1170Platform::info() const {
    return PlatformInfo{
        .name = "NXP i.MX RT1170",
        .cpu_hz = 1'000'000'000u,
        .ram_bytes = 2u * 1024u * 1024u,
        .dual_core = true,
    };
}

void Rt1170Platform::init() {}

void Rt1170Platform::present_frame(std::span<const u16> framebuffer) {
    (void)framebuffer;
}

void Rt1170Platform::push_audio(std::span<const s16> samples) {
    (void)samples;
}

u16 Rt1170Platform::read_keys() {
    return 0x03FFu;
}

}  // namespace gba
