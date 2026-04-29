#pragma once

#ifndef GBA_PLATFORM_ESP32
#include <filesystem>
#endif

#include <memory>
#include <span>
#include <vector>

#include "gba/core/apu.hpp"
#include "gba/core/bus.hpp"
#include "gba/core/cartridge.hpp"
#include "gba/core/cpu.hpp"
#include "gba/core/dma.hpp"
#include "gba/core/irq.hpp"
#include "gba/core/ppu.hpp"
#include "gba/core/scheduler.hpp"
#include "gba/core/timers.hpp"
#include "gba/core/types.hpp"

namespace gba {

class Emulator {
public:
    explicit Emulator(TraceLogger* logger = nullptr);

#ifndef GBA_PLATFORM_ESP32
    [[nodiscard]] bool load_rom_from_file(const std::filesystem::path& path);
    [[nodiscard]] bool load_bios_from_file(const std::filesystem::path& path);
#endif
    void load_rom(std::vector<u8> rom);
    void set_rom_provider(std::unique_ptr<RomProvider> rom);
    void load_bios(std::vector<u8> bios);
    void set_save_type(SaveType save_type);

    void reset(bool skip_bios = false);
    void run_until(u64 target_cycle);
    void run_frame();

    void set_keys(u16 key_mask);
    void set_skip_render(bool skip) { skip_render_ = skip; }

    [[nodiscard]] Bus& bus();
    [[nodiscard]] Cartridge& cartridge() { return cartridge_; }
    [[nodiscard]] Arm7tdmi& cpu();
    [[nodiscard]] const IrqController& irq() const;
    [[nodiscard]] const Ppu& ppu() const;
    [[nodiscard]] Ppu& ppu();
    [[nodiscard]] Apu& apu() { return apu_; }
    [[nodiscard]] RomAccessStats last_rom_stats() const { return last_rom_stats_; }
    [[nodiscard]] std::span<const u16> framebuffer() const;

    void step_scheduler_event();

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
    bool skip_render_ = false;
    RomAccessStats last_rom_stats_{};
};

}  // namespace gba
