#pragma once

#include <array>
#include <optional>

#include "gba/core/constants.hpp"
#include "gba/core/types.hpp"

namespace gba {

class IrqController;

class Ppu {
public:
    void reset();

    [[nodiscard]] u32 read_register(u32 address, BusWidth width) const;
    void write_register(u32 address, u32 value, BusWidth width);

    void advance_to(u64 cycle_now, IrqController& irq);
    void render_scanline(int line, std::span<const u8> vram, std::span<const u8> palette);

    [[nodiscard]] u64 next_event_cycle() const;
    [[nodiscard]] bool is_hblank() const;
    [[nodiscard]] bool is_vblank() const;
    [[nodiscard]] bool is_video_memory_contended() const;
    [[nodiscard]] bool frame_ready() const;
    [[nodiscard]] std::optional<int> consume_scanline_ready();
    bool consume_frame_ready();

    [[nodiscard]] const std::array<u16, kFramebufferPixels>& framebuffer() const;

    [[nodiscard]] u16 dispcnt() const;
    [[nodiscard]] u16 dispstat() const;
    [[nodiscard]] u16 vcount() const;
    [[nodiscard]] bool force_blank() const;
    [[nodiscard]] bool hblank_free() const;

private:
    void render_text_bg(int line, std::span<const u8> vram, std::span<const u8> palette, int bg, u16* row);
    void render_affine_bg(int line, std::span<const u8> vram, std::span<const u8> palette, int bg, u16* row);
    void update_dispstat_flags();
    void enter_hblank(IrqController& irq);
    void leave_hblank(IrqController& irq);
    void enter_vblank(IrqController& irq);
    void leave_vblank();
    void handle_vcount_compare(IrqController& irq);

    u16 dispcnt_ = 0x0080;
    u16 greenswp_ = 0;
    u16 dispstat_ = 0;
    u16 vcount_ = 0;
    std::array<u16, 4> bgcnt_{};
    std::array<u16, 4> bghofs_{};
    std::array<u16, 4> bgvofs_{};
    std::array<u16, 2> winh_{};
    std::array<u16, 2> winv_{};
    u16 winin_ = 0;
    u16 winout_ = 0;
    u16 mosaic_ = 0;
    u16 bldcnt_ = 0;
    u16 bldalpha_ = 0;
    u16 bldy_ = 0;

    std::array<s16, 2> bg_pa_{};
    std::array<s16, 2> bg_pb_{};
    std::array<s16, 2> bg_pc_{};
    std::array<s16, 2> bg_pd_{};
    std::array<s32, 2> bg_ref_x_{};
    std::array<s32, 2> bg_ref_y_{};

    std::array<u16, kFramebufferPixels> framebuffer_{};
    u64 next_event_cycle_ = 960;
    bool hblank_ = false;
    bool vblank_ = false;
    bool frame_ready_ = false;
    std::optional<int> scanline_ready_;
};

}  // namespace gba
