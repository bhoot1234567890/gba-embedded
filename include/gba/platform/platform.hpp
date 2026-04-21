#pragma once

#include <span>
#include <string>

#include "gba/core/constants.hpp"
#include "gba/core/types.hpp"

namespace gba {

struct PlatformInfo {
    std::string name;
    u32 cpu_hz = 0;
    u32 ram_bytes = 0;
    bool dual_core = false;
};

class Platform {
public:
    virtual ~Platform() = default;

    [[nodiscard]] virtual PlatformInfo info() const = 0;
    virtual void init() = 0;
    virtual void present_frame(std::span<const u16> framebuffer) = 0;
    virtual void push_audio(std::span<const s16> samples) = 0;
    [[nodiscard]] virtual u16 read_keys() = 0;
};

}  // namespace gba
