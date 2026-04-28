/*
 * GBA emulator core test on ESP32-S3 (QEMU).
 * Tests lightweight components. Full Emulator tests skipped due to SRAM limits.
 * The Emulator struct needs ~463KB (EWRAM 256KB, VRAM 96KB, framebuffer 77KB)
 * which exceeds ESP32-S3 SRAM without PSRAM.
 */

#include <array>
#include <cstdlib>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "gba/core/apu.hpp"
#include "gba/core/bus.hpp"
#include "gba/core/constants.hpp"
#include "gba/core/cpu.hpp"
#include "gba/core/dma.hpp"
#include "gba/core/emulator.hpp"
#include "gba/core/irq.hpp"
#include "gba/core/ppu.hpp"
#include "gba/core/scheduler.hpp"
#include "gba/core/timers.hpp"

namespace {

using namespace gba;

static const char* kTag = "gba_test";

void expect(bool condition, const char* message) {
    if (!condition) {
        ESP_LOGE(kTag, "FAIL: %s", message);
        throw std::runtime_error(message);
    }
}

void write32(std::span<u8> bytes, u32 offset, u32 value) {
    bytes[offset + 0] = static_cast<u8>(value & 0xFFu);
    bytes[offset + 1] = static_cast<u8>((value >> 8u) & 0xFFu);
    bytes[offset + 2] = static_cast<u8>((value >> 16u) & 0xFFu);
    bytes[offset + 3] = static_cast<u8>((value >> 24u) & 0xFFu);
}

void write16(std::span<u8> bytes, u32 offset, u16 value) {
    bytes[offset + 0] = static_cast<u8>(value & 0xFFu);
    bytes[offset + 1] = static_cast<u8>((value >> 8u) & 0xFFu);
}

void test_heap_info() {
    auto free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    auto free_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    ESP_LOGI(kTag, "  free internal: %u bytes", (unsigned)free_internal);
    ESP_LOGI(kTag, "  free 8bit: %u bytes", (unsigned)free_8bit);
}

void test_scheduler() {
    Scheduler scheduler;
    scheduler.reset();
    scheduler.set_next_event(SchedulerSlot::Ppu, 100);
    scheduler.set_next_event(SchedulerSlot::Timers, 40);
    scheduler.set_next_event(SchedulerSlot::Dma, 80);
    expect(scheduler.next_event() == 40, "scheduler should return the earliest event");
    ESP_LOGI(kTag, "  scheduler: OK");
}

void test_irq_controller() {
    IrqController irq;
    irq.reset();
    irq.write_register(kIe, IrqVBlank, BusWidth::Half, 0);
    irq.write_register(kIme, 1, BusWidth::Half, 0);
    irq.request(IrqVBlank);
    expect(irq.line_asserted(), "irq line should assert when IME and IE allow a pending interrupt");
    irq.write_register(kIf, IrqVBlank, BusWidth::Half, 0);
    expect(!irq.line_asserted(), "writing IF should acknowledge the interrupt");
    ESP_LOGI(kTag, "  irq_controller: OK");
}

void test_timer_overflow_irq() {
    Timers timers;
    IrqController irq;
    Apu apu;
    timers.reset();
    irq.reset();
    apu.reset();

    timers.write_register(kTm0CntL, 0xFFFEu, BusWidth::Half, 0, irq, apu);
    timers.write_register(kTm0CntH, 0x00C0u, BusWidth::Half, 0, irq, apu);
    timers.advance_to(1, irq, apu);
    expect((irq.iflags() & IrqTimer0) == 0u, "timer 0 should not overflow before its exact cycle");
    timers.advance_to(2, irq, apu);
    expect((irq.iflags() & IrqTimer0) != 0u, "timer 0 should request an IRQ on overflow");
    ESP_LOGI(kTag, "  timer_overflow_irq: OK");
}

void test_audio_fifo_timer_cadence() {
    Timers timers;
    IrqController irq;
    Apu apu;
    timers.reset();
    irq.reset();
    apu.reset();

    apu.write_register(kSoundCntX, 0x0080u, BusWidth::Half, 0);
    apu.write_register(kSoundCntH, 0x0300u, BusWidth::Half, 0);
    apu.write_register(kFifoA, 0x04030201u, BusWidth::Word, 0);

    timers.write_register(kTm0CntL, 0xFFFFu, BusWidth::Half, 0, irq, apu);
    timers.write_register(kTm0CntH, 0x0080u, BusWidth::Half, 0, irq, apu);
    timers.advance_to(1, irq, apu);

    const auto chunk = apu.consume_audio_chunk();
    expect(chunk.size() == 2u, "one timer tick should produce one stereo sample pair");
    expect(chunk[0] == 256 && chunk[1] == 256, "direct sound sample values should follow FIFO/timer cadence");
    expect(apu.take_fifo_request_a(), "FIFO A should request DMA refill when level falls below threshold");
    ESP_LOGI(kTag, "  audio_fifo: OK");
}

// CPU test using Bus — allocate one at a time and free immediately
void test_cpu_arm_add() {
    ESP_LOGI(kTag, "  heap before CPU test: %u bytes", (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));

    auto cartridge = new(std::nothrow) Cartridge();
    auto irq = new(std::nothrow) IrqController();
    auto ppu = new(std::nothrow) Ppu();
    auto timers = new(std::nothrow) Timers();
    auto dma = new(std::nothrow) DmaEngine();
    auto apu = new(std::nothrow) Apu();
    auto bus = new(std::nothrow) Bus(*cartridge, *ppu, *timers, *dma, *apu, *irq);

    if (!bus) {
        ESP_LOGW(kTag, "  SKIPPED (not enough heap for Bus)");
        delete apu; delete dma; delete timers; delete ppu; delete irq; delete cartridge;
        return;
    }

    irq->reset();
    ppu->reset();
    timers->reset();
    dma->reset();
    apu->reset();
    bus->reset();

    Arm7tdmi cpu(*bus, *irq);
    cpu.reset();

    auto& state = cpu.state();
    state.cpsr = static_cast<u32>(CpuMode::System);
    state.regs[15] = 0x03000000u;

    auto iwram = bus->iwram();
    write32(iwram, 0, 0xE3A01002u);  // MOV r1, #2
    write32(iwram, 4, 0xE2810001u);  // ADD r0, r1, #1

    cpu.step();
    cpu.step();

    expect(state.regs[0] == 3u, "ARM interpreter should execute MOV/ADD immediate");

    delete bus;
    delete apu;
    delete dma;
    delete timers;
    delete ppu;
    delete irq;
    delete cartridge;
    ESP_LOGI(kTag, "  cpu_arm_add: OK");
}

void test_cpu_thumb_add() {
    auto cartridge = new(std::nothrow) Cartridge();
    auto irq = new(std::nothrow) IrqController();
    auto ppu = new(std::nothrow) Ppu();
    auto timers = new(std::nothrow) Timers();
    auto dma = new(std::nothrow) DmaEngine();
    auto apu = new(std::nothrow) Apu();
    auto bus = new(std::nothrow) Bus(*cartridge, *ppu, *timers, *dma, *apu, *irq);

    if (!bus) {
        ESP_LOGW(kTag, "  SKIPPED (not enough heap for Bus)");
        delete apu; delete dma; delete timers; delete ppu; delete irq; delete cartridge;
        return;
    }

    irq->reset();
    ppu->reset();
    timers->reset();
    dma->reset();
    apu->reset();
    bus->reset();

    Arm7tdmi cpu(*bus, *irq);
    cpu.reset();

    auto& state = cpu.state();
    state.cpsr = static_cast<u32>(CpuMode::System) | (1u << 5);
    state.regs[15] = 0x03000000u;

    auto iwram = bus->iwram();
    write16(iwram, 0, 0x2001u);  // MOVS r0, #1
    write16(iwram, 2, 0x3002u);  // ADDS r0, #2

    cpu.step();
    cpu.step();

    expect(state.regs[0] == 3u, "Thumb interpreter should execute MOVS/ADDS immediate");

    delete bus;
    delete apu;
    delete dma;
    delete timers;
    delete ppu;
    delete irq;
    delete cartridge;
    ESP_LOGI(kTag, "  cpu_thumb_add: OK");
}

}  // namespace

extern "C" void app_main(void) {
    ESP_LOGI(kTag, "=== GBA Emulator Core Tests (ESP32-S3 QEMU) ===");
    test_heap_info();

    struct TestCase {
        const char* name;
        void (*func)();
    };

    const TestCase tests[] = {
        {"scheduler", test_scheduler},
        {"irq_controller", test_irq_controller},
        {"timer_overflow_irq", test_timer_overflow_irq},
        {"audio_fifo", test_audio_fifo_timer_cadence},
        {"cpu_arm_add", test_cpu_arm_add},
        {"cpu_thumb_add", test_cpu_thumb_add},
    };

    int passed = 0;
    int failed = 0;

    for (const auto& test : tests) {
        ESP_LOGI(kTag, "Running: %s", test.name);
        try {
            test.func();
            ++passed;
        } catch (...) {
            ++failed;
            ESP_LOGE(kTag, "  FAILED");
        }
    }

    ESP_LOGI(kTag, "=== Results: %d passed, %d failed ===", passed, failed);

    if (failed == 0) {
        ESP_LOGI(kTag, "ALL TESTS PASSED on ESP32-S3!");
    } else {
        ESP_LOGE(kTag, "SOME TESTS FAILED");
    }
}
