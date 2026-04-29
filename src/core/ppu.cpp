#include "gba/core/ppu.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

#ifdef GBA_PLATFORM_ESP32
#include "esp_heap_caps.h"
#include "esp_attr.h"
#else
#define IRAM_ATTR
#endif

#include "gba/core/constants.hpp"
#include "gba/core/irq.hpp"

namespace gba {

namespace {

void free_video_memory(u16* ptr) {
    if (!ptr) {
        return;
    }
#ifdef GBA_PLATFORM_ESP32
    heap_caps_free(ptr);
#else
    delete[] ptr;
#endif
}

u16* alloc_video_memory(size_t size) {
#ifdef GBA_PLATFORM_ESP32
    auto* ptr = static_cast<u16*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
    if (ptr) {
        std::memset(ptr, 0, size);
        return ptr;
    }
    ptr = static_cast<u16*>(heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (ptr) {
        std::memset(ptr, 0, size);
        return ptr;
    }
#else
    return new u16[size / sizeof(u16)]();
#endif
    return nullptr;
}

}  // namespace

Ppu::Ppu() : framebuffer_(alloc_video_memory(kFramebufferPixels * sizeof(u16)), free_video_memory) {
    mark_all_dirty();
}

namespace {

[[nodiscard]] u16 read16(std::span<const u8> bytes, u32 offset) {
    if (bytes.empty()) {
        return 0;
    }
    const auto base = align_down(offset, 2u);
    return static_cast<u16>(bytes[base % bytes.size()] | (bytes[(base + 1u) % bytes.size()] << 8u));
}

struct BgCnt {
    u16 raw;
    [[nodiscard]] u32 priority() const { return raw & 0x3u; }
    [[nodiscard]] u32 char_base_block() const { return (raw >> 2u & 0x3u) * 0x4000u; }
    [[nodiscard]] bool mosaic() const { return (raw >> 6u & 1u) != 0; }
    [[nodiscard]] u32 color_mode() const { return raw >> 7u & 1u; }  // 0=4bpp, 1=8bpp
    [[nodiscard]] u32 screen_base_block() const { return (raw >> 8u & 0x1Fu) * 0x800u; }
    [[nodiscard]] bool wraparound() const { return (raw >> 13u & 1u) != 0; }
    [[nodiscard]] u32 size() const { return raw >> 14u & 0x3u; }
};

constexpr u8 kSpriteWidths[3][4] = {
    {8, 16, 32, 64}, {16, 32, 32, 64}, {8, 8, 16, 32}
};
constexpr u8 kSpriteHeights[3][4] = {
    {8, 16, 32, 64}, {8, 8, 16, 32}, {16, 32, 32, 64}
};

}  // namespace

void Ppu::reset() {
    dispcnt_ = 0x0080;
    greenswp_ = 0;
    dispstat_ = 0;
    vcount_ = 0;
    bgcnt_.fill(0);
    bghofs_.fill(0);
    bgvofs_.fill(0);
    winh_.fill(0);
    winv_.fill(0);
    winin_ = 0;
    winout_ = 0;
    mosaic_ = 0;
    bldcnt_ = 0;
    bldalpha_ = 0;
    bldy_ = 0;
    bg_pa_.fill(0);
    bg_pb_.fill(0);
    bg_pc_.fill(0);
    bg_pd_.fill(0);
    bg_ref_x_.fill(0);
    bg_ref_y_.fill(0);
    if (framebuffer_) {
        std::fill_n(framebuffer_.get(), kFramebufferPixels, u16{0x7FFF});
    }
    next_event_cycle_ = kHDrawCycles;
    hblank_ = false;
    vblank_ = false;
    frame_ready_ = false;
    scanline_ready_.reset();
    sprite_scanline_count_.fill(0);
    sprite_cache_valid_ = false;
    mark_all_dirty();
    update_dispstat_flags();
}

u32 Ppu::read_register(u32 address, BusWidth width) const {
    auto read_half = [&](u32 half_address) -> u16 {
        switch (half_address) {
        case kDispcnt: return dispcnt_;
        case kDispcnt + 2u: return greenswp_;
        case kDispstat: return dispstat_;
        case kVcount: return vcount_;
        case kBg0Cnt:
        case kBg0Cnt + 2u:
        case kBg0Cnt + 4u:
        case kBg0Cnt + 6u: {
            const auto index = (half_address - kBg0Cnt) / 2u;
            const auto mask = index < 2u ? 0xDFFFu : 0xFFFFu;
            return static_cast<u16>(bgcnt_[index] & mask);
        }
        case 0x04000010u:
        case 0x04000014u:
        case 0x04000018u:
        case 0x0400001Cu: return bghofs_[(half_address - 0x04000010u) / 4u];
        case 0x04000012u:
        case 0x04000016u:
        case 0x0400001Au:
        case 0x0400001Eu: return bgvofs_[(half_address - 0x04000012u) / 4u];
        case kWin0H:
        case kWin0H + 2u: return winh_[(half_address - kWin0H) / 2u];
        case 0x04000044u:
        case 0x04000046u: return winv_[(half_address - 0x04000044u) / 2u];
        case 0x04000048u: return static_cast<u16>(winin_ & 0x3F3Fu);
        case 0x0400004Au: return static_cast<u16>(winout_ & 0x3F3Fu);
        case kBg2Pa:
        case 0x04000030u: return static_cast<u16>(bg_pa_[(half_address - kBg2Pa) / 0x10u]);
        case kBg2Pa + 2u:
        case 0x04000032u: return static_cast<u16>(bg_pb_[(half_address - kBg2Pa - 2u) / 0x10u]);
        case kBg2Pa + 4u:
        case 0x04000034u: return static_cast<u16>(bg_pc_[(half_address - kBg2Pa - 4u) / 0x10u]);
        case kBg2Pa + 6u:
        case 0x04000036u: return static_cast<u16>(bg_pd_[(half_address - kBg2Pa - 6u) / 0x10u]);
        case kBg2X:
        case 0x04000038u: {
            const auto idx = static_cast<std::size_t>((half_address - kBg2X) / 0x10u);
            return static_cast<u16>(static_cast<u32>(bg_ref_x_[idx]) & 0xFFFFu);
        }
        case kBg2X + 2u:
        case 0x0400003Au: {
            const auto idx = static_cast<std::size_t>((half_address - kBg2X - 2u) / 0x10u);
            return static_cast<u16>((static_cast<u32>(bg_ref_x_[idx]) >> 16u) & 0xFFFFu);
        }
        case kBg2X + 4u:
        case 0x0400003Cu: {
            const auto idx = static_cast<std::size_t>((half_address - kBg2X - 4u) / 0x10u);
            return static_cast<u16>(static_cast<u32>(bg_ref_y_[idx]) & 0xFFFFu);
        }
        case kBg2X + 6u:
        case 0x0400003Eu: {
            const auto idx = static_cast<std::size_t>((half_address - kBg2X - 6u) / 0x10u);
            return static_cast<u16>((static_cast<u32>(bg_ref_y_[idx]) >> 16u) & 0xFFFFu);
        }
        case kMosaic: return mosaic_;
        case kBldCnt: return static_cast<u16>(bldcnt_ & 0x3FFFu);
        case kBldCnt + 2u: return static_cast<u16>(bldalpha_ & 0x1F1Fu);
        case kBldCnt + 4u: return bldy_;
        default: return 0;
        }
    };

    if (width == BusWidth::Byte) {
        const auto aligned = align_down(address, 2u);
        const auto shift = (address & 1u) * 8u;
        return static_cast<u32>((read_half(aligned) >> shift) & 0xFFu);
    }
    if (width == BusWidth::Half) {
        return read_half(address);
    }
    return static_cast<u32>(read_half(address)) | (static_cast<u32>(read_half(address + 2u)) << 16u);
}

void Ppu::write_register(u32 address, u32 value, BusWidth width) {
    auto write_half = [&](u32 half_address, u16 half_value) -> bool {
        switch (half_address) {
        case kDispcnt: dispcnt_ = half_value; return true;
        case kDispcnt + 2u: greenswp_ = half_value; return true;
        case kDispstat: dispstat_ = static_cast<u16>((dispstat_ & 0x0007u) | (half_value & 0xFFF8u)); return false;
        case kBg0Cnt:
        case kBg0Cnt + 2u:
        case kBg0Cnt + 4u:
        case kBg0Cnt + 6u: bgcnt_[(half_address - kBg0Cnt) / 2u] = half_value; return true;
        case 0x04000010u:
        case 0x04000014u:
        case 0x04000018u:
        case 0x0400001Cu: bghofs_[(half_address - 0x04000010u) / 4u] = half_value; return true;
        case 0x04000012u:
        case 0x04000016u:
        case 0x0400001Au:
        case 0x0400001Eu: bgvofs_[(half_address - 0x04000012u) / 4u] = half_value; return true;
        case kWin0H:
        case kWin0H + 2u: winh_[(half_address - kWin0H) / 2u] = half_value; return true;
        case 0x04000044u:
        case 0x04000046u: winv_[(half_address - 0x04000044u) / 2u] = half_value; return true;
        case 0x04000048u: winin_ = half_value; return true;
        case 0x0400004Au: winout_ = half_value; return true;
        case kMosaic: mosaic_ = half_value; return true;
        case kBldCnt: bldcnt_ = half_value; return true;
        case kBldCnt + 2u: bldalpha_ = half_value; return true;
        case kBldCnt + 4u: bldy_ = half_value; return true;
        case kBg2Pa:
        case 0x04000030u: bg_pa_[(half_address - kBg2Pa) / 0x10u] = static_cast<s16>(half_value); return true;
        case kBg2Pa + 2u:
        case 0x04000032u: bg_pb_[(half_address - kBg2Pa - 2u) / 0x10u] = static_cast<s16>(half_value); return true;
        case kBg2Pa + 4u:
        case 0x04000034u: bg_pc_[(half_address - kBg2Pa - 4u) / 0x10u] = static_cast<s16>(half_value); return true;
        case kBg2Pa + 6u:
        case 0x04000036u: bg_pd_[(half_address - kBg2Pa - 6u) / 0x10u] = static_cast<s16>(half_value); return true;
        case kBg2X:
        case 0x04000038u: {
            const auto idx = static_cast<std::size_t>((half_address - kBg2X) / 0x10u);
            const auto raw = static_cast<u32>(bg_ref_x_[idx]);
            bg_ref_x_[idx] = static_cast<s32>((raw & 0xFFFF0000u) | half_value);
            return true;
        }
        case kBg2X + 2u:
        case 0x0400003Au: {
            const auto idx = static_cast<std::size_t>((half_address - kBg2X - 2u) / 0x10u);
            const auto raw = static_cast<u32>(bg_ref_x_[idx]);
            bg_ref_x_[idx] = static_cast<s32>((static_cast<u32>(static_cast<s16>(half_value)) << 16u) | (raw & 0x0000FFFFu));
            return true;
        }
        case kBg2X + 4u:
        case 0x0400003Cu: {
            const auto idx = static_cast<std::size_t>((half_address - kBg2X - 4u) / 0x10u);
            const auto raw = static_cast<u32>(bg_ref_y_[idx]);
            bg_ref_y_[idx] = static_cast<s32>((raw & 0xFFFF0000u) | half_value);
            return true;
        }
        case kBg2X + 6u:
        case 0x0400003Eu: {
            const auto idx = static_cast<std::size_t>((half_address - kBg2X - 6u) / 0x10u);
            const auto raw = static_cast<u32>(bg_ref_y_[idx]);
            bg_ref_y_[idx] = static_cast<s32>((static_cast<u32>(static_cast<s16>(half_value)) << 16u) | (raw & 0x0000FFFFu));
            return true;
        }
        default: return false;
        }
    };

    bool render_dirty = false;
    if (width == BusWidth::Byte) {
        const auto aligned = align_down(address, 2u);
        const auto existing = static_cast<u16>(read_register(aligned, BusWidth::Half));
        const auto shift = (address & 1u) * 8u;
        const auto merged = static_cast<u16>((existing & ~(0xFFu << shift)) | ((value & 0xFFu) << shift));
        render_dirty |= write_half(aligned, merged);
    } else if (width == BusWidth::Half) {
        render_dirty |= write_half(address, static_cast<u16>(value));
    } else {
        render_dirty |= write_half(address, static_cast<u16>(value & 0xFFFFu));
        render_dirty |= write_half(address + 2u, static_cast<u16>((value >> 16u) & 0xFFFFu));
    }

    update_dispstat_flags();
    if (render_dirty) {
        mark_all_scanlines_dirty();
        invalidate_sprite_cache();
    }
}

void IRAM_ATTR Ppu::advance_to(u64 cycle_now, IrqController& irq) {
    while (next_event_cycle_ <= cycle_now) {
        if (!hblank_) {
            enter_hblank(irq, next_event_cycle_);
            next_event_cycle_ += kHBlankCycles;
        } else {
            leave_hblank(irq, next_event_cycle_);
            next_event_cycle_ += kHDrawCycles;
        }
    }
}

IRAM_ATTR void Ppu::render_scanline(int line, std::span<const u8> vram, std::span<const u8> palette, std::span<const u8> oam, u16* out_row) {
    if (!framebuffer_) return;

    constexpr u32 kCropY = (kScreenHeight - kOutH) / 2u;
    constexpr u32 kCropX = (kScreenWidth - kOutW) / 2u;

    if (direct_128x128_) {
        if (line < static_cast<int>(kCropY) || line >= static_cast<int>(kCropY + kOutH)) return;
    } else {
        if (line < 0 || line >= static_cast<int>(kScreenHeight)) return;
    }

    if (dirty_count_ == 0 || !scanline_dirty_[static_cast<std::size_t>(line)]) return;
    clear_dirty(line);

    u16* fb_base = external_fb_ ? external_fb_ : framebuffer_.get();
    u16* final_row = nullptr;
    
    const u32 x_start = direct_128x128_ ? kCropX : 0;
    const u32 out_width = direct_128x128_ ? kOutW : kScreenWidth;

    if (direct_128x128_) {
        const auto out_y = static_cast<u32>(line) - kCropY;
        final_row = out_row ? out_row : (fb_base + (static_cast<std::size_t>(out_y) * kOutW));
    } else {
        final_row = out_row ? out_row : (fb_base + (static_cast<std::size_t>(line) * kScreenWidth));
    }

    if (force_blank()) { 
        std::fill_n(final_row, out_width, static_cast<u16>(0x7FFF)); 
        return; 
    }

    // Line buffers for compositing (persistent workspace, not per-call stack locals).
    auto& bg_line = bg_line_buf_;
    auto& obj_line = obj_line_buf_;
    auto& obj_priority = obj_priority_buf_;
    auto& obj_trans = obj_trans_buf_;
    auto& obj_win = obj_win_buf_;

    for (int i = 0; i < 4; ++i) {
        std::fill_n(&bg_line[static_cast<std::size_t>(i)][x_start], out_width, static_cast<u16>(0x8000));
    }
    std::fill_n(&obj_line[x_start], out_width, static_cast<u16>(0x8000));
    std::fill_n(&obj_priority[x_start], out_width, static_cast<u8>(4));
    std::fill_n(&obj_trans[x_start], out_width, false);
    std::fill_n(&obj_win[x_start], out_width, false);

    const auto mode = dispcnt_ & 0x0007u;
    const bool obj_enabled = test_bit(dispcnt_, 12u);

    // Render BGs to Line Buffers
    if (mode < 3) {
        if (mode == 0) {
            for (int bg = 3; bg >= 0; --bg) {
                if (test_bit(dispcnt_, 8u + static_cast<u32>(bg))) {
                    render_text_bg(line, vram, palette, bg, bg_line[static_cast<std::size_t>(bg)].data(), x_start, out_width);
                }
            }
        } else if (mode == 1) {
            if (test_bit(dispcnt_, 8u + 2u)) {
                render_affine_bg(line, vram, palette, 2, bg_line[2].data(), x_start, out_width);
            }
            if (test_bit(dispcnt_, 8u + 1u)) {
                render_text_bg(line, vram, palette, 1, bg_line[1].data(), x_start, out_width);
            }
            if (test_bit(dispcnt_, 8u + 0u)) {
                render_text_bg(line, vram, palette, 0, bg_line[0].data(), x_start, out_width);
            }
        } else if (mode == 2) {
            if (test_bit(dispcnt_, 8u + 3u)) {
                render_affine_bg(line, vram, palette, 3, bg_line[3].data(), x_start, out_width);
            }
            if (test_bit(dispcnt_, 8u + 2u)) {
                render_affine_bg(line, vram, palette, 2, bg_line[2].data(), x_start, out_width);
            }
        }
    } else {
        // Bitmap Modes (render directly to bg_line[2] for compositing)
        if (mode == 3) {
            const auto base = static_cast<u32>(line) * kScreenWidth * 2u;
            for (u32 x = x_start; x < x_start + out_width; ++x) bg_line[2][x] = read16(vram, base + (x * 2u));
        } else if (mode == 4) {
            const auto page = test_bit(dispcnt_, 4) ? 0xA000u : 0u;
            const auto base = page + (static_cast<u32>(line) * kScreenWidth);
            for (u32 x = x_start; x < x_start + out_width; ++x) {
                const auto index = vram[(base + x) % vram.size()];
                if (index != 0) bg_line[2][x] = read16(palette, static_cast<u32>(index) * 2u);
            }
        } else if (mode == 5) {
            constexpr u32 kM5W = 160u;
            constexpr u32 kM5H = 128u;
            const auto page = test_bit(dispcnt_, 4) ? 0xA000u : 0u;

            if (direct_128x128_) {
                if (line >= static_cast<int>(kCropY) && line < static_cast<int>(kCropY + kOutH)) {
                    constexpr u32 kM5CX = (kM5W - kOutW) / 2u;
                    const auto src_y = static_cast<u32>(line) - kCropY;
                    const auto base = page + (src_y * kM5W * 2u);
                    for (u32 out_x = 0; out_x < out_width; ++out_x) {
                        bg_line[2][x_start + out_x] = read16(vram, base + ((out_x + kM5CX) * 2u));
                    }
                }
            } else if (line >= 0 && static_cast<u32>(line) < kM5H) {
                const auto base = page + (static_cast<u32>(line) * kM5W * 2u);
                const auto visible = std::min(out_width, kM5W);
                for (u32 x = 0; x < visible; ++x) {
                    bg_line[2][x] = read16(vram, base + (x * 2u));
                }
            }
        }
    }

    // Render OBJs to Line Buffers
    if (obj_enabled) {
        render_objects(line, vram, palette, oam, obj_line.data(), obj_priority.data(), obj_trans.data(),
                       obj_win.data(), x_start, out_width);
    }

    // Layer Definitions for Sorting
    struct Layer {
        u8 id; // 0-3: BG0-BG3, 4: OBJ, 5: Backdrop
        u8 priority;
        u16 color;
    };
    
    const u16 backdrop_color = read16(palette, 0);

    // Pre-calculate Window boundaries
    const bool win0_en = test_bit(dispcnt_, 13u);
    const bool win1_en = test_bit(dispcnt_, 14u);
    const bool obj_win_en = test_bit(dispcnt_, 15u);
    const bool windows_enabled = win0_en || win1_en || obj_win_en;
    
    const u8 win0_r = winh_[0] & 0xFF; const u8 win0_l = winh_[0] >> 8;
    const u8 win0_b = winv_[0] & 0xFF; const u8 win0_t = winv_[0] >> 8;
    const u8 win1_r = winh_[1] & 0xFF; const u8 win1_l = winh_[1] >> 8;
    const u8 win1_b = winv_[1] & 0xFF; const u8 win1_t = winv_[1] >> 8;
    
    // Y checks for Windows (handled simply here, assuming hardware wrap-around is rare but properly done, l > r means wrap)
    auto in_window_y = [line](u8 t, u8 b) {
        if (t <= b) return line >= t && line < b;
        return line >= t || line < b;
    };
    const bool in_win0_y = in_window_y(win0_t, win0_b);
    const bool in_win1_y = in_window_y(win1_t, win1_b);

    const u8 effect_mode = (bldcnt_ >> 6) & 3;
    const u8 target1_mask = bldcnt_ & 0x3F;
    const u8 target2_mask = (bldcnt_ >> 8) & 0x3F;
    const int eva = std::min(16, bldalpha_ & 0x1F);
    const int evb = std::min(16, (bldalpha_ >> 8) & 0x1F);
    const int evy = std::min(16, bldy_ & 0x1F);

    // Compositing Pass
    for (u32 x = x_start; x < x_start + out_width; ++x) {
        u8 active_mask = 0xFF; // Default all enabled

        if (windows_enabled) {
            bool in_win0 = false, in_win1 = false;
            
            if (win0_en && in_win0_y) {
                if (win0_l <= win0_r) in_win0 = (x >= win0_l && x < win0_r);
                else in_win0 = (x >= win0_l || x < win0_r);
            }
            if (win1_en && in_win1_y) {
                if (win1_l <= win1_r) in_win1 = (x >= win1_l && x < win1_r);
                else in_win1 = (x >= win1_l || x < win1_r);
            }

            if (in_win0) active_mask = winin_ & 0xFF;
            else if (in_win1) active_mask = winin_ >> 8;
            else if (obj_win_en && obj_win[x]) active_mask = winout_ >> 8;
            else active_mask = winout_ & 0xFF;
        }

        // Collect visible pixels
        Layer layers[6];
        int num_layers = 0;

        // Backdrop is always present, priority 4
        layers[num_layers++] = {5, 4, backdrop_color};

        if (obj_enabled && (active_mask & 0x10) && obj_line[x] != 0x8000) {
            layers[num_layers++] = {4, obj_priority[x], obj_line[x]};
        }
        
        for (u8 bg = 0; bg < 4; ++bg) {
            bool is_enabled = test_bit(dispcnt_, 8u + bg);
            if (mode >= 3 && bg == 2) is_enabled = true;
            if (is_enabled && (active_mask & (1 << bg)) && bg_line[bg][x] != 0x8000) {
                const u32 priority = bgcnt_[bg] & 3u;
                layers[num_layers++] = {bg, static_cast<u8>(priority), bg_line[bg][x]};
            }
        }
        
        static constexpr std::array<u8, 6> kLayerTiebreaker{{1, 2, 3, 4, 0, 5}};

        Layer top = layers[0];
        Layer second = {5, 4, backdrop_color};
        
        for (int i = 1; i < num_layers; ++i) {
            bool is_better = false;
            if (layers[i].priority < top.priority) is_better = true;
            else if (layers[i].priority == top.priority &&
                     kLayerTiebreaker[layers[i].id] < kLayerTiebreaker[top.id]) is_better = true;
            
            if (is_better) {
                second = top;
                top = layers[i];
            } else {
                bool is_better_than_second = false;
                if (layers[i].priority < second.priority) is_better_than_second = true;
                else if (layers[i].priority == second.priority &&
                         kLayerTiebreaker[layers[i].id] < kLayerTiebreaker[second.id]) is_better_than_second = true;
                
                if (is_better_than_second) {
                    second = layers[i];
                }
            }
        }

        u16 final_color = top.color;
        
        // Color Special Effects
        const bool effect_enabled = (active_mask & 0x20) != 0;
        const bool obj_translucent_pixel = (top.id == 4 && obj_trans[x]);
        
        // Translucent OBJs FORCE alpha blending with Target 2, ignoring Target 1 and effect_enabled
        if (obj_translucent_pixel && (target2_mask & (1 << second.id))) {
            const int r1 = final_color & 0x1F;
            const int g1 = (final_color >> 5) & 0x1F;
            const int b1 = (final_color >> 10) & 0x1F;
            const int r2 = second.color & 0x1F;
            const int g2 = (second.color >> 5) & 0x1F;
            const int b2 = (second.color >> 10) & 0x1F;
            
            const int r = std::min(31, (r1 * eva + r2 * evb) >> 4);
            const int g = std::min(31, (g1 * eva + g2 * evb) >> 4);
            const int b = std::min(31, (b1 * eva + b2 * evb) >> 4);
            final_color = static_cast<u16>(r | (g << 5) | (b << 10));
        } else if (effect_enabled && (target1_mask & (1 << top.id))) {
            const int r1 = final_color & 0x1F;
            const int g1 = (final_color >> 5) & 0x1F;
            const int b1 = (final_color >> 10) & 0x1F;
            
            if (effect_mode == 1 && (target2_mask & (1 << second.id))) { // Alpha
                const int r2 = second.color & 0x1F;
                const int g2 = (second.color >> 5) & 0x1F;
                const int b2 = (second.color >> 10) & 0x1F;
                
                const int r = std::min(31, (r1 * eva + r2 * evb) >> 4);
                const int g = std::min(31, (g1 * eva + g2 * evb) >> 4);
                const int b = std::min(31, (b1 * eva + b2 * evb) >> 4);
                final_color = static_cast<u16>(r | (g << 5) | (b << 10));
            } else if (effect_mode == 2) { // Brightness Increase
                const int r = r1 + (((31 - r1) * evy) >> 4);
                const int g = g1 + (((31 - g1) * evy) >> 4);
                const int b = b1 + (((31 - b1) * evy) >> 4);
                final_color = static_cast<u16>(r | (g << 5) | (b << 10));
            } else if (effect_mode == 3) { // Brightness Decrease
                const int r = r1 - ((r1 * evy) >> 4);
                const int g = g1 - ((g1 * evy) >> 4);
                const int b = b1 - ((b1 * evy) >> 4);
                final_color = static_cast<u16>(r | (g << 5) | (b << 10));
            }
        }
        
        final_row[x - x_start] = final_color;
    }
}

IRAM_ATTR void Ppu::render_text_bg(int line, std::span<const u8> vram, std::span<const u8> palette,
                         int bg, u16* row, u32 x_start, u32 out_width) {
    const auto bg_index = static_cast<std::size_t>(bg);
    const BgCnt cnt{bgcnt_[bg_index]};
    const auto hofs = bghofs_[bg_index] & 0x1FFu;
    const auto vofs = bgvofs_[bg_index] & 0x1FFu;

    const u32 map_width = (cnt.size() & 1u) ? 64u : 32u;
    const u32 map_height = (cnt.size() & 2u) ? 64u : 32u;

    const u32 effective_y = (static_cast<u32>(line) + vofs) & (map_height * 8u - 1u);
    const u32 tile_row = effective_y / 8u;
    const u32 fine_y = effective_y & 7u;

    const auto char_base = cnt.char_base_block();
    const auto screen_base = cnt.screen_base_block();

    const u32 end_x = x_start + out_width;
    u32 last_tc = std::numeric_limits<u32>::max();
    u16 entry = 0;
    u32 tile_num = 0;
    bool h_flip = false;
    bool v_flip = false;
    u32 pal_bank = 0;
    const u8* cached_pixels = nullptr;

    for (u32 screen_x = x_start; screen_x < end_x; ++screen_x) {
        const u32 pixel_x = (screen_x + hofs) & (map_width * 8u - 1u);
        const u32 tc = pixel_x / 8u;
        const u32 fine_x = pixel_x & 7u;

        if (tc != last_tc) {
            last_tc = tc;
            const u32 screen_col = tc & 0x1Fu;
            const u32 map_block_offset = ((tc >= 32u) ? 1u : 0u) + ((tile_row >= 32u) ? 2u : 0u);
            const u32 map_row = tile_row & 0x1Fu;
            const u32 entry_addr = screen_base + (map_block_offset * 0x800u) + (map_row * 64u) + (screen_col * 2u);

            entry = read16(vram, entry_addr);
            tile_num = entry & 0x3FFu;
            h_flip = (entry >> 10u & 1u) != 0;
            v_flip = (entry >> 11u & 1u) != 0;
            pal_bank = (entry >> 12u & 0xFu) << 4u;

            const u32 y_off = v_flip ? (7u - fine_y) : fine_y;

            if (cnt.color_mode()) {
                const u32 base_data_addr = char_base + tile_num * 0x40u + y_off * 8u;
                const u32 cache_key = (tile_cache_epoch_ << 20) ^ base_data_addr;
                const u32 cache_idx = (base_data_addr >> 3) % kTileCacheEntries;
                auto& cache_entry = tile_cache_8bpp_[cache_idx];

                if (cache_entry.key != cache_key) {
                    cache_entry.key = cache_key;
                    for (u32 p = 0; p < 8u; ++p) {
                        cache_entry.pixels[p] = vram[(base_data_addr + p) % vram.size()];
                    }
                }
                cached_pixels = cache_entry.pixels;
            } else {
                const u32 base_byte_addr = char_base + tile_num * 0x20u + y_off * 4u;
                const u32 cache_key = (tile_cache_epoch_ << 20) ^ base_byte_addr;
                const u32 cache_idx = (base_byte_addr >> 2) % kTileCacheEntries;
                auto& cache_entry = tile_cache_4bpp_[cache_idx];

                if (cache_entry.key != cache_key) {
                    cache_entry.key = cache_key;
                    for (u32 p = 0; p < 8u; ++p) {
                        const u32 byte_addr = base_byte_addr + (p / 2u);
                        const u8 byte_val = vram[byte_addr % vram.size()];
                        cache_entry.pixels[p] = (p & 1u) ? (byte_val >> 4u) : (byte_val & 0xFu);
                    }
                }
                cached_pixels = cache_entry.pixels;
            }
        }

        const u32 real_x = h_flip ? (7u - fine_x) : fine_x;

        const u8 color_idx = cached_pixels[real_x];
        if (color_idx != 0) {
            if (cnt.color_mode()) {
                row[screen_x] = read16(palette, static_cast<u32>(color_idx) * 2u);
            } else {
                row[screen_x] = read16(palette, (pal_bank + color_idx) * 2u);
            }
        }
    }
}

IRAM_ATTR void Ppu::render_affine_bg(int line, std::span<const u8> vram, std::span<const u8> palette,
                           int bg, u16* row, u32 x_start, u32 out_width) {
    const auto bg_index = static_cast<std::size_t>(bg);
    const auto affine_index = static_cast<std::size_t>(bg - 2);
    const BgCnt cnt{bgcnt_[bg_index]};
    const auto screen_base = cnt.screen_base_block();
    const auto char_base = cnt.char_base_block();

    static const u32 size_pixels[] = {128, 256, 512, 1024};
    const u32 bg_size = size_pixels[cnt.size()];

    const s32 ref_x = bg_ref_x_[affine_index] + line * bg_pb_[affine_index];
    const s32 ref_y = bg_ref_y_[affine_index] + line * bg_pd_[affine_index];
    const s16 pa = bg_pa_[affine_index];
    const s16 pc = bg_pc_[affine_index];

    for (u32 out_x = 0; out_x < out_width; out_x++) {
        const u32 screen_x = out_x + x_start;
        const s32 x = (ref_x + static_cast<s32>(screen_x) * pa) >> 8;
        const s32 y = (ref_y + static_cast<s32>(screen_x) * pc) >> 8;

        if (cnt.wraparound()) {
            const u32 wrapped_x = static_cast<u32>(x) % bg_size;
            const u32 wrapped_y = static_cast<u32>(y) % bg_size;
            const u32 tile_x = wrapped_x / 8u;
            const u32 tile_y = wrapped_y / 8u;
            const u32 map_size = bg_size / 8u;
            const u32 entry_addr = screen_base + (tile_y * map_size) + tile_x;
            const u8 tile_num = vram[entry_addr % vram.size()];
            const u32 data_addr = char_base + tile_num * 0x40u + (wrapped_y & 7u) * 8u + (wrapped_x & 7u);
            const u8 color_idx = vram[data_addr % vram.size()];
            if (color_idx != 0) {
                row[screen_x] = read16(palette, static_cast<u32>(color_idx) * 2u);
            }
        } else if (x >= 0 && x < static_cast<s32>(bg_size) && y >= 0 && y < static_cast<s32>(bg_size)) {
            const u32 tile_x = static_cast<u32>(x) / 8u;
            const u32 tile_y = static_cast<u32>(y) / 8u;
            const u32 map_size = bg_size / 8u;
            const u32 entry_addr = screen_base + (tile_y * map_size) + tile_x;
            const u8 tile_num = vram[entry_addr % vram.size()];
            const u32 data_addr = char_base + (tile_num * 0x40u) + (static_cast<u32>(y) & 7u) * 8u + (static_cast<u32>(x) & 7u);
            const u8 color_idx = vram[data_addr % vram.size()];
            if (color_idx != 0) {
                row[screen_x] = read16(palette, static_cast<u32>(color_idx) * 2u);
            }
        }
    }
}

u64 Ppu::next_event_cycle() const { return next_event_cycle_; }
bool Ppu::is_hblank() const { return hblank_; }
bool Ppu::is_vblank() const { return vblank_; }
bool Ppu::is_video_memory_contended() const { return !force_blank() && !vblank_ && !hblank_; }
bool Ppu::frame_ready() const { return frame_ready_; }
std::optional<int> Ppu::consume_scanline_ready() {
    const auto ready = scanline_ready_;
    scanline_ready_.reset();
    return ready;
}
bool Ppu::consume_frame_ready() {
    const auto ready = frame_ready_;
    frame_ready_ = false;
    return ready;
}
std::span<const u16> Ppu::framebuffer() const {
    return {framebuffer_.get(), direct_128x128_ ? kOutputPixels : kFramebufferPixels};
}
u16 Ppu::dispcnt() const { return dispcnt_; }
u16 Ppu::dispstat() const { return dispstat_; }
u16 Ppu::vcount() const { return vcount_; }
bool Ppu::force_blank() const { return test_bit(dispcnt_, 7); }
bool Ppu::hblank_free() const { return test_bit(dispcnt_, 5); }

void Ppu::update_dispstat_flags() {
    dispstat_ = static_cast<u16>(dispstat_ & ~0x0007u);
    if (vblank_) dispstat_ |= 0x0001u;
    if (hblank_) dispstat_ |= 0x0002u;
    if (((dispstat_ >> 8u) & 0x00FFu) == vcount_) dispstat_ |= 0x0004u;
}

void Ppu::enter_hblank(IrqController& irq, u64 cycle_now) {
    hblank_ = true;
    update_dispstat_flags();
    if (!vblank_) scanline_ready_ = static_cast<int>(vcount_);
    if (test_bit(dispstat_, 4)) irq.raise_delayed(IrqHBlank, cycle_now, 1);
}

void Ppu::leave_hblank(IrqController& irq, u64 cycle_now) {
    hblank_ = false;
    vcount_ = static_cast<u16>((vcount_ + 1u) % kScanlinesPerFrame);
    if (vcount_ == kVisibleScanlines) enter_vblank(irq, cycle_now);
    else if (vcount_ == 0) leave_vblank();
    update_dispstat_flags();
    handle_vcount_compare(irq, cycle_now);
}

void Ppu::enter_vblank(IrqController& irq, u64 cycle_now) {
    vblank_ = true;
    frame_ready_ = true;
    update_dispstat_flags();
    if (test_bit(dispstat_, 3)) irq.raise_delayed(IrqVBlank, cycle_now, 1);
}

void Ppu::leave_vblank() {
    vblank_ = false;
    update_dispstat_flags();
}

void Ppu::handle_vcount_compare(IrqController& irq, u64 cycle_now) {
    if (((dispstat_ >> 8u) & 0x00FFu) == vcount_) {
        dispstat_ |= 0x0004u;
        if (test_bit(dispstat_, 5)) irq.raise_delayed(IrqVCount, cycle_now, 1);
    } else {
        dispstat_ = static_cast<u16>(dispstat_ & ~0x0004u);
    }
}

void Ppu::mark_all_dirty() {
    mark_all_scanlines_dirty();
    invalidate_sprite_cache();
    ++tile_cache_epoch_;
}

void Ppu::mark_all_scanlines_dirty() {
    scanline_dirty_.fill(true);
    dirty_count_ = kScreenHeight;
    all_dirty_ = true;
}

void Ppu::mark_dirty(int line) {
    if (line < 0 || line >= static_cast<int>(kScreenHeight)) {
        return;
    }
    auto& dirty = scanline_dirty_[static_cast<std::size_t>(line)];
    if (!dirty) {
        dirty = true;
        ++dirty_count_;
    }
    all_dirty_ = dirty_count_ == kScreenHeight;
}

bool Ppu::is_dirty(int line) const {
    if (line < 0 || line >= static_cast<int>(kScreenHeight)) {
        return false;
    }
    return scanline_dirty_[static_cast<std::size_t>(line)];
}

void Ppu::clear_dirty(int line) {
    if (line < 0 || line >= static_cast<int>(kScreenHeight)) {
        return;
    }
    auto& dirty = scanline_dirty_[static_cast<std::size_t>(line)];
    if (dirty) {
        dirty = false;
        if (dirty_count_ != 0) {
            --dirty_count_;
        }
    }
    all_dirty_ = dirty_count_ == kScreenHeight;
}

void Ppu::invalidate_sprite_cache() {
    sprite_cache_valid_ = false;
}

void Ppu::rebuild_sprite_cache(std::span<const u8> oam) {
    sprite_scanline_count_.fill(0);
    if (oam.empty()) {
        sprite_cache_valid_ = true;
        return;
    }

    for (int i = 127; i >= 0; --i) {
        const u32 obj_addr = static_cast<u32>(i) * 8u;
        const u16 attr0 = read16(oam, obj_addr);
        const u16 attr1 = read16(oam, obj_addr + 2u);

        const u32 obj_mode = (attr0 >> 8u) & 3u;
        if (obj_mode == 2u) {
            continue;
        }

        const u32 gfx_mode = (attr0 >> 10u) & 3u;
        if (gfx_mode == 3u) {
            continue;
        }

        const u32 shape = (attr0 >> 14u) & 3u;
        if (shape == 3u) {
            continue;
        }

        const u32 size = (attr1 >> 14u) & 3u;
        const bool double_size = obj_mode == 3u;
        const int h = kSpriteHeights[shape][size];
        const int bounding_h = double_size ? h * 2 : h;

        int y = attr0 & 0xFF;
        if (y >= 128) {
            y -= 256;
        }

        const auto first = std::max(0, y);
        const auto last = std::min(static_cast<int>(kScreenHeight), y + bounding_h);
        for (int line = first; line < last; ++line) {
            auto& count = sprite_scanline_count_[static_cast<std::size_t>(line)];
            sprite_scanline_cache_[static_cast<std::size_t>(line)][count++] = static_cast<u8>(i);
        }
    }

    sprite_cache_valid_ = true;
}

IRAM_ATTR void Ppu::render_objects(int line, std::span<const u8> vram, std::span<const u8> palette,
                         std::span<const u8> oam, u16* obj_line, u8* obj_priority,
                         bool* obj_trans, bool* obj_win, u32 x_start, u32 out_width) {
    if (oam.empty() || vram.empty() || palette.empty()) return;

    const bool obj_1d = test_bit(dispcnt_, 6u);
    const u32 obj_vram_base = 0x10000;

    if (!sprite_cache_valid_) {
        rebuild_sprite_cache(oam);
    }
    const auto cache_line = static_cast<std::size_t>(line);
    const auto active_count = sprite_scanline_count_[cache_line];

    for (u32 active = 0; active < active_count; ++active) {
        const int i = sprite_scanline_cache_[cache_line][active];
        const u32 obj_addr = static_cast<u32>(i) * 8u;
        const u16 attr0 = read16(oam, obj_addr);
        const u16 attr1 = read16(oam, obj_addr + 2u);
        const u16 attr2 = read16(oam, obj_addr + 4u);

        const u32 obj_mode = (attr0 >> 8u) & 3u;
        if (obj_mode == 2) continue; // Disabled

        const u32 gfx_mode = (attr0 >> 10u) & 3u;
        if (gfx_mode == 3) continue; // Prohibited

        int y = attr0 & 0xFF;
        if (y >= 128) y -= 256;

        const u32 shape = (attr0 >> 14u) & 3u;
        if (shape == 3) continue; // Prohibited
        const u32 size = (attr1 >> 14u) & 3u;

        const int w = kSpriteWidths[shape][size];
        const int h = kSpriteHeights[shape][size];

        const bool affine = (obj_mode == 1 || obj_mode == 3);
        const bool double_size = (obj_mode == 3);

        const int bounding_w = double_size ? (w * 2) : w;
        const int bounding_h = double_size ? (h * 2) : h;

        if (line < y || line >= y + bounding_h) continue;

        int x = attr1 & 0x1FF;
        if (x >= 256) x -= 512;

        if (x + bounding_w <= static_cast<int>(x_start) || x >= static_cast<int>(x_start + out_width)) continue;

        const bool color_8bpp = (attr0 >> 13u) & 1u;
        const u32 base_tile = attr2 & 0x3FFu;
        const u32 pal_bank = color_8bpp ? 0 : ((attr2 >> 12u) & 0xFu);
        const u8 priority = static_cast<u8>((attr2 >> 10u) & 3u);

        s16 pa = 0x100, pb = 0, pc = 0, pd = 0x100;
        if (affine) {
            const u32 param_idx = (attr1 >> 9u) & 0x1Fu;
            pa = static_cast<s16>(read16(oam, param_idx * 32u + 6u));
            pb = static_cast<s16>(read16(oam, param_idx * 32u + 14u));
            pc = static_cast<s16>(read16(oam, param_idx * 32u + 22u));
            pd = static_cast<s16>(read16(oam, param_idx * 32u + 30u));
        }

        const bool h_flip = !affine && ((attr1 >> 12u) & 1u);
        const bool v_flip = !affine && ((attr1 >> 13u) & 1u);

        const int local_y = line - y;
        const int start_x = std::max(x, static_cast<int>(x_start));
        const int end_x = std::min(x + bounding_w, static_cast<int>(x_start + out_width));

        const int half_w = bounding_w / 2;
        const int half_h = bounding_h / 2;
        const int origin_x = w / 2;
        const int origin_y = h / 2;

        for (int screen_x = start_x; screen_x < end_x; ++screen_x) {
            const int local_x = screen_x - x;
            int tex_x, tex_y;

            if (affine) {
                const int d_x = local_x - half_w;
                const int d_y = local_y - half_h;
                tex_x = origin_x + ((pa * d_x + pb * d_y) >> 8);
                tex_y = origin_y + ((pc * d_x + pd * d_y) >> 8);
                if (tex_x < 0 || tex_x >= w || tex_y < 0 || tex_y >= h) continue;
            } else {
                tex_x = h_flip ? (w - 1 - local_x) : local_x;
                tex_y = v_flip ? (h - 1 - local_y) : local_y;
            }

            const u32 tile_x = static_cast<u32>(tex_x) / 8u;
            const u32 tile_y = static_cast<u32>(tex_y) / 8u;
            const u32 pixel_x = static_cast<u32>(tex_x) % 8u;
            const u32 pixel_y = static_cast<u32>(tex_y) % 8u;

            u32 tile_offset = 0;
            if (obj_1d) {
                const u32 tiles_per_row = static_cast<u32>(w) / 8u;
                const u32 tile_stride = color_8bpp ? 2u : 1u;
                tile_offset = (tile_y * tiles_per_row + tile_x) * tile_stride;
            } else {
                const u32 tile_stride = color_8bpp ? 2u : 1u;
                tile_offset = (tile_y * 32u) + (tile_x * tile_stride);
            }

            const u32 tile_num = (base_tile + tile_offset) & 0x3FFu;
            const u32 out_x = static_cast<u32>(screen_x);

            if (color_8bpp) {
                const u32 data_addr = obj_vram_base + (tile_num * 0x20u) + (pixel_y * 8u) + pixel_x;
                const u8 color_idx = vram[data_addr % vram.size()];
                if (color_idx != 0) {
                    if (gfx_mode == 2) {
                        obj_win[out_x] = true;
                    } else {
                        obj_line[out_x] = read16(palette, 0x200u + static_cast<u32>(color_idx) * 2u);
                        obj_priority[out_x] = priority;
                        obj_trans[out_x] = (gfx_mode == 1);
                    }
                }
            } else {
                const u32 data_addr = obj_vram_base + (tile_num * 0x20u) + (pixel_y * 4u) + (pixel_x / 2u);
                const u8 byte_val = vram[data_addr % vram.size()];
                const u32 color_idx = (pixel_x & 1u) ? (byte_val >> 4u) : (byte_val & 0xFu);
                if (color_idx != 0) {
                    if (gfx_mode == 2) {
                        obj_win[out_x] = true;
                    } else {
                        obj_line[out_x] = read16(palette, 0x200u + (pal_bank * 32u) + color_idx * 2u);
                        obj_priority[out_x] = priority;
                        obj_trans[out_x] = (gfx_mode == 1);
                    }
                }
            }
        }
    }
}

}  // namespace gba
