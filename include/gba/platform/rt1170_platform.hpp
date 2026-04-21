#pragma once

#include "gba/platform/platform.hpp"

namespace gba {

class Rt1170Platform final : public Platform {
public:
    [[nodiscard]] PlatformInfo info() const override;
    void init() override;
    void present_frame(std::span<const u16> framebuffer) override;
    void push_audio(std::span<const s16> samples) override;
    [[nodiscard]] u16 read_keys() override;
};

}  // namespace gba
