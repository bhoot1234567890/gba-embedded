#include "gba/core/ppu.hpp"

#include <algorithm>
#include <cstring>

#ifdef GBA_PLATFORM_ESP32
#include "esp_heap_caps.h"
#endif

#include "gba/core/constants.hpp"
#include "gba/core/irq.hpp"

namespace gba {

namespace {

void free_framebuffer(u16* ptr) {
    if (!ptr) {
        return;
    }
#ifdef GBA_PLATFORM_ESP32
    heap_caps_free(ptr);
#else
    delete[] ptr;
#endif
}

u16* alloc_framebuffer() {
#ifdef GBA_PLATFORM_ESP32
    auto* ptr = static_cast<u16*>(heap_caps_malloc(kFramebufferPixels * sizeof(u16), MALLOC_CAP_SPIRAM));
    if (ptr) {
        std::fill_n(ptr, kFramebufferPixels, u16{0x7FFF});
        return ptr;
    }
    ptr = static_cast<u16*>(heap_caps_malloc(kFramebufferPixels * sizeof(u16), MALLOC_CAP_8BIT));
    if (ptr) {
        std::fill_n(ptr, kFramebufferPixels, u16{0x7FFF});
        return ptr;
    }
    return nullptr;
#else
    return new u16[kFramebufferPixels]();
#endif
}

}  // namespace

Ppu::Ppu()
    : framebuffer_(alloc_framebuffer(), free_framebuffer) {}

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
    next_event_cycle_ = 1007;
    hblank_ = false;
    vblank_ = false;
    frame_ready_ = false;
    scanline_ready_.reset();
    mark_all_dirty();
    update_dispstat_flags();
}

u32 Ppu::read_register(u32 address, BusWidth width) const {
    auto read_half = [&](u32 half_address) -> u16 {
        switch (half_address) {
        case kDispcnt:
            return dispcnt_;
        case kDispcnt + 2u:
            return greenswp_;
        case kDispstat:
            return dispstat_;
        case kVcount:
            return vcount_;
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
        case 0x0400001Cu:
            return bghofs_[(half_address - 0x04000010u) / 4u];
        case 0x04000012u:
        case 0x04000016u:
        case 0x0400001Au:
        case 0x0400001Eu:
            return bgvofs_[(half_address - 0x04000012u) / 4u];
        case kWin0H:
        case kWin0H + 2u:
            return winh_[(half_address - kWin0H) / 2u];
        case 0x04000044u:
        case 0x04000046u:
            return winv_[(half_address - 0x04000044u) / 2u];
        case 0x04000048u:
            return static_cast<u16>(winin_ & 0x3F3Fu);
        case 0x0400004Au:
            return static_cast<u16>(winout_ & 0x3F3Fu);
        case kBg2Pa:
        case 0x04000030u:
            return static_cast<u16>(bg_pa_[(half_address - kBg2Pa) / 0x10u]);
        case kBg2Pa + 2u:
        case 0x04000032u:
            return static_cast<u16>(bg_pb_[(half_address - kBg2Pa - 2u) / 0x10u]);
        case kBg2Pa + 4u:
        case 0x04000034u:
            return static_cast<u16>(bg_pc_[(half_address - kBg2Pa - 4u) / 0x10u]);
        case kBg2Pa + 6u:
        case 0x04000036u:
            return static_cast<u16>(bg_pd_[(half_address - kBg2Pa - 6u) / 0x10u]);
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
        case kMosaic:
            return mosaic_;
        case kBldCnt:
            return static_cast<u16>(bldcnt_ & 0x3FFFu);
        case kBldCnt + 2u:
            return static_cast<u16>(bldalpha_ & 0x1F1Fu);
        case kBldCnt + 4u:
            return bldy_;
        default:
            return 0;
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
    auto write_half = [&](u32 half_address, u16 half_value) {
        switch (half_address) {
        case kDispcnt:
            dispcnt_ = half_value;
            break;
        case kDispcnt + 2u:
            greenswp_ = half_value;
            break;
        case kDispstat:
            dispstat_ = static_cast<u16>((dispstat_ & 0x0007u) | (half_value & 0xFFF8u));
            break;
        case kBg0Cnt:
        case kBg0Cnt + 2u:
        case kBg0Cnt + 4u:
        case kBg0Cnt + 6u:
            bgcnt_[(half_address - kBg0Cnt) / 2u] = half_value;
            break;
        case 0x04000010u:
        case 0x04000014u:
        case 0x04000018u:
        case 0x0400001Cu:
            bghofs_[(half_address - 0x04000010u) / 4u] = half_value;
            break;
        case 0x04000012u:
        case 0x04000016u:
        case 0x0400001Au:
        case 0x0400001Eu:
            bgvofs_[(half_address - 0x04000012u) / 4u] = half_value;
            break;
        case kWin0H:
        case kWin0H + 2u:
            winh_[(half_address - kWin0H) / 2u] = half_value;
            break;
        case 0x04000044u:
        case 0x04000046u:
            winv_[(half_address - 0x04000044u) / 2u] = half_value;
            break;
        case 0x04000048u:
            winin_ = half_value;
            break;
        case 0x0400004Au:
            winout_ = half_value;
            break;
        case kMosaic:
            mosaic_ = half_value;
            break;
        case kBldCnt:
            bldcnt_ = half_value;
            break;
        case kBldCnt + 2u:
            bldalpha_ = half_value;
            break;
        case kBldCnt + 4u:
            bldy_ = half_value;
            break;
        case kBg2Pa:
        case 0x04000030u:
            bg_pa_[(half_address - kBg2Pa) / 0x10u] = static_cast<s16>(half_value);
            break;
        case kBg2Pa + 2u:
        case 0x04000032u:
            bg_pb_[(half_address - kBg2Pa - 2u) / 0x10u] = static_cast<s16>(half_value);
            break;
        case kBg2Pa + 4u:
        case 0x04000034u:
            bg_pc_[(half_address - kBg2Pa - 4u) / 0x10u] = static_cast<s16>(half_value);
            break;
        case kBg2Pa + 6u:
        case 0x04000036u:
            bg_pd_[(half_address - kBg2Pa - 6u) / 0x10u] = static_cast<s16>(half_value);
            break;
        case kBg2X:
        case 0x04000038u: {
            const auto idx = static_cast<std::size_t>((half_address - kBg2X) / 0x10u);
            const auto raw = static_cast<u32>(bg_ref_x_[idx]);
            bg_ref_x_[idx] = static_cast<s32>((raw & 0xFFFF0000u) | half_value);
            break;
        }
        case kBg2X + 2u:
        case 0x0400003Au: {
            const auto idx = static_cast<std::size_t>((half_address - kBg2X - 2u) / 0x10u);
            const auto raw = static_cast<u32>(bg_ref_x_[idx]);
            bg_ref_x_[idx] =
                static_cast<s32>((static_cast<u32>(static_cast<s16>(half_value)) << 16u) | (raw & 0x0000FFFFu));
            break;
        }
        case kBg2X + 4u:
        case 0x0400003Cu: {
            const auto idx = static_cast<std::size_t>((half_address - kBg2X - 4u) / 0x10u);
            const auto raw = static_cast<u32>(bg_ref_y_[idx]);
            bg_ref_y_[idx] = static_cast<s32>((raw & 0xFFFF0000u) | half_value);
            break;
        }
        case kBg2X + 6u:
        case 0x0400003Eu: {
            const auto idx = static_cast<std::size_t>((half_address - kBg2X - 6u) / 0x10u);
            const auto raw = static_cast<u32>(bg_ref_y_[idx]);
            bg_ref_y_[idx] =
                static_cast<s32>((static_cast<u32>(static_cast<s16>(half_value)) << 16u) | (raw & 0x0000FFFFu));
            break;
        }
        default:
            break;
        }
    };

    if (width == BusWidth::Byte) {
        const auto aligned = align_down(address, 2u);
        const auto existing = static_cast<u16>(read_register(aligned, BusWidth::Half));
        const auto shift = (address & 1u) * 8u;
        const auto merged = static_cast<u16>((existing & ~(0xFFu << shift)) | ((value & 0xFFu) << shift));
        write_half(aligned, merged);
    } else if (width == BusWidth::Half) {
        write_half(address, static_cast<u16>(value));
    } else {
        write_half(address, static_cast<u16>(value & 0xFFFFu));
        write_half(address + 2u, static_cast<u16>((value >> 16u) & 0xFFFFu));
    }

    update_dispstat_flags();
    mark_all_dirty();
}

void Ppu::advance_to(u64 cycle_now, IrqController& irq) {
    while (next_event_cycle_ <= cycle_now) {
        if (!hblank_) {
            enter_hblank(irq);
            next_event_cycle_ += 225;
        } else {
            leave_hblank(irq);
            next_event_cycle_ += 1007;
        }
    }
}

void Ppu::render_scanline(int line, std::span<const u8> vram, std::span<const u8> palette) {
    if (line < 0 || line >= static_cast<int>(kScreenHeight) || !framebuffer_) {
        return;
    }

    /* Skip re-rendering if this scanline hasn't changed */
    if (!all_dirty_ && !scanline_dirty_[static_cast<std::size_t>(line)]) {
        return;
    }
    clear_dirty(line);

    const auto backdrop = read16(palette, 0);
    auto* row = framebuffer_.get() + (static_cast<std::size_t>(line) * kScreenWidth);

    if (force_blank()) {
        std::fill_n(row, kScreenWidth, static_cast<u16>(0x7FFF));
        return;
    }

    std::fill_n(row, kScreenWidth, backdrop);

    const auto mode = dispcnt_ & 0x0007u;

    switch (mode) {
    case 0:
        for (int bg = 3; bg >= 0; --bg) {
            if (test_bit(dispcnt_, 8u + static_cast<u32>(bg))) {
                render_text_bg(line, vram, palette, bg, row);
            }
        }
        break;
    case 1:
        if (test_bit(dispcnt_, 8u + 2u)) {
            render_affine_bg(line, vram, palette, 2, row);
        }
        if (test_bit(dispcnt_, 8u + 1u)) {
            render_text_bg(line, vram, palette, 1, row);
        }
        if (test_bit(dispcnt_, 8u + 0u)) {
            render_text_bg(line, vram, palette, 0, row);
        }
        break;
    case 2:
        if (test_bit(dispcnt_, 8u + 3u)) {
            render_affine_bg(line, vram, palette, 3, row);
        }
        if (test_bit(dispcnt_, 8u + 2u)) {
            render_affine_bg(line, vram, palette, 2, row);
        }
        break;
    case 3: {
        const auto base = static_cast<u32>(line) * kScreenWidth * 2u;
        for (u32 x = 0; x < kScreenWidth; ++x) {
            row[x] = read16(vram, base + (x * 2u));
        }
        break;
    }
    case 4: {
        const auto page = test_bit(dispcnt_, 4) ? 0xA000u : 0u;
        const auto base = page + (static_cast<u32>(line) * kScreenWidth);
        for (u32 x = 0; x < kScreenWidth; ++x) {
            const auto index = vram[(base + x) % vram.size()];
            row[x] = read16(palette, static_cast<u32>(index) * 2u);
        }
        break;
    }
    case 5: {
        const auto page = test_bit(dispcnt_, 4) ? 0xA000u : 0u;
        if (line >= 128) {
            break;
        }
        const auto base = page + (static_cast<u32>(line) * 160u * 2u);
        for (u32 x = 0; x < 160u; ++x) {
            row[x] = read16(vram, base + (x * 2u));
        }
        break;
    }
    default:
        break;
    }
}

void Ppu::render_text_bg(int line, std::span<const u8> vram, std::span<const u8> palette,
                         int bg, u16* row) {
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

    for (u32 screen_x = 0; screen_x < kScreenWidth; screen_x += 8u) {
        const u32 pixel_x = (screen_x + hofs) & (map_width * 8u - 1u);
        const u32 tile_col = pixel_x / 8u;
        const u32 fine_x = pixel_x & 7u;

        const u32 screen_col = tile_col & 0x1Fu;
        const u32 map_block_offset = ((tile_col >= 32u) ? 1u : 0u) + ((tile_row >= 32u) ? 2u : 0u);
        const u32 map_row = tile_row & 0x1Fu;
        const u32 entry_addr = screen_base + (map_block_offset * 0x800u) + (map_row * 64u) + (screen_col * 2u);

        const u16 entry = read16(vram, entry_addr);
        const u32 tile_num = entry & 0x3FFu;
        const bool h_flip = (entry >> 10u & 1u) != 0;
        const bool v_flip = (entry >> 11u & 1u) != 0;
        const u32 pal_bank = (entry >> 12u & 0xFu) << 4u;

        const u32 y_off = v_flip ? (7u - fine_y) : fine_y;

        for (u32 tx = 0; tx < 8u; ++tx) {
            const u32 out_x = screen_x + tx;
            if (out_x >= kScreenWidth) {
                break;
            }

            const u32 tile_x = (fine_x + tx) & 7u;
            const u32 real_x = h_flip ? (7u - tile_x) : tile_x;

            if (cnt.color_mode()) {
                const u32 data_addr = char_base + tile_num * 0x40u + y_off * 8u + real_x;
                const u8 color_idx = vram[data_addr % vram.size()];
                if (color_idx == 0) {
                    continue;
                }
                row[out_x] = read16(palette, static_cast<u32>(color_idx) * 2u);
            } else {
                const u32 byte_addr = char_base + tile_num * 0x20u + y_off * 4u + (real_x / 2u);
                const u8 byte_val = vram[byte_addr % vram.size()];
                const u32 color_idx = (real_x & 1u) ? (byte_val >> 4u) : (byte_val & 0xFu);
                if (color_idx == 0) {
                    continue;
                }
                row[out_x] = read16(palette, (pal_bank + color_idx) * 2u);
            }
        }
    }
}

void Ppu::render_affine_bg(int line, std::span<const u8> vram, std::span<const u8> palette,
                           int bg, u16* row) {
    const auto bg_index = static_cast<std::size_t>(bg);
    const auto affine_index = static_cast<std::size_t>(bg - 2);
    const BgCnt cnt{bgcnt_[bg_index]};
    const auto screen_base = cnt.screen_base_block();
    const auto char_base = cnt.char_base_block();

    static const u32 size_pixels[] = {128, 256, 512, 1024};
    static const u32 size_map[] = {16, 32, 64, 128};
    const u32 bg_size = size_pixels[cnt.size()];
    const u32 map_size = size_map[cnt.size()];

    const s32 ref_x = bg_ref_x_[affine_index];
    const s32 ref_y = bg_ref_y_[affine_index];
    const s16 pa = bg_pa_[affine_index];
    const s16 pb = bg_pb_[affine_index];
    const s16 pc = bg_pc_[affine_index];
    const s16 pd = bg_pd_[affine_index];
    const auto line_offset = static_cast<s32>(line);

    for (u32 screen_x = 0; screen_x < kScreenWidth; ++screen_x) {
        s32 tex_x =
            (ref_x + static_cast<s32>(pa) * static_cast<s32>(screen_x) + static_cast<s32>(pb) * line_offset) >> 8;
        s32 tex_y =
            (ref_y + static_cast<s32>(pc) * static_cast<s32>(screen_x) + static_cast<s32>(pd) * line_offset) >> 8;

        if (cnt.wraparound()) {
            tex_x &= static_cast<s32>(bg_size - 1u);
            tex_y &= static_cast<s32>(bg_size - 1u);
        } else if (tex_x < 0 || tex_x >= static_cast<s32>(bg_size) ||
                   tex_y < 0 || tex_y >= static_cast<s32>(bg_size)) {
            continue;
        }

        const u32 tile_col = static_cast<u32>(tex_x) / 8u;
        const u32 tile_row = static_cast<u32>(tex_y) / 8u;
        const u32 fine_x = static_cast<u32>(tex_x) & 7u;
        const u32 fine_y = static_cast<u32>(tex_y) & 7u;

        const u32 entry_addr = screen_base + tile_row * map_size + tile_col;
        const u8 tile_num = vram[entry_addr % vram.size()];

        if (cnt.color_mode()) {
            const u32 data_addr = char_base + static_cast<u32>(tile_num) * 0x40u + fine_y * 8u + fine_x;
            const u8 color_idx = vram[data_addr % vram.size()];
            if (color_idx == 0) {
                continue;
            }
            row[screen_x] = read16(palette, static_cast<u32>(color_idx) * 2u);
        } else {
            const u32 byte_addr = char_base + static_cast<u32>(tile_num) * 0x20u + fine_y * 4u + (fine_x / 2u);
            const u8 byte_val = vram[byte_addr % vram.size()];
            const u32 color_idx = (fine_x & 1u) ? (byte_val >> 4u) : (byte_val & 0xFu);
            if (color_idx == 0) {
                continue;
            }
            const u32 pal_bank = (static_cast<u32>(tile_num) >> 8u) << 4u;
            row[screen_x] = read16(palette, (pal_bank + color_idx) * 2u);
        }
    }
}

u64 Ppu::next_event_cycle() const {
    return next_event_cycle_;
}

bool Ppu::is_hblank() const {
    return hblank_;
}

bool Ppu::is_vblank() const {
    return vblank_;
}

bool Ppu::is_video_memory_contended() const {
    return !force_blank() && !vblank_ && !hblank_;
}

bool Ppu::frame_ready() const {
    return frame_ready_;
}

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
    return {framebuffer_.get(), kFramebufferPixels};
}

u16 Ppu::dispcnt() const {
    return dispcnt_;
}

u16 Ppu::dispstat() const {
    return dispstat_;
}

u16 Ppu::vcount() const {
    return vcount_;
}

bool Ppu::force_blank() const {
    return test_bit(dispcnt_, 7);
}

bool Ppu::hblank_free() const {
    return test_bit(dispcnt_, 5);
}

void Ppu::update_dispstat_flags() {
    dispstat_ = static_cast<u16>(dispstat_ & ~0x0007u);
    if (vblank_) {
        dispstat_ |= 0x0001u;
    }
    if (hblank_) {
        dispstat_ |= 0x0002u;
    }
    if (((dispstat_ >> 8u) & 0x00FFu) == vcount_) {
        dispstat_ |= 0x0004u;
    }
}

void Ppu::enter_hblank(IrqController& irq) {
    hblank_ = true;
    update_dispstat_flags();
    if (!vblank_) {
        scanline_ready_ = static_cast<int>(vcount_);
        if (test_bit(dispstat_, 4)) {
            irq.request(IrqHBlank);
        }
    }
}

void Ppu::leave_hblank(IrqController& irq) {
    hblank_ = false;
    vcount_ = static_cast<u16>((vcount_ + 1u) % kScanlinesPerFrame);
    if (vcount_ == kVisibleScanlines) {
        enter_vblank(irq);
    } else if (vcount_ == 0) {
        leave_vblank();
    }
    update_dispstat_flags();
    handle_vcount_compare(irq);
}

void Ppu::enter_vblank(IrqController& irq) {
    vblank_ = true;
    frame_ready_ = true;
    update_dispstat_flags();
    if (test_bit(dispstat_, 3)) {
        irq.request(IrqVBlank);
    }
}

void Ppu::leave_vblank() {
    vblank_ = false;
    update_dispstat_flags();
}

void Ppu::handle_vcount_compare(IrqController& irq) {
    if (((dispstat_ >> 8u) & 0x00FFu) == vcount_) {
        dispstat_ |= 0x0004u;
        if (test_bit(dispstat_, 5)) {
            irq.request(IrqVCount);
        }
    } else {
        dispstat_ = static_cast<u16>(dispstat_ & ~0x0004u);
    }
}

void Ppu::mark_all_dirty() {
    all_dirty_ = true;
    scanline_dirty_.fill(true);
}

void Ppu::mark_dirty(int line) {
    if (line >= 0 && line < static_cast<int>(kScreenHeight)) {
        scanline_dirty_[static_cast<std::size_t>(line)] = true;
    }
}

bool Ppu::is_dirty(int line) const {
    if (all_dirty_) return true;
    if (line < 0 || line >= static_cast<int>(kScreenHeight)) return false;
    return scanline_dirty_[static_cast<std::size_t>(line)];
}

void Ppu::clear_dirty(int line) {
    if (line >= 0 && line < static_cast<int>(kScreenHeight)) {
        scanline_dirty_[static_cast<std::size_t>(line)] = false;
    }
    /* Check if all lines are now clean */
    if (all_dirty_) {
        all_dirty_ = false;
        for (std::size_t i = 0; i < scanline_dirty_.size(); ++i) {
            if (scanline_dirty_[i]) {
                all_dirty_ = false;
                break;
            }
        }
    }
}

}  // namespace gba
