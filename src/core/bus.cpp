#include "gba/core/bus.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

#ifdef GBA_PLATFORM_ESP32
#include "esp_heap_caps.h"
#include "esp_attr.h"
#else
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
#endif

#include "gba/core/apu.hpp"
#include "gba/core/constants.hpp"
#include "gba/core/dma.hpp"
#include "gba/core/irq.hpp"
#include "gba/core/ppu.hpp"
#include "gba/core/timers.hpp"

namespace gba {

namespace {

#ifndef GBA_TRACE_TIMERS
#define GBA_TRACE_TIMERS 0
#endif

void free_memory(u8* ptr) {
    if (!ptr) {
        return;
    }
#ifdef GBA_PLATFORM_ESP32
    heap_caps_free(ptr);
#else
    delete[] ptr;
#endif
}

	/* Allocate memory. Keep the hottest small blocks in internal SRAM on ESP32. */
u8* alloc_memory(size_t size, bool prefer_internal) {
#ifdef GBA_PLATFORM_ESP32
    auto* ptr = static_cast<u8*>(heap_caps_malloc(
        size, prefer_internal ? (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) : MALLOC_CAP_SPIRAM));
    if (ptr) {
        std::memset(ptr, 0, size);
        return ptr;
    }
    if (!prefer_internal) {
        ptr = static_cast<u8*>(heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (ptr) {
            std::memset(ptr, 0, size);
            return ptr;
        }
    }
    ptr = static_cast<u8*>(heap_caps_malloc(size, MALLOC_CAP_8BIT));
    if (ptr) {
        std::memset(ptr, 0, size);
        return ptr;
    }
    return nullptr;
#else
    (void)prefer_internal;
    return new u8[size]();
	#endif
	}

	Bus::WaitStateTables* alloc_wait_tables() {
	#ifdef GBA_PLATFORM_ESP32
	    auto* memory = heap_caps_aligned_alloc(4, sizeof(Bus::WaitStateTables),
	                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
	    if (memory) {
	        return new (memory) Bus::WaitStateTables{};
	    }
	#endif
	    return new Bus::WaitStateTables{};
	}

	void free_wait_tables(Bus::WaitStateTables* tables) {
	    if (!tables) {
	        return;
	    }
	    tables->~WaitStateTables();
	#ifdef GBA_PLATFORM_ESP32
	    heap_caps_free(tables);
	#else
	    ::operator delete(tables);
	#endif
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

[[nodiscard]] u32 sio_mode(u16 sio_cnt, u16 rcnt) {
    if ((rcnt & 0xC000u) != 0u) {
        return 0u;
    }
    return static_cast<u32>(sio_cnt & 0x3000u);
}

#if GBA_TRACE_TIMERS
struct ActiveTestInfoSnapshot {
    bool valid = false;
    u32 address = 0;
    int subtest_id = -1;
    int test_id = -1;
    int suite_id = -1;
};

[[nodiscard]] bool find_active_test_info_offset(std::span<const u8> iwram, u32& offset_out) {
    for (u32 offset = 0; offset + 8u <= iwram.size(); ++offset) {
        if (iwram[offset] == 'I' && iwram[offset + 1u] == 'n' && iwram[offset + 2u] == 'f' &&
            iwram[offset + 3u] == 'o') {
            offset_out = offset;
            return true;
        }
    }
    return false;
}

[[nodiscard]] ActiveTestInfoSnapshot read_active_test_info(std::span<const u8> iwram, u32 offset) {
    if (offset + 8u > iwram.size()) {
        return {};
    }

    const auto subtest_raw = static_cast<u16>(iwram[offset + 4u]) | (static_cast<u16>(iwram[offset + 5u]) << 8u);
    const auto test_raw = static_cast<u8>(iwram[offset + 6u]);
    const auto suite_raw = static_cast<u8>(iwram[offset + 7u]);
    return {
        true,
        0x03000000u + offset,
        static_cast<int>(static_cast<s16>(subtest_raw)),
        static_cast<int>(static_cast<s8>(test_raw)),
        static_cast<int>(static_cast<s8>(suite_raw)),
    };
}
#endif

}  // namespace

Bus::Bus(Cartridge& cartridge, Ppu& ppu, Timers& timers, DmaEngine& dma, Apu& apu, IrqController& irq)
    : irq_(irq), ppu_(ppu), apu_(apu), timers_(timers), dma_(dma),
      cartridge_(cartridge),
      ewram_(alloc_memory(kEwramSize, false), free_memory),
      iwram_(alloc_memory(kIwramSize, true), free_memory),
      palette_(alloc_memory(kPaletteSize, true), free_memory),
      vram_(alloc_memory(kVramSize, false), free_memory),
      oam_(alloc_memory(kOamSize, true), free_memory),
      wait_tables_(alloc_wait_tables(), free_wait_tables) {}

void Bus::reset() {
    if (ewram_) std::memset(ewram_.get(), 0, kEwramSize);
    if (iwram_) std::memset(iwram_.get(), 0, kIwramSize);
    if (palette_) std::memset(palette_.get(), 0, kPaletteSize);
    if (vram_) std::memset(vram_.get(), 0, kVramSize);
    if (oam_) std::memset(oam_.get(), 0, kOamSize);
    keyinput_ = 0x03FF;
    keycnt_ = 0;
    sio_multi_ = {{0u, 0u, 0u, 0u}};
    sio_data8_ = 0xFFFFu;
    sio_data32_lo_ = 0xFFFFu;
    sio_data32_hi_ = 0xFFFFu;
    sio_cnt_ = 0u;
    sio_event_cycle_ = std::numeric_limits<u64>::max();
    sio_active_ = false;
    sio_mlt_send_ = 0xFFFFu;
    rcnt_ = 0u;
    joycnt_ = 0x0040u;
    joyrecv_lo_ = 0u;
    joyrecv_hi_ = 0u;
    joytrans_lo_ = 0u;
    joytrans_hi_ = 0u;
    joystat_ = 0u;
    waitcnt_ = 0;
    postflg_ = 0;
    mgba_log_enable_ = 0;
    halted_ = false;
    bios_latch_valid_ = false;
    last_access_ = AccessType::NonSequential;
    rom_latch_valid_ = false;
    rom_latch_ = 0;
    rom_address_latch_ = 0;
    open_bus_ = 0;
#if GBA_TRACE_TIMERS
    trace_active_info_offset_ = kIwramSize;
#endif
    debug_string_.fill('\0');
    update_wait_state_table();
}

#if GBA_TRACE_TIMERS
void Bus::update_timer_trace_context() {
    const auto iwram_bytes = std::span<const u8>{iwram_.get(), kIwramSize};

    if (trace_active_info_offset_ >= kIwramSize) {
        u32 offset = 0;
        if (!find_active_test_info_offset(iwram_bytes, offset)) {
            timers_.set_trace_context(0, -1, -1, -1);
            return;
        }
        trace_active_info_offset_ = offset;
    }

    const auto snapshot = read_active_test_info(iwram_bytes, trace_active_info_offset_);
    if (!snapshot.valid) {
        timers_.set_trace_context(0, -1, -1, -1);
        trace_active_info_offset_ = kIwramSize;
        return;
    }

    timers_.set_trace_context(snapshot.address, snapshot.suite_id, snapshot.test_id, snapshot.subtest_id);
}
#endif

BusAccessResult IRAM_ATTR Bus::read(u32 address, BusWidth width, AccessType access, u64 cycle_now) {
    (void)cycle_now;
    BusAccessResult result{};

    if (address < 0x00004000u) {
        result.cycles = 1;
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
        result.cycles = width == BusWidth::Word ? 6u : 3u;
        result.value = read_array(ewram_.get(), kEwramSize, address - 0x02000000u, width);
    } else if ((address & 0x0F000000u) == 0x03000000u) {
        result.cycles = 1;
        result.value = read_array(iwram_.get(), kIwramSize, address - 0x03000000u, width);
    } else if ((address & 0x0F000000u) == 0x04000000u) {
        auto io_result = read_io(address, width, access, cycle_now);
        last_access_ = access;
        return io_result;
    } else if ((address & 0x0F000000u) == 0x05000000u) {
        result.cycles = dma_vram_cycles(width);
        result.value = read_array(palette_.get(), kPaletteSize, address - 0x05000000u, width);
    } else if ((address & 0x0F000000u) == 0x06000000u) {
        result.cycles = dma_vram_cycles(width);
        auto offset = (address - 0x06000000u) & 0x1FFFFu;
        if (offset >= 0x18000u) {
            offset = 0x10000u + (offset & 0x7FFFu);
        }
        result.value = read_array(vram_.get(), kVramSize, offset, width);
    } else if ((address & 0x0F000000u) == 0x07000000u) {
        result.cycles = 1u + (ppu_.is_video_memory_contended() ? 1u : 0u);
        result.value = read_array(oam_.get(), kOamSize, address - 0x07000000u, width);
    } else if (address >= 0x08000000u && address < 0x0E000000u) {
        const auto is_code_fetch = has_access_flag(access, AccessType::CodeFetch);
        const auto offset = address & 0x01FFFFFFu;
        const auto rom_size = cartridge_.rom_size();
        const auto aligned = align_down(offset, static_cast<u32>(width));
        const auto width_bytes = static_cast<u32>(width);
        const auto page = static_cast<std::size_t>(address >> 24u);

        auto sequential = has_access_flag(access, AccessType::Sequential);
        if ((address & 0x1FFFFu) == 0u ||
            (has_access_flag(last_access_, AccessType::Dma) && !has_access_flag(access, AccessType::Dma))) {
            sequential = false;
        }
        const auto seq_index = static_cast<std::size_t>(sequential ? 1u : 0u);

        const auto stop_prefetch_penalty = [&]() -> u32 {
            if (!prefetch_.active) {
                return 0;
            }
            const auto half_duty_plus_one = (prefetch_.duty >> 1) + 1;
            const auto countdown = prefetch_.countdown;
            if (countdown == 1 || (prefetch_.opcode_width == 4 && countdown == half_duty_plus_one)) {
                return 1;
            }
            return 0;
        };
        const auto data_prefetch_penalty = [&]() -> u32 {
            if (is_code_fetch || !prefetch_.active) {
                return 0;
            }
            if (has_access_flag(access, AccessType::Dma)) {
                return prefetch_.duty == prefetch_.opcode_width ? 1u : 0u;
            }
            const auto penalty = static_cast<u32>(wait_tables_->wait16[0][page] - wait_tables_->wait16[1][page]);
            const auto stop_penalty =
                width == BusWidth::Word && address < prefetch_.head_address ? stop_prefetch_penalty() : 0u;
            return penalty + stop_penalty;
        };

        if (is_code_fetch && prefetch_.active && prefetch_.count != 0 &&
            address == prefetch_.head_address && prefetch_.opcode_width == static_cast<int>(width_bytes)) {
            result.cycles = 1;
            result.value = aligned + width_bytes <= rom_size
                ? cartridge_.read_rom(offset, width)
                : gamepak_open_bus_value(address, width);
            prefetch_.count--;
            prefetch_.head_address += width_bytes;
            prefetch_advance(1);
        } else if (is_code_fetch && prefetch_.active && prefetch_.countdown > 0 &&
                   address == prefetch_.last_address &&
                   prefetch_.opcode_width == static_cast<int>(width_bytes)) {
            result.cycles = static_cast<u32>(prefetch_.countdown);
            result.value = aligned + width_bytes <= rom_size
                ? cartridge_.read_rom(offset, width)
                : gamepak_open_bus_value(address, width);
            prefetch_advance(prefetch_.countdown);
            prefetch_.head_address = prefetch_.last_address;
            prefetch_.count = 0;
        } else {
            const auto penalty = is_code_fetch ? stop_prefetch_penalty() : data_prefetch_penalty();
            result.cycles = width == BusWidth::Word ? wait_tables_->wait32[seq_index][page] : wait_tables_->wait16[seq_index][page];
            prefetch_stop();

            if (is_code_fetch && prefetch_.was_disabled) {
                result.cycles = width == BusWidth::Word ? wait_tables_->wait32[0][page] : wait_tables_->wait16[0][page];
                prefetch_.was_disabled = false;
            }
            result.cycles += penalty;

            if (has_access_flag(access, AccessType::Dma)) {
                result.value = read_gamepak_rom(address, width, sequential);
                if (!is_code_fetch) {
                    rom_latch_ = expand_bus_latch(result.value, width);
                    rom_latch_valid_ = true;
                }
            } else if (aligned + width_bytes <= rom_size) {
                result.value = cartridge_.read_rom(offset, width);
                if (!is_code_fetch) {
                    const auto word_base = align_down(aligned, 4u);
                    if (word_base + 4u <= rom_size) {
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

            if (is_code_fetch && test_bit(waitcnt_, 14u)) {
                prefetch_.active = true;
                prefetch_.count = 0;
                prefetch_.opcode_width = static_cast<int>(width_bytes);
                prefetch_.capacity = width == BusWidth::Word ? 4 : 8;
                prefetch_.duty = width == BusWidth::Word ? wait_tables_->wait32[1][page] : wait_tables_->wait16[1][page];
                prefetch_.countdown = prefetch_.duty;
                prefetch_.last_address = address + width_bytes;
                prefetch_.head_address = prefetch_.last_address;
            }
        }

        const auto next_offset = (offset + width_bytes) & 0x01FFFFFFu;
        if (sequential && (next_offset & 0x7FFFu) >= 0x7F00u) {
            cartridge_.prefetch_rom((next_offset + 0x100u) & ~0x7FFFu, 32u * 1024u);
        }
    } else if (address >= 0x0E000000u && address < 0x10000000u) {
        result.breaks_fetch_burst = true;
        result.cycles = wait_tables_->wait16[0][0xEu];
        if (prefetch_.active) {
            result.cycles += ((prefetch_.countdown == 1 ||
                               (prefetch_.opcode_width == 4 && prefetch_.countdown == ((prefetch_.duty >> 1) + 1)))
                                  ? 1u
                                  : 0u);
        }
        result.value = cartridge_.read_save(address - 0x0E000000u, width);
        prefetch_stop();
    } else {
        result.cycles = 1;
        result.value = open_bus_;
        result.open_bus = true;
        result.dma_open_bus = has_access_flag(last_access_, AccessType::Dma);
    }

    record_open_bus_read(result.value, width);
    last_access_ = access;
    return result;
}

BusAccessResult IRAM_ATTR Bus::write(u32 address, u32 value, BusWidth width, AccessType access, u64 cycle_now) {
    BusAccessResult result{};
    result.value = value;
    result.cycles = 1;

    if ((address & 0x0F000000u) == 0x02000000u) {
        result.cycles = width == BusWidth::Word ? 6u : 3u;
        write_array(ewram_.get(), kEwramSize, address - 0x02000000u, value, width);
    } else if ((address & 0x0F000000u) == 0x03000000u) {
        result.cycles = 1;
        write_array(iwram_.get(), kIwramSize, address - 0x03000000u, value, width);
    } else if ((address & 0x0F000000u) == 0x04000000u) {
        auto io_result = write_io(address, value, width, cycle_now);
        last_access_ = access;
        return io_result;
    } else if ((address & 0x0F000000u) == 0x05000000u) {
        result.cycles = dma_vram_cycles(width);
        if (width == BusWidth::Byte) {
            const auto aligned = align_down(address - 0x05000000u, 2u);
            const auto replicated = static_cast<u16>((value & 0xFFu) * 0x0101u);
            write_array(palette_.get(), kPaletteSize, aligned, replicated, BusWidth::Half);
        } else {
            write_array(palette_.get(), kPaletteSize, address - 0x05000000u, value, width);
        }
        mark_video_dirty();
    } else if ((address & 0x0F000000u) == 0x06000000u) {
        result.cycles = dma_vram_cycles(width);
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
            write_array(vram_.get(), kVramSize, aligned, replicated, BusWidth::Half);
        } else {
            write_array(vram_.get(), kVramSize, offset, value, width);
        }
        mark_video_dirty();
    } else if ((address & 0x0F000000u) == 0x07000000u) {
        result.cycles = 1u + (ppu_.is_video_memory_contended() ? 1u : 0u);
        if (width != BusWidth::Byte) {
            write_array(oam_.get(), kOamSize, address - 0x07000000u, value, width);
            mark_video_dirty();
        }
    } else if (address >= 0x08000000u && address < 0x0E000000u) {
        result.breaks_fetch_burst = true;
        const auto page = static_cast<std::size_t>(address >> 24u);
        auto sequential = has_access_flag(access, AccessType::Sequential);
        if ((address & 0x1FFFFu) == 0u ||
            (has_access_flag(last_access_, AccessType::Dma) && !has_access_flag(access, AccessType::Dma))) {
            sequential = false;
        }
        const auto seq_index = static_cast<std::size_t>(sequential ? 1u : 0u);

        result.cycles = width == BusWidth::Word ? wait_tables_->wait32[seq_index][page] : wait_tables_->wait16[seq_index][page];
        if (prefetch_.active) {
            if (has_access_flag(access, AccessType::Dma)) {
                result.cycles += prefetch_.duty > prefetch_.opcode_width ? 1u : 0u;
            } else {
                const auto half_duty_plus_one = (prefetch_.duty >> 1) + 1;
                if (prefetch_.countdown == 1 ||
                    (prefetch_.opcode_width == 4 && prefetch_.countdown == half_duty_plus_one)) {
                    result.cycles += 1;
                }
            }
        }
        prefetch_stop();
        cartridge_.write_rom(address & 0x01FFFFFFu, value, width);
    } else if (address >= 0x0E000000u && address < 0x10000000u) {
        result.breaks_fetch_burst = true;
        result.cycles = wait_tables_->wait16[0][0xEu];
        if (prefetch_.active) {
            const auto half_duty_plus_one = (prefetch_.duty >> 1) + 1;
            if (prefetch_.countdown == 1 ||
                (prefetch_.opcode_width == 4 && prefetch_.countdown == half_duty_plus_one)) {
                result.cycles += 1;
            }
        }
        cartridge_.write_save(address - 0x0E000000u, value, width);
        prefetch_stop();
    }

    if (!has_access_flag(access, AccessType::Dma) && (address & 0x0F000000u) == 0x04000000u) {
        const auto dma_cycle = cycle_now + result.cycles;
        result.cycles += service_dma(dma_cycle);
    }

    last_access_ = access;
    return result;
}

std::span<const u8> Bus::vram() const {
    return {vram_.get(), kVramSize};
}

std::span<u8> Bus::vram_write() {
    return {vram_.get(), kVramSize};
}

void Bus::mark_video_dirty() {
    ppu_.mark_all_dirty();
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

void Bus::set_rom(std::span<const u8> rom) {
    cartridge_.set_rom(std::vector<u8>(rom.begin(), rom.end()));
}

std::span<const u8> Bus::rom() const {
    return cartridge_.rom();
}

std::size_t Bus::rom_size() const {
    return cartridge_.rom_size();
}

bool Bus::halted() const {
    return halted_;
}

void Bus::clear_halt() {
    halted_ = false;
}

u64 Bus::next_event_cycle() const {
    return sio_active_ ? sio_event_cycle_ : std::numeric_limits<u64>::max();
}

void IRAM_ATTR Bus::service_timers(u64 cycle_now) {
    timers_.advance_to(cycle_now, irq_, apu_);
    if (sio_active_ && cycle_now >= sio_event_cycle_) {
        sio_active_ = false;
        sio_event_cycle_ = std::numeric_limits<u64>::max();
        sio_cnt_ = static_cast<u16>(sio_cnt_ & ~0x0080u);
        if (test_bit(sio_cnt_, 14u)) {
            irq_.request(IrqSerial);
        }
    }
    if (apu_.take_fifo_request_a()) {
        dma_.request_fifo_a(cycle_now);
    }
    if (apu_.take_fifo_request_b()) {
        dma_.request_fifo_b(cycle_now);
    }
}

u32 Bus::read_gamepak_rom(u32 address, BusWidth width, bool sequential) {
    const auto step = width == BusWidth::Word ? 4u : 2u;
    const auto align_mask = step - 1u;
    if (!sequential) {
        rom_address_latch_ = (address & 0x01FFFFFFu) & ~align_mask;
    }

    const auto read_offset = rom_address_latch_;
    const auto full_latch_address = 0x08000000u + read_offset;
    const auto rom_size = cartridge_.rom_size();

    u32 value = 0;
    if (width == BusWidth::Word) {
        value = read_offset + 4u <= rom_size
            ? cartridge_.read_rom(read_offset, BusWidth::Word)
            : gamepak_open_bus_value(full_latch_address, BusWidth::Word);
    } else {
        const auto half = read_offset + 2u <= rom_size
            ? cartridge_.read_rom(read_offset, BusWidth::Half)
            : gamepak_open_bus_value(full_latch_address, BusWidth::Half);
        value = width == BusWidth::Byte ? ((half >> ((address & 1u) * 8u)) & 0xFFu) : half;
    }

    rom_address_latch_ = (rom_address_latch_ + step) & 0x01FFFFFFu;
    if (sequential && (rom_address_latch_ & 0x7FFFu) >= 0x7F00u) {
        cartridge_.prefetch_rom((rom_address_latch_ + 0x100u) & ~0x7FFFu, 32u * 1024u);
    }
    return value;
}

void Bus::prefetch_stop() {
    prefetch_.active = false;
    prefetch_.count = 0;
    prefetch_.countdown = 0;
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
    if (dma_.next_event_cycle() > cycle_now || !dma_.has_pending_transfer()) {
        return 0;
    }
    return dma_.service_due(cycle_now, *this, irq_);
}

u64 Bus::dma_next_event_cycle() const {
    return dma_.next_event_cycle();
}

u32 Bus::dma_vram_cycles(BusWidth width) const {
    const auto base = width == BusWidth::Word ? 2u : 1u;
    return base + (ppu_.is_video_memory_contended() ? 1u : 0u);
}

u32 Bus::dma_rom_cycles(u32 address, bool is_word, bool sequential) const {
    const auto page = static_cast<std::size_t>(address >> 24u);
    if (is_word) {
        return sequential ? wait_tables_->wait32[1][page] : wait_tables_->wait32[0][page];
    }
    return sequential ? wait_tables_->wait16[1][page] : wait_tables_->wait16[0][page];
}

u32 Bus::prefetch_region_cycles(u32 address, BusWidth width) const {
    const auto page = static_cast<std::size_t>(address >> 24u);
    return width == BusWidth::Word ? wait_tables_->wait32[1][page] : wait_tables_->wait16[1][page];
}

void Bus::update_wait_state_table() {
    static constexpr std::array<u8, 4> nseq{5u, 4u, 3u, 9u};
    static constexpr std::array<u8, 2> seq0{3u, 2u};
    static constexpr std::array<u8, 2> seq1{5u, 2u};
    static constexpr std::array<u8, 2> seq2{9u, 2u};

    const auto sram = nseq[waitcnt_ & 0x3u];
    const auto ws0_n = (waitcnt_ >> 2u) & 0x3u;
    const auto ws0_s = (waitcnt_ >> 4u) & 0x1u;
    const auto ws1_n = (waitcnt_ >> 5u) & 0x3u;
    const auto ws1_s = (waitcnt_ >> 7u) & 0x1u;
    const auto ws2_n = (waitcnt_ >> 8u) & 0x3u;
    const auto ws2_s = (waitcnt_ >> 10u) & 0x1u;

    for (std::size_t mirror = 0; mirror < 2; ++mirror) {
        const auto ws0_page = 0x8u + mirror;
        const auto ws1_page = 0xAu + mirror;
        const auto ws2_page = 0xCu + mirror;
        const auto sram_page = 0xEu + mirror;

        wait_tables_->wait16[0][ws0_page] = nseq[ws0_n];
        wait_tables_->wait16[0][ws1_page] = nseq[ws1_n];
        wait_tables_->wait16[0][ws2_page] = nseq[ws2_n];

        wait_tables_->wait16[1][ws0_page] = seq0[ws0_s];
        wait_tables_->wait16[1][ws1_page] = seq1[ws1_s];
        wait_tables_->wait16[1][ws2_page] = seq2[ws2_s];

        wait_tables_->wait32[0][ws0_page] = static_cast<u8>(wait_tables_->wait16[0][ws0_page] + wait_tables_->wait16[1][ws0_page]);
        wait_tables_->wait32[0][ws1_page] = static_cast<u8>(wait_tables_->wait16[0][ws1_page] + wait_tables_->wait16[1][ws1_page]);
        wait_tables_->wait32[0][ws2_page] = static_cast<u8>(wait_tables_->wait16[0][ws2_page] + wait_tables_->wait16[1][ws2_page]);

        wait_tables_->wait32[1][ws0_page] = static_cast<u8>(wait_tables_->wait16[1][ws0_page] * 2u);
        wait_tables_->wait32[1][ws1_page] = static_cast<u8>(wait_tables_->wait16[1][ws1_page] * 2u);
        wait_tables_->wait32[1][ws2_page] = static_cast<u8>(wait_tables_->wait16[1][ws2_page] * 2u);

        wait_tables_->wait16[0][sram_page] = sram;
        wait_tables_->wait16[1][sram_page] = sram;
        wait_tables_->wait32[0][sram_page] = sram;
        wait_tables_->wait32[1][sram_page] = sram;
    }
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

u32 IRAM_ATTR Bus::read_array(const u8* bytes, u32 size, u32 address, BusWidth width) {
    if (bytes == nullptr || size == 0) {
        return 0xFFFFFFFFu;
    }

    const auto wrap = [size](u32 offset) -> u32 {
        return (size & (size - 1u)) == 0u ? (offset & (size - 1u)) : (offset % size);
    };
    const auto normalized = wrap(address);
    const auto get = [&](u32 offset) { return bytes[wrap(offset)]; };

    switch (width) {
    case BusWidth::Byte:
        return get(normalized);
    case BusWidth::Half: {
        const auto aligned = align_down(normalized, 2u);
        if (aligned + 2u <= size) {
            u16 value = 0;
            std::memcpy(&value, bytes + aligned, sizeof(value));
            return value;
        }
        return static_cast<u32>(get(aligned)) | (static_cast<u32>(get(aligned + 1u)) << 8u);
    }
    case BusWidth::Word: {
        const auto aligned = align_down(normalized, 4u);
        if (aligned + 4u <= size) {
            u32 value = 0;
            std::memcpy(&value, bytes + aligned, sizeof(value));
            return value;
        }
        return static_cast<u32>(get(aligned)) | (static_cast<u32>(get(aligned + 1u)) << 8u) |
               (static_cast<u32>(get(aligned + 2u)) << 16u) | (static_cast<u32>(get(aligned + 3u)) << 24u);
    }
    }
    return 0xFFFFFFFFu;
}

void IRAM_ATTR Bus::write_array(u8* bytes, u32 size, u32 address, u32 value, BusWidth width) {
    if (bytes == nullptr || size == 0) {
        return;
    }

    const auto wrap = [size](u32 offset) -> u32 {
        return (size & (size - 1u)) == 0u ? (offset & (size - 1u)) : (offset % size);
    };
    const auto put = [&](u32 offset, u8 byte) { bytes[wrap(offset)] = byte; };
    switch (width) {
    case BusWidth::Byte:
        put(address, static_cast<u8>(value));
        break;
    case BusWidth::Half: {
        const auto aligned = align_down(wrap(address), 2u);
        if (aligned + 2u <= size) {
            const auto half = static_cast<u16>(value & 0xFFFFu);
            std::memcpy(bytes + aligned, &half, sizeof(half));
            break;
        }
        put(aligned, static_cast<u8>(value & 0xFFu));
        put(aligned + 1u, static_cast<u8>((value >> 8u) & 0xFFu));
        break;
    }
    case BusWidth::Word: {
        const auto aligned = align_down(wrap(address), 4u);
        if (aligned + 4u <= size) {
            std::memcpy(bytes + aligned, &value, sizeof(value));
            break;
        }
        put(aligned, static_cast<u8>(value & 0xFFu));
        put(aligned + 1u, static_cast<u8>((value >> 8u) & 0xFFu));
        put(aligned + 2u, static_cast<u8>((value >> 16u) & 0xFFu));
        put(aligned + 3u, static_cast<u8>((value >> 24u) & 0xFFu));
        break;
    }
    }
}

u32 Bus::peek_word(u32 address) const {
    const auto peek_byte = [&](u32 byte_address) -> u8 {
        if (byte_address < 0x00004000u) {
            return static_cast<u8>(cartridge_.read_bios(byte_address, BusWidth::Byte) & 0xFFu);
        }
        if ((byte_address & 0x0F000000u) == 0x02000000u) {
            return static_cast<u8>(read_array(ewram_.get(), kEwramSize, byte_address - 0x02000000u, BusWidth::Byte) & 0xFFu);
        }
        if ((byte_address & 0x0F000000u) == 0x03000000u) {
            return static_cast<u8>(read_array(iwram_.get(), kIwramSize, byte_address - 0x03000000u, BusWidth::Byte) & 0xFFu);
        }
        if ((byte_address & 0x0F000000u) == 0x05000000u) {
            return static_cast<u8>(read_array(palette_.get(), kPaletteSize, byte_address - 0x05000000u, BusWidth::Byte) & 0xFFu);
        }
        if ((byte_address & 0x0F000000u) == 0x06000000u) {
            auto offset = (byte_address - 0x06000000u) & 0x1FFFFu;
            if (offset >= 0x18000u) {
                offset = 0x10000u + (offset & 0x7FFFu);
            }
            return static_cast<u8>(read_array(vram_.get(), kVramSize, offset, BusWidth::Byte) & 0xFFu);
        }
        if ((byte_address & 0x0F000000u) == 0x07000000u) {
            return static_cast<u8>(read_array(oam_.get(), kOamSize, byte_address - 0x07000000u, BusWidth::Byte) & 0xFFu);
        }
        if (byte_address >= 0x08000000u && byte_address < 0x0E000000u) {
            return static_cast<u8>(cartridge_.read_rom(byte_address & 0x01FFFFFFu, BusWidth::Byte) & 0xFFu);
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
    case 0x04000010u: case 0x04000012u: case 0x04000014u: case 0x04000016u:
    case 0x04000018u: case 0x0400001Au: case 0x0400001Cu: case 0x0400001Eu:
    case 0x04000020u: case 0x04000022u: case 0x04000024u: case 0x04000026u:
    case 0x04000028u: case 0x0400002Au: case 0x0400002Cu: case 0x0400002Eu:
    case 0x04000030u: case 0x04000032u: case 0x04000034u: case 0x04000036u:
    case 0x04000038u: case 0x0400003Au: case 0x0400003Cu: case 0x0400003Eu:
    case 0x04000040u: case 0x04000042u: case 0x04000044u: case 0x04000046u:
    case 0x0400004Cu: case 0x04000054u:
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
    case 0x0400004Eu: case 0x04000056u: case 0x04000058u: case 0x0400005Au:
    case 0x0400005Cu: case 0x0400005Eu:
    case 0x0400008Cu: case 0x0400008Eu:
    case kFifoA: case kFifoA + 2u:
    case kFifoB: case kFifoB + 2u:
    case 0x040000A8u: case 0x040000AAu: case 0x040000ACu: case 0x040000AEu:
    case kDma0Sad: case kDma0Sad + 2u:
    case kDma0Dad: case kDma0Dad + 2u:
    case kDma0Sad + 12u: case kDma0Sad + 14u:
    case kDma0Dad + 12u: case kDma0Dad + 14u:
    case kDma0Sad + 24u: case kDma0Sad + 26u:
    case kDma0Dad + 24u: case kDma0Dad + 26u:
    case kDma0Sad + 36u: case kDma0Sad + 38u:
    case kDma0Dad + 36u: case kDma0Dad + 38u:
    case 0x040000E0u: case 0x040000E2u: case 0x040000E4u: case 0x040000E6u:
    case 0x040000E8u: case 0x040000EAu: case 0x040000ECu: case 0x040000EEu:
    case 0x040000F0u: case 0x040000F2u: case 0x040000F4u: case 0x040000F6u:
    case 0x040000F8u: case 0x040000FAu: case 0x040000FCu: case 0x040000FEu:
    case 0x0400100Cu:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] bool is_zero_read_io_halfword(u32 address) {
    switch (address) {
    case 0x04000136u: case 0x04000142u: case 0x0400015Au:
    case 0x04000206u: case 0x0400020Au: case 0x04000302u:
        return true;
    default:
        return false;
    }
}

}  // namespace

BusAccessResult Bus::read_io(u32 address, BusWidth width, AccessType access, u64 cycle_now) {
    (void)access;
    BusAccessResult result{};
    result.cycles = 1;
    const auto io_address = width == BusWidth::Word ? align_down(address, 4u)
                                                    : (width == BusWidth::Half ? align_down(address, 2u) : address);
    const auto half_to_check = width == BusWidth::Byte ? align_down(io_address, 2u) : io_address;

    if ((width == BusWidth::Half || width == BusWidth::Byte) && is_open_bus_io_halfword(half_to_check)) {
        result.value = narrow_open_bus(open_bus_, width, io_address);
        result.open_bus = true;
        result.dma_open_bus = has_access_flag(last_access_, AccessType::Dma);
        record_open_bus_read(result.value, width);
        return result;
    }
    if ((width == BusWidth::Half || width == BusWidth::Byte) && is_zero_read_io_halfword(half_to_check)) {
        result.value = 0;
        record_open_bus_read(result.value, width);
        return result;
    }

    if (sio_active_ && cycle_now >= sio_event_cycle_) {
        sio_active_ = false;
        sio_event_cycle_ = std::numeric_limits<u64>::max();
        sio_cnt_ = static_cast<u16>(sio_cnt_ & ~0x0080u);
        if (test_bit(sio_cnt_, 14u)) {
            irq_.request(IrqSerial);
        }
    }

    if ((io_address >= kDispcnt && io_address <= kBldCnt + 4u) || io_address == kVcount) {
        result.value = ppu_.read_register(io_address, width);
    } else if (io_address >= 0x04000060u && io_address <= kFifoB + 2u) {
        result.value = apu_.read_register(io_address, width);
        if (io_address == kSoundCntX) {
            result.value &= 0x0080u;
        }
    } else if ((io_address >= kDma0Sad && io_address < kDma0Sad + 48u)) {
        result.value = dma_.read_register(io_address, width);
    } else if (io_address >= kTm0CntL && io_address <= kTm0CntH + 12u) {
        if (width == BusWidth::Word) {
            service_timers(cycle_now);
#if GBA_TRACE_TIMERS
            update_timer_trace_context();
#endif
            result.value = timers_.read_word_register_split(io_address, cycle_now, cycle_now + 1u);
        } else {
            service_timers(cycle_now);
#if GBA_TRACE_TIMERS
            update_timer_trace_context();
#endif
            result.value = timers_.read_register(io_address, width, cycle_now);
        }
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
    } else if (io_address >= kSioMulti0 && io_address <= kJoyStat + 2u) {
        const auto read_half = [&](u32 half_address) -> u16 {
            const auto serial_mode = sio_mode(sio_cnt_, rcnt_);
            switch (half_address) {
            case kSioMulti0:
                return serial_mode == 0x1000u ? sio_data32_lo_ : 0u;
            case kSioMulti0 + 2u:
                return serial_mode == 0x1000u ? sio_data32_hi_ : 0u;
            case kSioMulti0 + 4u:
            case kSioMulti0 + 6u:
                return 0u;
            case kSioCnt:
                return static_cast<u16>(sio_cnt_ & ((serial_mode == 0x3000u) ? 0x7FAFu : 0x7F8Fu));
            case kSioMltSend:
                return serial_mode == 0x3000u ? 0u : sio_mlt_send_;
            case kRcnt: {
                const auto rcnt_mode = static_cast<u16>(rcnt_ & 0xC000u);
                if (rcnt_mode == 0x8000u) {
                    return static_cast<u16>(rcnt_mode | 0x01FFu);
                }
                if (rcnt_mode == 0xC000u) {
                    return static_cast<u16>(rcnt_mode | 0x01FCu);
                }
                if (serial_mode == 0x0000u || serial_mode == 0x1000u) {
                    return 0x01F5u;
                }
                return 0x01FFu;
            }
            case kJoyCnt:
                return static_cast<u16>(joycnt_ & 0x0040u);
            case kJoyRecv:
            case kJoyRecv + 2u:
            case kJoyTrans:
            case kJoyTrans + 2u:
            case kJoyStat:
            case kJoyStat + 2u:
                return 0u;
            default:
                return 0u;
            }
        };
        if (width == BusWidth::Byte) {
            const auto aligned = align_down(io_address, 2u);
            const auto half = read_half(aligned);
            const auto shift = (io_address & 1u) * 8u;
            result.value = (half >> shift) & 0xFFu;
        } else if (width == BusWidth::Half) {
            result.value = read_half(io_address);
        } else {
            result.value = static_cast<u32>(read_half(io_address)) |
                           (static_cast<u32>(read_half(io_address + 2u)) << 16u);
        }
    } else if (io_address == kWaitCnt) {
        result.value = waitcnt_;
    } else if (io_address == kPostFlg) {
        result.value = postflg_;
    } else if (io_address == kMgbaLogEnable) {
        result.value = static_cast<u32>(mgba_log_enable_);
    } else if (io_address == 0x04000206u || io_address == 0x0400020Au || io_address == 0x04000302u) {
        result.value = 0u;
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
        service_timers(cycle_now);
        timers_.write_register(io_address, value, width, cycle_now, irq_, apu_);
    } else if (io_address == kKeyCnt) {
        keycnt_ = static_cast<u16>(value & 0xFFFFu);
        update_keypad_irq();
    } else if (io_address == kIe || io_address == kIf || io_address == kIme) {
        irq_.write_register(io_address, value, width, cycle_now);
    } else if (io_address >= kSioMulti0 && io_address <= kJoyStat + 2u) {
        const auto write_half = [&](u32 half_address, u16 half_value) {
            switch (half_address) {
            case kSioMulti0:
                sio_data32_lo_ = half_value;
                break;
            case kSioMulti0 + 2u:
                sio_data32_hi_ = half_value;
                break;
            case kSioMulti0 + 4u:
            case kSioMulti0 + 6u:
                break;
            case kSioCnt:
                sio_cnt_ = static_cast<u16>(half_value & 0x7FAFu);
                if (test_bit(sio_cnt_, 7u)) {
                    const auto serial_mode = sio_mode(sio_cnt_, rcnt_);
                    const auto speed = static_cast<u16>(sio_cnt_ & 0x0003u);
                    if ((serial_mode == 0x0000u || serial_mode == 0x1000u) &&
                        (speed == 1u || speed == 3u)) {
                        const auto bits = serial_mode == 0x1000u ? 32u : 8u;
                        static constexpr u64 kSioCycles[2][2] = {
                            {524u, 2097u},
                            {67u, 268u},
                        };
                        const auto spd = static_cast<std::size_t>(speed == 3u ? 1u : 0u);
                        const auto nbits = static_cast<std::size_t>(bits == 32u ? 1u : 0u);
                        sio_active_ = true;
                        sio_event_cycle_ = cycle_now + kSioCycles[spd][nbits];
                    } else {
                        sio_active_ = false;
                        sio_event_cycle_ = std::numeric_limits<u64>::max();
                    }
                } else {
                    sio_active_ = false;
                    sio_event_cycle_ = std::numeric_limits<u64>::max();
                }
                break;
            case kSioMltSend:
                sio_mlt_send_ = half_value;
                break;
            case kRcnt:
                rcnt_ = static_cast<u16>(half_value & 0xC3FFu);
                break;
            case kJoyCnt: joycnt_ = static_cast<u16>(half_value & 0x0040u); break;
            case kJoyRecv:
            case kJoyRecv + 2u:
            case kJoyTrans:
            case kJoyTrans + 2u:
            case kJoyStat:
                break;
            default:
                break;
            }
        };
        if (width == BusWidth::Byte) {
            const auto aligned = align_down(io_address, 2u);
            const auto shift = (io_address & 1u) * 8u;
            const auto current = static_cast<u16>(read_io(aligned, BusWidth::Half, AccessType::Io, cycle_now).value);
            const auto merged = static_cast<u16>((current & ~(0xFFu << shift)) | ((value & 0xFFu) << shift));
            write_half(aligned, merged);
        } else if (width == BusWidth::Half) {
            write_half(io_address, static_cast<u16>(value & 0xFFFFu));
        } else {
            write_half(io_address, static_cast<u16>(value & 0xFFFFu));
            write_half(io_address + 2u, static_cast<u16>((value >> 16u) & 0xFFFFu));
        }
    } else if (io_address == kWaitCnt) {
        const auto prefetch_was_enabled = test_bit(waitcnt_, 14u);
        waitcnt_ = static_cast<u16>(value & 0xFFFFu);
        update_wait_state_table();
        if (prefetch_.active) {
            prefetch_.duty = static_cast<int>(prefetch_region_cycles(
                prefetch_.head_address, prefetch_.opcode_width == 4 ? BusWidth::Word : BusWidth::Half));
        }
        if (prefetch_was_enabled && !test_bit(waitcnt_, 14u)) {
            prefetch_.was_disabled = true;
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
        // Write all bytes according to bus width
        if (width == BusWidth::Byte) {
            if (offset < debug_string_.size()) debug_string_[offset] = static_cast<char>(value & 0xFFu);
        } else if (width == BusWidth::Half) {
            if (offset < debug_string_.size()) debug_string_[offset] = static_cast<char>(value & 0xFFu);
            if (offset + 1u < debug_string_.size()) debug_string_[offset + 1u] = static_cast<char>((value >> 8u) & 0xFFu);
        } else {
            for (u32 i = 0; i < 4u; ++i) {
                if (offset + i < debug_string_.size()) debug_string_[offset + i] = static_cast<char>((value >> (i * 8u)) & 0xFFu);
            }
        }
    } else if (io_address == kMgbaLogSend) {
        if (mgba_log_enable_ != 0u && (value & 0x100u) != 0u && debug_callback_) {
            debug_callback_(debug_string_.data());
            debug_string_.fill('\0');
        }
    } else if (io_address == kMgbaLogEnable || io_address == (kMgbaLogEnable + 1u)) {
        if (value == 0xC0DEu) mgba_log_enable_ = 0x1DEAu;
    }
    return result;
}

}  // namespace gba
