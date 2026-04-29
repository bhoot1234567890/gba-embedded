#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>

#include "gba/core/cartridge.hpp"
#include "gba/core/constants.hpp"
#include "gba/core/types.hpp"

namespace gba {

#ifndef GBA_TRACE_TIMERS
#define GBA_TRACE_TIMERS 0
#endif

using DebugOutputCallback = std::function<void(const char*)>;

class Apu;
class DmaEngine;
class IrqController;
class Ppu;
class Timers;

class Bus {
public:
    struct WaitStateTables {
        std::array<std::array<u8, 16>, 2> wait16{};
        std::array<std::array<u8, 16>, 2> wait32{};
    };

    Bus(Cartridge& cartridge, Ppu& ppu, Timers& timers, DmaEngine& dma, Apu& apu, IrqController& irq);

    void reset();

    [[nodiscard]] BusAccessResult read(u32 address, BusWidth width, AccessType access, u64 cycle_now);
    [[nodiscard]] BusAccessResult write(u32 address, u32 value, BusWidth width, AccessType access, u64 cycle_now);

    [[nodiscard]] std::span<u8> ewram();
    [[nodiscard]] std::span<u8> iwram();
    [[nodiscard]] std::span<const u8> vram() const;
    [[nodiscard]] std::span<u8> vram_write();
    [[nodiscard]] std::span<const u8> palette() const;
    [[nodiscard]] std::span<const u8> oam() const;
    [[nodiscard]] u8* ewram_data() { return ewram_.get(); }
    [[nodiscard]] const u8* ewram_data() const { return ewram_.get(); }
    [[nodiscard]] u8* iwram_data() { return iwram_.get(); }
    [[nodiscard]] const u8* iwram_data() const { return iwram_.get(); }
    [[nodiscard]] u8* vram_data() { return vram_.get(); }
    [[nodiscard]] const u8* vram_data() const { return vram_.get(); }

    void set_rom(std::span<const u8> rom);
    [[nodiscard]] std::span<const u8> rom() const;
    [[nodiscard]] std::size_t rom_size() const;

    void set_keyinput(u16 value);
    void set_debug_output(DebugOutputCallback callback);
    [[nodiscard]] u16 keyinput() const;
    [[nodiscard]] u16 keycnt() const;
    [[nodiscard]] u16 waitcnt() const;
    void mark_video_dirty();

    [[nodiscard]] u32 peek_word(u32 address) const;

    [[nodiscard]] bool has_bios() const;
    [[nodiscard]] bool halted() const;
    void clear_halt();
    void service_timers(u64 cycle_now);
    [[nodiscard]] u32 service_dma(u64 cycle_now);
    [[nodiscard]] u64 dma_next_event_cycle() const;
    [[nodiscard]] u32 dma_vram_cycles(BusWidth width) const;
    void prefetch_advance(int cycles);
    [[nodiscard]] u32 dma_rom_cycles(u32 address, bool is_word, bool sequential) const;
    [[nodiscard]] u64 next_event_cycle() const;

private:
    [[nodiscard]] static u32 read_array(const u8* bytes, u32 size, u32 address, BusWidth width);
    static void write_array(u8* bytes, u32 size, u32 address, u32 value, BusWidth width);
    [[nodiscard]] static u32 expand_bus_latch(u32 value, BusWidth width);
    void record_open_bus_read(u32 value, BusWidth width);
    void update_keypad_irq();
    void update_wait_state_table();
    [[nodiscard]] u32 prefetch_region_cycles(u32 address, BusWidth width) const;
    [[nodiscard]] u32 read_gamepak_rom(u32 address, BusWidth width, bool sequential);
    [[nodiscard]] BusAccessResult read_io(u32 address, BusWidth width, AccessType access, u64 cycle_now);
    [[nodiscard]] BusAccessResult write_io(u32 address, u32 value, BusWidth width, u64 cycle_now);
#if GBA_TRACE_TIMERS
    void update_timer_trace_context();
#endif

    IrqController& irq_;
    Ppu& ppu_;
    Apu& apu_;
    Timers& timers_;
    DmaEngine& dma_;

    Cartridge& cartridge_;

    /* Heap-allocated memory arrays — use PSRAM on ESP32 when available */
    std::unique_ptr<u8, void (*)(u8*)> ewram_;
    std::unique_ptr<u8, void (*)(u8*)> iwram_;
    std::unique_ptr<u8, void (*)(u8*)> palette_;
    std::unique_ptr<u8, void (*)(u8*)> vram_;
    std::unique_ptr<u8, void (*)(u8*)> oam_;

    std::unique_ptr<WaitStateTables, void (*)(WaitStateTables*)> wait_tables_;

    struct PrefetchState {
        bool active = false;
        u32 head_address = 0;
        u32 last_address = 0;
        int count = 0;
        int countdown = 0;
        int duty = 0;
        int capacity = 0;
        int opcode_width = 2;
        bool was_disabled = false;
    } prefetch_;

    void prefetch_stop();

    u16 keyinput_ = 0x03FF;
    u16 keycnt_ = 0;
    std::array<u16, 4> sio_multi_{{0u, 0u, 0u, 0u}};
    u16 sio_data8_ = 0xFFFFu;
    u16 sio_data32_lo_ = 0u;
    u16 sio_data32_hi_ = 0u;
    u16 sio_cnt_ = 0u;
    u64 sio_event_cycle_ = 0;
    bool sio_active_ = false;
    u16 sio_mlt_send_ = 0xFFFFu;
    u16 rcnt_ = 0u;
    u16 joycnt_ = 0x0040u;
    u16 joyrecv_lo_ = 0u;
    u16 joyrecv_hi_ = 0u;
    u16 joytrans_lo_ = 0u;
    u16 joytrans_hi_ = 0u;
    u16 joystat_ = 0u;
    u16 waitcnt_ = 0;
    u16 mgba_log_enable_ = 0;
    u8 postflg_ = 0;
    bool halted_ = false;
    bool bios_latch_valid_ = false;

    AccessType last_access_ = AccessType::NonSequential;
    u32 rom_address_latch_ = 0;
    u32 rom_latch_ = 0;
    bool rom_latch_valid_ = false;
    u32 bios_latch_ = 0;
    u32 open_bus_ = 0;
#if GBA_TRACE_TIMERS
    u32 trace_active_info_offset_ = kIwramSize;
#endif
    DebugOutputCallback debug_callback_;
    std::array<char, 256> debug_string_{};
};

}  // namespace gba
