#include "gba/core/bus.hpp"

#include <algorithm>

#include "gba/core/apu.hpp"
#include "gba/core/constants.hpp"
#include "gba/core/dma.hpp"
#include "gba/core/irq.hpp"
#include "gba/core/ppu.hpp"
#include "gba/core/timers.hpp"

namespace gba {

Bus::Bus(Cartridge& cartridge, Ppu& ppu, Timers& timers, DmaEngine& dma, Apu& apu, IrqController& irq)
    : cartridge_(cartridge), ppu_(ppu), timers_(timers), dma_(dma), apu_(apu), irq_(irq) {}

void Bus::reset() {
    ewram_.fill(0);
    iwram_.fill(0);
    palette_.fill(0);
    vram_.fill(0);
    oam_.fill(0);
    keyinput_ = 0x03FF;
    keycnt_ = 0;
    waitcnt_ = 0;
    postflg_ = 0;
    halted_ = false;
    open_bus_ = 0;
}

BusAccessResult Bus::read(u32 address, BusWidth width, AccessType access, u64 cycle_now) {
    BusAccessResult result{};
    result.cycles = region_cycles(address, width, access, cycle_now);

    if (address < 0x00004000u) {
        result.value = cartridge_.read_bios(address, width);
    } else if ((address & 0x0F000000u) == 0x02000000u) {
        result.value = read_array(ewram_, address - 0x02000000u, width);
    } else if ((address & 0x0F000000u) == 0x03000000u) {
        result.value = read_array(iwram_, address - 0x03000000u, width);
    } else if ((address & 0x0F000000u) == 0x04000000u) {
        return read_io(address, width, cycle_now);
    } else if ((address & 0x0F000000u) == 0x05000000u) {
        result.value = read_array(palette_, address - 0x05000000u, width);
    } else if ((address & 0x0F000000u) == 0x06000000u) {
        auto offset = (address - 0x06000000u) & 0x1FFFFu;
        if (offset >= 0x18000u) {
            offset = 0x10000u + (offset & 0x7FFFu);
        }
        result.value = read_array(vram_, offset, width);
    } else if ((address & 0x0F000000u) == 0x07000000u) {
        result.value = read_array(oam_, address - 0x07000000u, width);
    } else if ((address & 0x0E000000u) == 0x08000000u) {
        result.value = cartridge_.read_rom(address - 0x08000000u, width);
    } else if ((address & 0x0E000000u) == 0x0E000000u) {
        result.value = cartridge_.read_save(address - 0x0E000000u, width);
    } else {
        result.value = open_bus_;
        result.open_bus = true;
    }

    open_bus_ = result.value;
    return result;
}

BusAccessResult Bus::write(u32 address, u32 value, BusWidth width, AccessType access, u64 cycle_now) {
    BusAccessResult result{};
    result.value = value;
    result.cycles = region_cycles(address, width, access, cycle_now);

    if ((address & 0x0F000000u) == 0x02000000u) {
        write_array(ewram_, address - 0x02000000u, value, width);
    } else if ((address & 0x0F000000u) == 0x03000000u) {
        write_array(iwram_, address - 0x03000000u, value, width);
    } else if ((address & 0x0F000000u) == 0x04000000u) {
        return write_io(address, value, width, cycle_now);
    } else if ((address & 0x0F000000u) == 0x05000000u) {
        if (width == BusWidth::Byte) {
            const auto aligned = align_down(address - 0x05000000u, 2u);
            const auto replicated = static_cast<u16>((value & 0xFFu) * 0x0101u);
            write_array(palette_, aligned, replicated, BusWidth::Half);
        } else {
            write_array(palette_, address - 0x05000000u, value, width);
        }
    } else if ((address & 0x0F000000u) == 0x06000000u) {
        auto offset = (address - 0x06000000u) & 0x1FFFFu;
        if (offset >= 0x18000u) {
            offset = 0x10000u + (offset & 0x7FFFu);
        }

        if (width == BusWidth::Byte) {
            if (offset >= 0x14000u) {
                return result;
            }
            const auto aligned = align_down(offset, 2u);
            const auto replicated = static_cast<u16>((value & 0xFFu) * 0x0101u);
            write_array(vram_, aligned, replicated, BusWidth::Half);
        } else {
            write_array(vram_, offset, value, width);
        }
    } else if ((address & 0x0F000000u) == 0x07000000u) {
        if (width != BusWidth::Byte) {
            write_array(oam_, address - 0x07000000u, value, width);
        }
    } else if ((address & 0x0E000000u) == 0x0E000000u) {
        cartridge_.write_save(address - 0x0E000000u, value, width);
    }

    open_bus_ = value;
    return result;
}

std::span<const u8> Bus::vram() const {
    return vram_;
}

std::span<const u8> Bus::palette() const {
    return palette_;
}

std::span<const u8> Bus::oam() const {
    return oam_;
}

std::span<u8> Bus::ewram() {
    return ewram_;
}

std::span<u8> Bus::iwram() {
    return iwram_;
}

void Bus::set_keyinput(u16 value) {
    keyinput_ = static_cast<u16>(value & 0x03FFu);
    update_keypad_irq();
}

u16 Bus::keyinput() const {
    return keyinput_;
}

u16 Bus::keycnt() const {
    return keycnt_;
}

u16 Bus::waitcnt() const {
    return waitcnt_;
}

bool Bus::has_bios() const {
    return cartridge_.has_bios();
}

bool Bus::halted() const {
    return halted_;
}

void Bus::clear_halt() {
    halted_ = false;
}

void Bus::update_keypad_irq() {
    if (!test_bit(keycnt_, 14)) {
        return;
    }

    const auto selected_mask = static_cast<u16>(keycnt_ & 0x03FFu);
    if (selected_mask == 0u) {
        return;
    }

    const auto pressed_mask = static_cast<u16>((~keyinput_) & 0x03FFu);
    const auto selected_pressed = static_cast<u16>(pressed_mask & selected_mask);
    const auto and_mode = test_bit(keycnt_, 15);
    const auto condition_met =
        and_mode ? selected_pressed == selected_mask : selected_pressed != 0u;

    if (condition_met) {
        irq_.request(IrqKeypad);
    }
}

u32 Bus::read_array(std::span<const u8> bytes, u32 address, BusWidth width) {
    if (bytes.empty()) {
        return 0xFFFFFFFFu;
    }

    const auto normalized = address % static_cast<u32>(bytes.size());
    const auto get = [&](u32 offset) { return bytes[offset % static_cast<u32>(bytes.size())]; };

    switch (width) {
    case BusWidth::Byte:
        return get(normalized);
    case BusWidth::Half: {
        const auto aligned = align_down(normalized, 2u);
        return get(aligned) | (get(aligned + 1u) << 8u);
    }
    case BusWidth::Word: {
        const auto aligned = align_down(normalized, 4u);
        return get(aligned) | (get(aligned + 1u) << 8u) | (get(aligned + 2u) << 16u) | (get(aligned + 3u) << 24u);
    }
    }
    return 0xFFFFFFFFu;
}

void Bus::write_array(std::span<u8> bytes, u32 address, u32 value, BusWidth width) {
    if (bytes.empty()) {
        return;
    }

    const auto put = [&](u32 offset, u8 byte) { bytes[offset % static_cast<u32>(bytes.size())] = byte; };
    switch (width) {
    case BusWidth::Byte:
        put(address, static_cast<u8>(value));
        break;
    case BusWidth::Half: {
        const auto aligned = align_down(address, 2u);
        put(aligned, static_cast<u8>(value & 0xFFu));
        put(aligned + 1u, static_cast<u8>((value >> 8u) & 0xFFu));
        break;
    }
    case BusWidth::Word: {
        const auto aligned = align_down(address, 4u);
        put(aligned, static_cast<u8>(value & 0xFFu));
        put(aligned + 1u, static_cast<u8>((value >> 8u) & 0xFFu));
        put(aligned + 2u, static_cast<u8>((value >> 16u) & 0xFFu));
        put(aligned + 3u, static_cast<u8>((value >> 24u) & 0xFFu));
        break;
    }
    }
}

u32 Bus::region_cycles(u32 address, BusWidth width, AccessType access, u64 cycle_now) const {
    (void)cycle_now;

    const auto contiguous_video_penalty = [&]() -> u32 {
        return ppu_.is_video_memory_contended() ? 1u : 0u;
    };

    if (address < 0x00004000u) {
        return 1;
    }
    if ((address & 0x0F000000u) == 0x02000000u) {
        return width == BusWidth::Word ? 6u : 3u;
    }
    if ((address & 0x0F000000u) == 0x03000000u) {
        return 1;
    }
    if ((address & 0x0F000000u) == 0x04000000u) {
        return 1;
    }
    if ((address & 0x0F000000u) == 0x05000000u || (address & 0x0F000000u) == 0x06000000u ||
        (address & 0x0F000000u) == 0x07000000u) {
        const auto base = width == BusWidth::Word ? 2u : 1u;
        return base + contiguous_video_penalty();
    }
    if ((address & 0x0E000000u) == 0x08000000u) {
        const auto sequential = access == AccessType::Sequential;
        const auto region = (address >> 25u) & 0x3u;
        const auto first_access = std::array<u32, 4>{5u, 5u, 5u, 5u};
        const auto second_access = std::array<u32, 4>{3u, 5u, 9u, 3u};
        const auto half_cycles = sequential ? second_access[region] : first_access[region];
        return width == BusWidth::Word ? (half_cycles + second_access[region]) : half_cycles;
    }
    if ((address & 0x0E000000u) == 0x0E000000u) {
        return 5;
    }
    return 1;
}

BusAccessResult Bus::read_io(u32 address, BusWidth width, u64 cycle_now) {
    (void)cycle_now;
    BusAccessResult result{};
    result.cycles = 1;

    if ((address >= kDispcnt && address <= kBldCnt + 4u) || address == kVcount) {
        result.value = ppu_.read_register(address, width);
    } else if (address >= kSoundCntL && address <= kSoundBias) {
        result.value = apu_.read_register(address, width);
    } else if ((address >= kDma0Sad && address < kDma0Sad + 48u)) {
        result.value = dma_.read_register(address, width);
    } else if (address >= kTm0CntL && address <= kTm0CntH + 12u) {
        result.value = timers_.read_register(address, width);
    } else if (address == kKeyInput || address == kKeyCnt) {
        const auto half = address == kKeyInput ? keyinput_ : keycnt_;
        if (width == BusWidth::Byte) {
            const auto shift = (address & 1u) * 8u;
            result.value = (half >> shift) & 0xFFu;
        } else {
            result.value = half;
        }
    } else if (address == kIe || address == kIf || address == kIme) {
        result.value = irq_.read_register(address, width);
    } else if (address == kWaitCnt) {
        result.value = waitcnt_;
    } else if (address == kPostFlg || address == kHaltCnt) {
        result.value = postflg_;
    } else {
        result.value = open_bus_;
        result.open_bus = true;
    }

    open_bus_ = result.value;
    return result;
}

BusAccessResult Bus::write_io(u32 address, u32 value, BusWidth width, u64 cycle_now) {
    BusAccessResult result{};
    result.value = value;
    result.cycles = 1;

    if ((address >= kDispcnt && address <= kBldCnt + 4u) || address == kVcount) {
        ppu_.write_register(address, value, width);
    } else if (address >= kSoundCntL && address <= kFifoB + 2u) {
        apu_.write_register(address, value, width, cycle_now);
    } else if (address >= kDma0Sad && address < kDma0Sad + 48u) {
        dma_.write_register(address, value, width, cycle_now);
    } else if (address >= kTm0CntL && address <= kTm0CntH + 12u) {
        timers_.write_register(address, value, width, cycle_now);
    } else if (address == kKeyCnt) {
        keycnt_ = static_cast<u16>(value & 0xFFFFu);
        update_keypad_irq();
    } else if (address == kIe || address == kIf || address == kIme) {
        irq_.write_register(address, value, width);
    } else if (address == kWaitCnt) {
        waitcnt_ = static_cast<u16>(value & 0xFFFFu);
    } else if (address == kPostFlg) {
        postflg_ = static_cast<u8>(value & 0x01u);
    } else if (address == kHaltCnt) {
        halted_ = true;
    }

    open_bus_ = value;
    return result;
}

}  // namespace gba
