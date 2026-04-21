#pragma once

#ifndef GBA_PLATFORM_ESP32
#include <filesystem>
#endif

#include "gba/core/apu.hpp"
#include "gba/core/bus.hpp"
#include "gba/core/cartridge.hpp"
#include "gba/core/cpu.hpp"
#include "gba/core/dma.hpp"
#include "gba/core/irq.hpp"
#include "gba/core/ppu.hpp"
#include "gba/core/scheduler.hpp"
#include "gba/core/timers.hpp"

namespace gba {

class Emulator {
public:
    explicit Emulator(TraceLogger* logger = nullptr);

#ifndef GBA_PLATFORM_ESP32
    [[nodiscard]] bool load_rom_from_file(const std::filesystem::path& path);
    [[nodiscard]] bool load_bios_from_file(const std::filesystem::path& path);
#endif
    void load_rom(std::vector<u8> rom);
    void load_bios(std::vector<u8> bios);

    void reset();
    void run_until(u64 target_cycle);
    void run_frame();

    void set_keys(u16 key_mask);

    [[nodiscard]] Bus& bus();
    [[nodiscard]] Arm7tdmi& cpu();
    [[nodiscard]] const Ppu& ppu() const;
    [[nodiscard]] const std::array<u16, kFramebufferPixels>& framebuffer() const;

private:
    void refresh_schedule();
    void service_due_hardware();

    Cartridge cartridge_{};
    IrqController irq_{};
    Ppu ppu_{};
    Apu apu_{};
    Timers timers_{};
    DmaEngine dma_{};
    Bus bus_;
    Arm7tdmi cpu_;
    Scheduler scheduler_{};
};

}  // namespace gba
