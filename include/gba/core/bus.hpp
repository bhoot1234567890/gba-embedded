#pragma once

#include <array>

#include "gba/core/cartridge.hpp"
#include "gba/core/constants.hpp"
#include "gba/core/types.hpp"

namespace gba {

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

    [[nodiscard]] std::span<const u8> vram() const;
    [[nodiscard]] std::span<const u8> palette() const;
    [[nodiscard]] std::span<const u8> oam() const;
    [[nodiscard]] std::span<u8> ewram();
    [[nodiscard]] std::span<u8> iwram();

    void set_keyinput(u16 value);
    [[nodiscard]] u16 keyinput() const;
    [[nodiscard]] u16 keycnt() const;
    [[nodiscard]] u16 waitcnt() const;
    [[nodiscard]] bool has_bios() const;
    [[nodiscard]] bool halted() const;
    void clear_halt();

private:
    [[nodiscard]] static u32 read_array(std::span<const u8> bytes, u32 address, BusWidth width);
    static void write_array(std::span<u8> bytes, u32 address, u32 value, BusWidth width);
    void update_keypad_irq();
    [[nodiscard]] u32 region_cycles(u32 address, BusWidth width, AccessType access, u64 cycle_now) const;
    [[nodiscard]] BusAccessResult read_io(u32 address, BusWidth width, u64 cycle_now);
    [[nodiscard]] BusAccessResult write_io(u32 address, u32 value, BusWidth width, u64 cycle_now);

    Cartridge& cartridge_;
    Ppu& ppu_;
    Timers& timers_;
    DmaEngine& dma_;
    Apu& apu_;
    IrqController& irq_;

    std::array<u8, kEwramSize> ewram_{};
    std::array<u8, kIwramSize> iwram_{};
    std::array<u8, kPaletteSize> palette_{};
    std::array<u8, kVramSize> vram_{};
    std::array<u8, kOamSize> oam_{};

    u16 keyinput_ = 0x03FF;
    u16 keycnt_ = 0;
    u16 waitcnt_ = 0;
    u8 postflg_ = 0;
    bool halted_ = false;
    u32 open_bus_ = 0;
};

}  // namespace gba
