#pragma once

#include <array>
#include <functional>
#include <memory>

#include "gba/core/cartridge.hpp"
#include "gba/core/constants.hpp"
#include "gba/core/types.hpp"

namespace gba {

using DebugOutputCallback = std::function<void(const char*)>;

class Apu;
class DmaEngine;
class IrqController;
class Ppu;
class Timers;

class Bus {
public:
    Bus(Cartridge& cartridge, Ppu& ppu, Timers& timers, DmaEngine& dma, Apu& apu, IrqController& irq);

    void reset();

    [[nodiscard]] BusAccessResult read(u32 address, BusWidth width, AccessType access, u64 cycle_now);
    [[nodiscard]] BusAccessResult write(u32 address, u32 value, BusWidth width, AccessType access, u64 cycle_now);
    [[nodiscard]] u32 peek_word(u32 address) const;

    [[nodiscard]] std::span<const u8> vram() const;
    [[nodiscard]] std::span<const u8> palette() const;
    [[nodiscard]] std::span<const u8> oam() const;
    [[nodiscard]] std::span<u8> ewram();
    [[nodiscard]] std::span<u8> iwram();

    void set_keyinput(u16 value);
    void set_debug_output(DebugOutputCallback callback);
    [[nodiscard]] u16 keyinput() const;
    [[nodiscard]] u16 keycnt() const;
    [[nodiscard]] u16 waitcnt() const;
    [[nodiscard]] bool has_bios() const;
    [[nodiscard]] bool halted() const;
    void clear_halt();
    void service_timers(u64 cycle_now);
    [[nodiscard]] u32 service_dma(u64 cycle_now);
    void prefetch_advance(int cycles);

private:
    [[nodiscard]] static u32 read_array(std::span<const u8> bytes, u32 address, BusWidth width);
    static void write_array(std::span<u8> bytes, u32 address, u32 value, BusWidth width);
    [[nodiscard]] static u32 expand_bus_latch(u32 value, BusWidth width);
    void record_open_bus_read(u32 value, BusWidth width);
    void update_keypad_irq();
    void update_wait_state_table();
    [[nodiscard]] u32 region_cycles(u32 address, BusWidth width, AccessType access, u64 cycle_now) const;
    [[nodiscard]] u32 prefetch_region_cycles(u32 address, BusWidth width) const;
    [[nodiscard]] BusAccessResult read_io(u32 address, BusWidth width, AccessType access, u64 cycle_now);
    [[nodiscard]] BusAccessResult write_io(u32 address, u32 value, BusWidth width, u64 cycle_now);

    Cartridge& cartridge_;
    Ppu& ppu_;
    Timers& timers_;
    DmaEngine& dma_;
    Apu& apu_;
    IrqController& irq_;

    /* Heap-allocated memory arrays — use PSRAM on ESP32 when available */
    std::unique_ptr<u8, void (*)(u8*)> ewram_;
    std::unique_ptr<u8, void (*)(u8*)> iwram_;
    std::unique_ptr<u8, void (*)(u8*)> palette_;
    std::unique_ptr<u8, void (*)(u8*)> vram_;
    std::unique_ptr<u8, void (*)(u8*)> oam_;

    /* GamePak prefetch buffer — 8 halfwords deep */
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
    u16 waitcnt_ = 0;
    u16 siocnt_ = 0;
    u16 rcnt_ = 0;
    u16 mgba_log_enable_ = 0;
    u8 postflg_ = 0;
    bool halted_ = false;
    bool bios_latch_valid_ = false;
    bool rom_latch_valid_ = false;
    AccessType last_access_ = AccessType::NonSequential;
    u32 bios_latch_ = 0;
    u32 rom_latch_ = 0;
    u32 open_bus_ = 0;
    std::array<std::array<u8, 16>, 2> wait16_{};
    std::array<std::array<u8, 16>, 2> wait32_{};
    DebugOutputCallback debug_callback_;
    std::array<char, 256> debug_string_{};
};

}  // namespace gba
