#include "gba/core/ppu.hpp"

#include <algorithm>

#include "gba/core/constants.hpp"
#include "gba/core/irq.hpp"

namespace gba {

namespace {

[[nodiscard]] u16 read16(std::span<const u8> bytes, u32 offset) {
    if (bytes.empty()) {
        return 0;
    }
    const auto base = align_down(offset, 2u);
    return static_cast<u16>(bytes[base % bytes.size()] | (bytes[(base + 1u) % bytes.size()] << 8u));
}

void write_halfword(u16& target, u32 address, u32 value, BusWidth width) {
    if (width == BusWidth::Byte) {
        const auto shift = (address & 1u) * 8u;
        target = static_cast<u16>((target & ~(0xFFu << shift)) | ((value & 0xFFu) << shift));
    } else {
        target = static_cast<u16>(value & 0xFFFFu);
    }
}

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
    framebuffer_.fill(0x7FFF);
    next_event_cycle_ = 960;
    hblank_ = false;
    vblank_ = false;
    frame_ready_ = false;
    scanline_ready_.reset();
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
        case kBg0Cnt + 6u:
            return bgcnt_[(half_address - kBg0Cnt) / 2u];
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
            return winin_;
        case 0x0400004Au:
            return winout_;
        case kMosaic:
            return mosaic_;
        case kBldCnt:
            return bldcnt_;
        case kBldCnt + 2u:
            return bldalpha_;
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
}

void Ppu::advance_to(u64 cycle_now, IrqController& irq) {
    while (next_event_cycle_ <= cycle_now) {
        if (!hblank_) {
            enter_hblank(irq);
            next_event_cycle_ += 272;
        } else {
            leave_hblank(irq);
            next_event_cycle_ += 960;
        }
    }
}

void Ppu::render_scanline(int line, std::span<const u8> vram, std::span<const u8> palette) {
    if (line < 0 || line >= static_cast<int>(kScreenHeight)) {
        return;
    }

    const auto backdrop = read16(palette, 0);
    auto* row = framebuffer_.data() + (static_cast<std::size_t>(line) * kScreenWidth);

    if (force_blank()) {
        std::fill_n(row, kScreenWidth, static_cast<u16>(0x7FFF));
        return;
    }

    const auto mode = dispcnt_ & 0x0007u;
    switch (mode) {
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
        std::fill_n(row, kScreenWidth, backdrop);
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
        std::fill_n(row, kScreenWidth, backdrop);
        break;
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

const std::array<u16, kFramebufferPixels>& Ppu::framebuffer() const {
    return framebuffer_;
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

}  // namespace gba
