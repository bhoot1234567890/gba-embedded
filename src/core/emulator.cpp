#include "gba/core/emulator.hpp"

#include <algorithm>

#ifdef GBA_PLATFORM_ESP32
#include "esp_attr.h"
#else
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
#endif

namespace gba {

Emulator::Emulator(TraceLogger* logger)
    : bus_(cartridge_, ppu_, timers_, dma_, apu_, irq_), cpu_(bus_, irq_, logger) {}

#ifndef GBA_PLATFORM_ESP32
bool Emulator::load_rom_from_file(const std::filesystem::path& path) {
    return cartridge_.load_rom_from_file(path);
}

bool Emulator::load_bios_from_file(const std::filesystem::path& path) {
    return cartridge_.load_bios_from_file(path);
}
#endif

void Emulator::load_rom(std::vector<u8> rom) {
    cartridge_.set_rom(std::move(rom));
}

void Emulator::set_rom_provider(std::unique_ptr<RomProvider> rom) {
    cartridge_.set_rom_provider(std::move(rom));
}

void Emulator::load_bios(std::vector<u8> bios) {
    cartridge_.set_bios(std::move(bios));
}

void Emulator::set_save_type(SaveType save_type) {
    cartridge_.set_save_type(save_type);
}

void Emulator::reset(bool skip_bios) {
    irq_.reset();
    ppu_.reset();
    apu_.reset();
    timers_.reset();
    dma_.reset();
    bus_.reset();
    cpu_.reset(skip_bios);
    scheduler_.reset(0);

    if (skip_bios) {
        (void)bus_.write(kWaitCnt, 0x4317u, BusWidth::Half, AccessType::Io, 0);
    }

    refresh_schedule();
}

void Emulator::run_until(u64 target_cycle) {
    while (cpu_.current_cycle() < target_cycle) {
        auto next_cycle = scheduler_.next_event();
        next_cycle = std::min(next_cycle, target_cycle);
        if (next_cycle == std::numeric_limits<u64>::max()) {
            next_cycle = target_cycle;
        }
        cpu_.cpu_run_until(next_cycle);
        scheduler_.set_current_cycle(cpu_.current_cycle());
        service_due_hardware();
    }
}

void Emulator::run_frame() {
    cartridge_.reset_rom_frame_stats();
    while (!ppu_.frame_ready()) {
        auto next_cycle = scheduler_.next_event();
        if (next_cycle == std::numeric_limits<u64>::max()) {
            next_cycle = cpu_.current_cycle() + 1;
        }
        cpu_.cpu_run_until(next_cycle);
        scheduler_.set_current_cycle(cpu_.current_cycle());
        service_due_hardware();
    }
    last_rom_stats_ = cartridge_.rom_frame_stats();
    ppu_.consume_frame_ready();
}

void Emulator::set_keys(u16 key_mask) {
    bus_.set_keyinput(static_cast<u16>(static_cast<u16>(~key_mask) & 0x03FFu));
}

Bus& Emulator::bus() {
    return bus_;
}

Arm7tdmi& Emulator::cpu() {
    return cpu_;
}

const IrqController& Emulator::irq() const {
    return irq_;
}

const Ppu& Emulator::ppu() const {
    return ppu_;
}

Ppu& Emulator::ppu() {
    return ppu_;
}

std::span<const u16> Emulator::framebuffer() const {
    return ppu_.framebuffer();
}

void Emulator::step_scheduler_event() {
    refresh_schedule();
    auto next_cycle = scheduler_.next_event();
    if (next_cycle == std::numeric_limits<u64>::max()) {
        next_cycle = cpu_.current_cycle() + 1;
    }
    cpu_.cpu_run_until(next_cycle);
    scheduler_.set_current_cycle(cpu_.current_cycle());
    service_due_hardware();
}

void IRAM_ATTR Emulator::refresh_schedule() {
    scheduler_.set_current_cycle(cpu_.current_cycle());
    scheduler_.set_next_event(SchedulerSlot::Ppu, ppu_.next_event_cycle());
    scheduler_.set_next_event(SchedulerSlot::Timers, timers_.next_event_cycle());
    scheduler_.set_next_event(SchedulerSlot::Dma, dma_.next_event_cycle());
    scheduler_.set_next_event(SchedulerSlot::Apu, apu_.next_event_cycle());
    scheduler_.set_next_event(SchedulerSlot::Serial, bus_.next_event_cycle());
    scheduler_.set_next_event(SchedulerSlot::Irq, irq_.next_event_cycle());
}

void IRAM_ATTR Emulator::service_due_hardware() {
    auto stable_cycle = scheduler_.current_cycle();
    bool progress = true;
    while (progress) {
        progress = false;

        const auto was_hblank = ppu_.is_hblank();
        const auto was_vblank = ppu_.is_vblank();

        irq_.advance(stable_cycle);
        bus_.service_timers(stable_cycle);

        ppu_.advance_to(stable_cycle, irq_);
        if (!was_hblank && ppu_.is_hblank() && !ppu_.is_vblank()) {
            dma_.request_hblank(stable_cycle);
        }
        if (!was_vblank && ppu_.is_vblank()) {
            dma_.request_vblank(stable_cycle);
        }

        while (const auto line = ppu_.consume_scanline_ready()) {
            if (!skip_render_) {
                ppu_.render_scanline(*line, bus_.vram(), bus_.palette(), bus_.oam());
            }
        }

        apu_.advance_to(stable_cycle);

        const auto dma_cycles = dma_.service_due(stable_cycle, bus_, irq_);
        if (dma_cycles != 0) {
            stable_cycle += dma_cycles;
            scheduler_.set_current_cycle(stable_cycle);
            cpu_.set_current_cycle(stable_cycle);
            progress = true;
        }
    }

    refresh_schedule();
}

}  // namespace gba
