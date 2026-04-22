#include "gba/core/bus.hpp"

#include <algorithm>
#include <cstring>

#ifdef GBA_PLATFORM_ESP32
#include "esp_heap_caps.h"
#endif

#include "gba/core/apu.hpp"
#include "gba/core/constants.hpp"
#include "gba/core/dma.hpp"
#include "gba/core/irq.hpp"
#include "gba/core/ppu.hpp"
#include "gba/core/timers.hpp"

namespace gba {

namespace {

/* Allocate memory — prefer PSRAM on ESP32, fall back to heap */
u8* alloc_memory(size_t size) {
#ifdef GBA_PLATFORM_ESP32
    auto* ptr = static_cast<u8*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
    if (ptr) {
        std::memset(ptr, 0, size);
        return ptr;
    }
    ptr = static_cast<u8*>(heap_caps_malloc(size, MALLOC_CAP_8BIT));
    if (ptr) {
        std::memset(ptr, 0, size);
        return ptr;
    }
    return nullptr;
#else
    return new u8[size]();
#endif
}

[[nodiscard]] u32 narrow_latch(u32 latch, BusWidth width, u32 address) {
    switch (width) {
    case BusWidth::Byte:
        return (latch >> ((address & 3u) * 8u)) & 0xFFu;
    case BusWidth::Half:
        return (latch >> ((address & 2u) * 8u)) & 0xFFFFu;
    case BusWidth::Word:
        return latch;
    }
    return latch;
}

[[nodiscard]] u32 gamepak_open_bus_value(u32 address, BusWidth width) {
    const auto half_at = [](u32 half_address) -> u32 {
        return (align_down(half_address, 2u) >> 1u) & 0xFFFFu;
    };

    switch (width) {
    case BusWidth::Byte: {
        const auto half = half_at(address);
        return (half >> ((address & 1u) * 8u)) & 0xFFu;
    }
    case BusWidth::Half:
        return half_at(address);
    case BusWidth::Word: {
        const auto aligned = align_down(address, 4u);
        return half_at(aligned) | (half_at(aligned + 2u) << 16u);
    }
    }
    return 0xFFFFFFFFu;
}

}  // namespace

Bus::Bus(Cartridge& cartridge, Ppu& ppu, Timers& timers, DmaEngine& dma, Apu& apu, IrqController& irq)
    : cartridge_(cartridge), ppu_(ppu), timers_(timers), dma_(dma), apu_(apu), irq_(irq),
      ewram_(alloc_memory(kEwramSize)),
      iwram_(alloc_memory(kIwramSize)),
      palette_(alloc_memory(kPaletteSize)),
      vram_(alloc_memory(kVramSize)),
      oam_(alloc_memory(kOamSize)) {}

void Bus::reset() {
    if (ewram_) std::memset(ewram_.get(), 0, kEwramSize);
    if (iwram_) std::memset(iwram_.get(), 0, kIwramSize);
    if (palette_) std::memset(palette_.get(), 0, kPaletteSize);
    if (vram_) std::memset(vram_.get(), 0, kVramSize);
    if (oam_) std::memset(oam_.get(), 0, kOamSize);
    keyinput_ = 0x03FF;
    keycnt_ = 0;
    waitcnt_ = 0;
    postflg_ = 0;
    mgba_log_enable_ = 0;
    halted_ = false;
    bios_latch_valid_ = false;
    rom_latch_valid_ = false;
    bios_latch_ = 0;
    rom_latch_ = 0;
    open_bus_ = 0;
    prefetch_ = {};
    debug_string_.fill('\0');
}

BusAccessResult Bus::read(u32 address, BusWidth width, AccessType access, u64 cycle_now) {
    BusAccessResult result{};
    result.cycles = region_cycles(address, width, access, cycle_now);

    const auto ewram_span = std::span<const u8>{ewram_.get(), kEwramSize};
    const auto iwram_span = std::span<const u8>{iwram_.get(), kIwramSize};
    const auto palette_span = std::span<const u8>{palette_.get(), kPaletteSize};
    const auto vram_span = std::span<const u8>{vram_.get(), kVramSize};
    const auto oam_span = std::span<const u8>{oam_.get(), kOamSize};

    if (address < 0x00004000u) {
        const auto bios = cartridge_.bios();
        const auto aligned = align_down(address, static_cast<u32>(width));
        const auto width_bytes = static_cast<u32>(width);
        const auto protected_cpu_read = has_access_flag(access, AccessType::CpuOutsideBios);
        if (protected_cpu_read && bios_latch_valid_) {
            result.value = narrow_latch(bios_latch_, width, address);
        } else if (protected_cpu_read) {
            result.value = open_bus_;
            result.open_bus = true;
        } else if (aligned + width_bytes <= bios.size()) {
            result.value = cartridge_.read_bios(address, width);
            if (has_access_flag(access, AccessType::CodeFetch)) {
                const auto pipeline_offset = width == BusWidth::Word ? 8u : 4u;
                const auto word_base = align_down(aligned + pipeline_offset, 4u);
                if (word_base + 4u <= bios.size()) {
                    bios_latch_ = cartridge_.read_bios(word_base, BusWidth::Word);
                    bios_latch_valid_ = true;
                } else {
                    bios_latch_ = expand_bus_latch(result.value, width);
                    bios_latch_valid_ = true;
                }
            }
        } else if (bios_latch_valid_) {
            result.value = narrow_latch(bios_latch_, width, address);
        } else {
            result.value = open_bus_;
            result.open_bus = true;
        }
    } else if ((address & 0x0F000000u) == 0x02000000u) {
        result.value = read_array(ewram_span, address - 0x02000000u, width);
    } else if ((address & 0x0F000000u) == 0x03000000u) {
        result.value = read_array(iwram_span, address - 0x03000000u, width);
    } else if ((address & 0x0F000000u) == 0x04000000u) {
        return read_io(address, width, access, cycle_now);
    } else if ((address & 0x0F000000u) == 0x05000000u) {
        result.value = read_array(palette_span, address - 0x05000000u, width);
    } else if ((address & 0x0F000000u) == 0x06000000u) {
        auto offset = (address - 0x06000000u) & 0x1FFFFu;
        if (offset >= 0x18000u) {
            offset = 0x10000u + (offset & 0x7FFFu);
        }
        result.value = read_array(vram_span, offset, width);
    } else if ((address & 0x0F000000u) == 0x07000000u) {
        result.value = read_array(oam_span, address - 0x07000000u, width);
    } else if (address >= 0x08000000u && address < 0x0E000000u) {
        const auto is_code_fetch = has_access_flag(access, AccessType::CodeFetch);
        const auto offset = address - 0x08000000u;
        const auto rom = cartridge_.rom();
        const auto aligned = align_down(offset, static_cast<u32>(width));
        const auto width_bytes = static_cast<u32>(width);

        /* Check prefetch buffer hit for code fetches */
        const auto entries_needed = (width == BusWidth::Word) ? 2 : 1;
        if (is_code_fetch && prefetch_.active && prefetch_.head_address == address &&
            prefetch_.count >= entries_needed) {
            /* Prefetch hit — 1 internal cycle, ROM bus stays free */
            result.cycles = 1;
            if (aligned + width_bytes <= rom.size()) {
                result.value = cartridge_.read_rom(offset, width);
            } else {
                result.value = gamepak_open_bus_value(address, width);
            }
            prefetch_.count -= entries_needed;
            prefetch_.head_address += entries_needed * 2u;
            prefetch_advance(1);
        } else {
            /* Prefetch miss or data fetch — full ROM access */
            prefetch_stop();

            if (aligned + width_bytes <= rom.size()) {
                result.value = cartridge_.read_rom(offset, width);
                if (!is_code_fetch) {
                    const auto word_base = align_down(aligned, 4u);
                    if (word_base + 4u <= rom.size()) {
                        rom_latch_ = cartridge_.read_rom(word_base, BusWidth::Word);
                        rom_latch_valid_ = true;
                    } else {
                        rom_latch_ = expand_bus_latch(result.value, width);
                        rom_latch_valid_ = true;
                    }
                }
            } else {
                result.value = gamepak_open_bus_value(address, width);
            }

            /* Start prefetch after ROM code fetch if enabled */
            if (is_code_fetch && test_bit(waitcnt_, 14u)) {
                const auto next_address = address + width_bytes;
                const auto seq_half = prefetch_region_cycles(next_address, BusWidth::Half);
                prefetch_.active = true;
                prefetch_.head_address = next_address;
                prefetch_.last_address = next_address;
                prefetch_.count = 0;
                prefetch_.capacity = 8;
                prefetch_.opcode_width = 2;
                prefetch_.duty = static_cast<int>(seq_half);
                prefetch_.countdown = static_cast<int>(seq_half);
            }
        }
    } else if (address >= 0x0E000000u && address < 0x10000000u) {
        result.value = cartridge_.read_save(address - 0x0E000000u, width);
        prefetch_stop();
    } else {
        result.value = open_bus_;
        result.open_bus = true;
    }

    record_open_bus_read(result.value, width);
    return result;
}

BusAccessResult Bus::write(u32 address, u32 value, BusWidth width, AccessType access, u64 cycle_now) {
    BusAccessResult result{};
    result.value = value;
    result.cycles = region_cycles(address, width, access, cycle_now);

    const auto ewram_w = std::span<u8>{ewram_.get(), kEwramSize};
    const auto iwram_w = std::span<u8>{iwram_.get(), kIwramSize};
    const auto palette_w = std::span<u8>{palette_.get(), kPaletteSize};
    const auto vram_w = std::span<u8>{vram_.get(), kVramSize};
    const auto oam_w = std::span<u8>{oam_.get(), kOamSize};

    if ((address & 0x0F000000u) == 0x02000000u) {
        write_array(ewram_w, address - 0x02000000u, value, width);
    } else if ((address & 0x0F000000u) == 0x03000000u) {
        write_array(iwram_w, address - 0x03000000u, value, width);
    } else if ((address & 0x0F000000u) == 0x04000000u) {
        return write_io(address, value, width, cycle_now);
    } else if ((address & 0x0F000000u) == 0x05000000u) {
        if (width == BusWidth::Byte) {
            const auto aligned = align_down(address - 0x05000000u, 2u);
            const auto replicated = static_cast<u16>((value & 0xFFu) * 0x0101u);
            write_array(palette_w, aligned, replicated, BusWidth::Half);
        } else {
            write_array(palette_w, address - 0x05000000u, value, width);
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
            write_array(vram_w, aligned, replicated, BusWidth::Half);
        } else {
            write_array(vram_w, offset, value, width);
        }
    } else if ((address & 0x0F000000u) == 0x07000000u) {
        if (width != BusWidth::Byte) {
            write_array(oam_w, address - 0x07000000u, value, width);
        }
    } else if (address >= 0x0E000000u && address < 0x10000000u) {
        cartridge_.write_save(address - 0x0E000000u, value, width);
        prefetch_stop();
    }

    if (!has_access_flag(access, AccessType::Dma) && (address & 0x0F000000u) == 0x04000000u) {
        const auto dma_cycle = cycle_now + result.cycles;
        result.cycles += service_dma(dma_cycle);
    }

    return result;
}

std::span<const u8> Bus::vram() const {
    return {vram_.get(), kVramSize};
}

std::span<const u8> Bus::palette() const {
    return {palette_.get(), kPaletteSize};
}

std::span<const u8> Bus::oam() const {
    return {oam_.get(), kOamSize};
}

std::span<u8> Bus::ewram() {
    return {ewram_.get(), kEwramSize};
}

std::span<u8> Bus::iwram() {
    return {iwram_.get(), kIwramSize};
}

void Bus::set_keyinput(u16 value) {
    keyinput_ = static_cast<u16>(value & 0x03FFu);
    update_keypad_irq();
}

void Bus::set_debug_output(DebugOutputCallback callback) {
    debug_callback_ = std::move(callback);
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

void Bus::service_timers(u64 cycle_now) {
    timers_.advance_to(cycle_now, irq_, apu_);
    if (apu_.take_fifo_request_a()) {
        dma_.request_fifo_a(cycle_now);
    }
    if (apu_.take_fifo_request_b()) {
        dma_.request_fifo_b(cycle_now);
    }
}

void Bus::prefetch_stop() {
    prefetch_.active = false;
}

void Bus::prefetch_advance(int cycles) {
    if (!prefetch_.active) {
        return;
    }
    prefetch_.countdown -= cycles;
    while (prefetch_.countdown <= 0) {
        prefetch_.count++;
        if (prefetch_.count < prefetch_.capacity) {
            prefetch_.last_address += static_cast<u32>(prefetch_.opcode_width);
            prefetch_.countdown += prefetch_.duty;
        } else {
            break;
        }
    }
}

u32 Bus::service_dma(u64 cycle_now) {
    if (!dma_.has_pending_transfer() || dma_.next_event_cycle() > cycle_now) {
        return 0;
    }
    return dma_.service_due(cycle_now, *this, irq_);
}

void Bus::update_keypad_irq() {
    if (!test_bit(keycnt_, 14)) {
        return;
    }

    const auto selected_mask = static_cast<u16>(keycnt_ & 0x03FFu);
    if (selected_mask == 0u) {
        return;
    }

    const auto pressed_mask = static_cast<u16>(static_cast<u16>(~keyinput_) & 0x03FFu);
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
        return static_cast<u32>(get(aligned)) | (static_cast<u32>(get(aligned + 1u)) << 8u);
    }
    case BusWidth::Word: {
        const auto aligned = align_down(normalized, 4u);
        return static_cast<u32>(get(aligned)) | (static_cast<u32>(get(aligned + 1u)) << 8u) |
               (static_cast<u32>(get(aligned + 2u)) << 16u) | (static_cast<u32>(get(aligned + 3u)) << 24u);
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

u32 Bus::peek_word(u32 address) const {
    const auto ewram_span = std::span<const u8>{ewram_.get(), kEwramSize};
    const auto iwram_span = std::span<const u8>{iwram_.get(), kIwramSize};
    const auto palette_span = std::span<const u8>{palette_.get(), kPaletteSize};
    const auto vram_span = std::span<const u8>{vram_.get(), kVramSize};
    const auto oam_span = std::span<const u8>{oam_.get(), kOamSize};
    const auto peek_byte = [&](u32 byte_address) -> u8 {
        if (byte_address < 0x00004000u) {
            return static_cast<u8>(cartridge_.read_bios(byte_address, BusWidth::Byte) & 0xFFu);
        }
        if ((byte_address & 0x0F000000u) == 0x02000000u) {
            return static_cast<u8>(read_array(ewram_span, byte_address - 0x02000000u, BusWidth::Byte) & 0xFFu);
        }
        if ((byte_address & 0x0F000000u) == 0x03000000u) {
            return static_cast<u8>(read_array(iwram_span, byte_address - 0x03000000u, BusWidth::Byte) & 0xFFu);
        }
        if ((byte_address & 0x0F000000u) == 0x05000000u) {
            return static_cast<u8>(read_array(palette_span, byte_address - 0x05000000u, BusWidth::Byte) & 0xFFu);
        }
        if ((byte_address & 0x0F000000u) == 0x06000000u) {
            auto offset = (byte_address - 0x06000000u) & 0x1FFFFu;
            if (offset >= 0x18000u) {
                offset = 0x10000u + (offset & 0x7FFFu);
            }
            return static_cast<u8>(read_array(vram_span, offset, BusWidth::Byte) & 0xFFu);
        }
        if ((byte_address & 0x0F000000u) == 0x07000000u) {
            return static_cast<u8>(read_array(oam_span, byte_address - 0x07000000u, BusWidth::Byte) & 0xFFu);
        }
        if (byte_address >= 0x08000000u && byte_address < 0x0E000000u) {
            return static_cast<u8>(cartridge_.read_rom(byte_address - 0x08000000u, BusWidth::Byte) & 0xFFu);
        }
        return 0;
    };

    return static_cast<u32>(peek_byte(address)) | (static_cast<u32>(peek_byte(address + 1u)) << 8u) |
           (static_cast<u32>(peek_byte(address + 2u)) << 16u) |
           (static_cast<u32>(peek_byte(address + 3u)) << 24u);
}

u32 Bus::expand_bus_latch(u32 value, BusWidth width) {
    switch (width) {
    case BusWidth::Byte: {
        const auto byte = value & 0xFFu;
        return byte | (byte << 8u) | (byte << 16u) | (byte << 24u);
    }
    case BusWidth::Half: {
        const auto half = value & 0xFFFFu;
        return half | (half << 16u);
    }
    case BusWidth::Word:
        return value;
    }
    return value;
}

void Bus::record_open_bus_read(u32 value, BusWidth width) {
    const auto expanded = expand_bus_latch(value, width);
    open_bus_ = expanded;
}

namespace {

[[nodiscard]] u32 narrow_open_bus(u32 open_bus, BusWidth width, u32 address) {
    switch (width) {
    case BusWidth::Byte:
        return (open_bus >> ((address & 3u) * 8u)) & 0xFFu;
    case BusWidth::Half:
        return (open_bus >> ((address & 2u) * 8u)) & 0xFFFFu;
    case BusWidth::Word:
        return open_bus;
    }
    return open_bus;
}

[[nodiscard]] bool is_write_only_ppu_halfword(u32 address) {
    switch (address) {
    case 0x04000010u:
    case 0x04000012u:
    case 0x04000014u:
    case 0x04000016u:
    case 0x04000018u:
    case 0x0400001Au:
    case 0x0400001Cu:
    case 0x0400001Eu:
    case 0x04000020u:
    case 0x04000022u:
    case 0x04000024u:
    case 0x04000026u:
    case 0x04000028u:
    case 0x0400002Au:
    case 0x0400002Cu:
    case 0x0400002Eu:
    case 0x04000030u:
    case 0x04000032u:
    case 0x04000034u:
    case 0x04000036u:
    case 0x04000038u:
    case 0x0400003Au:
    case 0x0400003Cu:
    case 0x0400003Eu:
    case 0x04000040u:
    case 0x04000042u:
    case 0x04000044u:
    case 0x04000046u:
    case 0x0400004Cu:
    case 0x04000054u:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool is_open_bus_io_halfword(u32 address) {
    if (is_write_only_ppu_halfword(address)) {
        return true;
    }
    switch (address) {
    case 0x0400004Eu:
    case 0x04000056u:
    case 0x04000058u:
    case 0x0400005Au:
    case 0x0400005Cu:
    case 0x0400005Eu:
    case 0x0400008Cu:
    case 0x0400008Eu:
    case kFifoA:
    case kFifoA + 2u:
    case kFifoB:
    case kFifoB + 2u:
    case 0x040000A8u:
    case 0x040000AAu:
    case 0x040000ACu:
    case 0x040000AEu:
    case kDma0Sad:
    case kDma0Sad + 2u:
    case kDma0Dad:
    case kDma0Dad + 2u:
    case kDma0Sad + 12u:
    case kDma0Sad + 14u:
    case kDma0Dad + 12u:
    case kDma0Dad + 14u:
    case kDma0Sad + 24u:
    case kDma0Sad + 26u:
    case kDma0Dad + 24u:
    case kDma0Dad + 26u:
    case kDma0Sad + 36u:
    case kDma0Sad + 38u:
    case kDma0Dad + 36u:
    case kDma0Dad + 38u:
    case 0x040000E0u:
    case 0x040000E2u:
    case 0x040000E4u:
    case 0x040000E6u:
    case 0x040000E8u:
    case 0x040000EAu:
    case 0x040000ECu:
    case 0x040000EEu:
    case 0x040000F0u:
    case 0x040000F2u:
    case 0x040000F4u:
    case 0x040000F6u:
    case 0x040000F8u:
    case 0x040000FAu:
    case 0x040000FCu:
    case 0x040000FEu:
    case 0x0400100Cu:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool is_zero_read_io_halfword(u32 address) {
    switch (address) {
    case 0x04000136u:
    case 0x04000142u:
    case 0x0400015Au:
    case 0x04000206u:
    case 0x0400020Au:
    case 0x04000302u:
        return true;
    default:
        return false;
    }
}

}  // namespace

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
    if (address >= 0x08000000u && address < 0x0E000000u) {
        const auto sequential = has_access_flag(access, AccessType::Sequential);
        const auto region = (address >> 25u) & 0x3u;
        const auto first_shift = std::array<u32, 3>{2u, 5u, 8u};
        const auto second_bit = std::array<u32, 3>{4u, 7u, 10u};
        const auto first_cycles = std::array<u32, 4>{5u, 4u, 3u, 9u};
        const auto slow_second_cycles = std::array<u32, 3>{3u, 5u, 9u};
        const auto first = first_cycles[(waitcnt_ >> first_shift[region]) & 0x3u];
        const auto second = test_bit(waitcnt_, second_bit[region]) ? 2u : slow_second_cycles[region];
        const auto half_cycles = sequential ? second : first;
        return width == BusWidth::Word ? (half_cycles + second) : half_cycles;
    }
    if (address >= 0x0E000000u && address < 0x10000000u) {
        const auto save_cycles = std::array<u32, 4>{5u, 4u, 3u, 9u};
        return save_cycles[waitcnt_ & 0x3u];
    }
    return 1;
}

u32 Bus::prefetch_region_cycles(u32 address, BusWidth width) const {
    /* ROM sequential wait state calculation for prefetch duty */
    const auto region = (address >> 25u) & 0x3u;
    const auto second_bit = std::array<u32, 3>{4u, 7u, 10u};
    const auto slow_second_cycles = std::array<u32, 3>{3u, 5u, 9u};
    const auto second = test_bit(waitcnt_, second_bit[region]) ? 2u : slow_second_cycles[region];
    return width == BusWidth::Word ? (second + second) : second;
}

BusAccessResult Bus::read_io(u32 address, BusWidth width, AccessType access, u64 cycle_now) {
    (void)access;
    (void)cycle_now;
    BusAccessResult result{};
    result.cycles = 1;
    const auto io_address = width == BusWidth::Word ? align_down(address, 4u)
                                                    : (width == BusWidth::Half ? align_down(address, 2u) : address);

    if (width == BusWidth::Half && is_open_bus_io_halfword(io_address)) {
        result.value = narrow_open_bus(open_bus_, width, io_address);
        result.open_bus = true;
        record_open_bus_read(result.value, width);
        return result;
    }
    if (width == BusWidth::Half && is_zero_read_io_halfword(io_address)) {
        result.value = 0;
        record_open_bus_read(result.value, width);
        return result;
    }

    if ((io_address >= kDispcnt && io_address <= kBldCnt + 4u) || io_address == kVcount) {
        result.value = ppu_.read_register(io_address, width);
    } else if (io_address >= 0x04000060u && io_address <= kFifoB + 2u) {
        result.value = apu_.read_register(io_address, width);
    } else if ((io_address >= kDma0Sad && io_address < kDma0Sad + 48u)) {
        result.value = dma_.read_register(io_address, width);
    } else if (io_address >= kTm0CntL && io_address <= kTm0CntH + 12u) {
        result.value = timers_.read_register(io_address, width, cycle_now);
    } else if (io_address == kKeyInput || io_address == kKeyCnt) {
        const auto half = io_address == kKeyInput ? keyinput_ : keycnt_;
        if (width == BusWidth::Byte) {
            const auto shift = (io_address & 1u) * 8u;
            result.value = (half >> shift) & 0xFFu;
        } else {
            result.value = half;
        }
    } else if (io_address == kIe || io_address == kIf || io_address == kIme) {
        result.value = irq_.read_register(io_address, width);
    } else if (io_address == kSioCnt) {
        result.value = siocnt_;
    } else if (io_address == kRcnt) {
        result.value = rcnt_;
    } else if (io_address >= kSioMulti0 && io_address < kSioMulti0 + 8u) {
        result.value = 0;
    } else if (io_address == kSioMltSend || io_address == kJoyCnt || io_address == kJoyRecv ||
               io_address == kJoyTrans || io_address == kJoyStat) {
        result.value = 0;
    } else if (io_address == kWaitCnt) {
        result.value = waitcnt_;
    } else if (io_address == kPostFlg) {
        result.value = postflg_;
    } else if (io_address == kHaltCnt) {
        result.value = 0;
    } else if (io_address >= kMgbaLogStringLo && io_address < kMgbaLogStringHi) {
        result.value = 0;
    } else if (io_address == kMgbaLogSend) {
        result.value = 0;
    } else if (io_address == kMgbaLogEnable || io_address == (kMgbaLogEnable + 1u)) {
        if (width == BusWidth::Byte) {
            const auto shift = (io_address - kMgbaLogEnable) * 8u;
            result.value = (mgba_log_enable_ >> shift) & 0xFFu;
        } else {
            result.value = mgba_log_enable_;
        }
    } else {
        result.value = open_bus_;
        result.open_bus = true;
    }

    record_open_bus_read(result.value, width);
    return result;
}

BusAccessResult Bus::write_io(u32 address, u32 value, BusWidth width, u64 cycle_now) {
    BusAccessResult result{};
    result.value = value;
    result.cycles = 1;
    const auto io_address = width == BusWidth::Word ? align_down(address, 4u)
                                                    : (width == BusWidth::Half ? align_down(address, 2u) : address);

    if ((io_address >= kDispcnt && io_address <= kBldCnt + 4u) || io_address == kVcount) {
        ppu_.write_register(io_address, value, width);
    } else if (io_address >= 0x04000060u && io_address <= kFifoB + 2u) {
        apu_.write_register(io_address, value, width, cycle_now);
    } else if (io_address >= kDma0Sad && io_address < kDma0Sad + 48u) {
        dma_.write_register(io_address, value, width, cycle_now);
    } else if (io_address >= kTm0CntL && io_address <= kTm0CntH + 12u) {
        timers_.write_register(io_address, value, width, cycle_now);
    } else if (io_address == kKeyCnt) {
        keycnt_ = static_cast<u16>(value & 0xFFFFu);
        update_keypad_irq();
    } else if (io_address == kIe || io_address == kIf || io_address == kIme) {
        irq_.write_register(io_address, value, width);
    } else if (io_address == kSioCnt) {
        siocnt_ = static_cast<u16>(value & 0xFFFFu);
    } else if (io_address == kRcnt) {
        rcnt_ = static_cast<u16>(value & 0xFFFFu);
    } else if (io_address == kWaitCnt) {
        waitcnt_ = static_cast<u16>(value & 0xFFFFu);
        /* Update prefetch settings when wait states change */
        if (prefetch_.active) {
            const auto seq_half = prefetch_region_cycles(prefetch_.head_address, BusWidth::Half);
            prefetch_.duty = static_cast<int>(seq_half);
        }
        if (!test_bit(waitcnt_, 14u)) {
            prefetch_stop();
        }
    } else if (io_address == kPostFlg) {
        postflg_ = static_cast<u8>(value & 0x01u);
    } else if (io_address == kHaltCnt) {
        halted_ = true;
    } else if (io_address >= kMgbaLogStringLo && io_address < kMgbaLogStringHi) {
        const auto offset = io_address - kMgbaLogStringLo;
        if (width == BusWidth::Byte) {
            if (offset < debug_string_.size()) {
                debug_string_[offset] = static_cast<char>(value & 0xFFu);
            }
        } else if (width == BusWidth::Half) {
            const auto aligned = offset & ~1u;
            if (aligned < debug_string_.size()) debug_string_[aligned] = static_cast<char>(value & 0xFFu);
            if (aligned + 1 < debug_string_.size()) debug_string_[aligned + 1] = static_cast<char>((value >> 8u) & 0xFFu);
        } else {
            const auto aligned = offset & ~3u;
            if (aligned < debug_string_.size()) debug_string_[aligned] = static_cast<char>(value & 0xFFu);
            if (aligned + 1 < debug_string_.size()) debug_string_[aligned + 1] = static_cast<char>((value >> 8u) & 0xFFu);
            if (aligned + 2 < debug_string_.size()) debug_string_[aligned + 2] = static_cast<char>((value >> 16u) & 0xFFu);
            if (aligned + 3 < debug_string_.size()) debug_string_[aligned + 3] = static_cast<char>((value >> 24u) & 0xFFu);
        }
    } else if (io_address == kMgbaLogSend) {
        if (mgba_log_enable_ != 0u && (value & 0x100u) != 0u && debug_callback_) {
            debug_string_[255] = '\0';
            debug_callback_(debug_string_.data());
            debug_string_.fill('\0');
        }
    } else if (io_address == kMgbaLogEnable || io_address == (kMgbaLogEnable + 1u)) {
        u16 enable_write = static_cast<u16>(value & 0xFFFFu);
        if (width == BusWidth::Byte) {
            const auto shift = (io_address - kMgbaLogEnable) * 8u;
            enable_write = static_cast<u16>((mgba_log_enable_ & ~(0xFFu << shift)) | ((value & 0xFFu) << shift));
        }
        if (enable_write == 0xC0DEu) {
            mgba_log_enable_ = 0x1DEAu;
        }
    }

    return result;
}

}  // namespace gba
