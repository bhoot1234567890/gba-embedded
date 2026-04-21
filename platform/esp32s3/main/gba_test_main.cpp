/*
 * GBA emulator core test on ESP32-S3 (QEMU).
 * Runs the unit test suite on-target to verify the core works on Xtensa LX7.
 */

#include <array>
#include <cstdlib>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>

#include "esp_log.h"
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

static int g_pass = 0;
static int g_fail = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        ESP_LOGE(kTag, "FAIL: %s", message);
        ++g_fail;
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

u64 hash_words(std::span<const u16> words) {
    u64 hash = 1469598103934665603ull;
    constexpr u64 kPrime = 1099511628211ull;
    for (const auto word : words) {
        const auto lo = static_cast<u8>(word & 0xFFu);
        const auto hi = static_cast<u8>((word >> 8u) & 0xFFu);
        hash ^= lo;
        hash *= kPrime;
        hash ^= hi;
        hash *= kPrime;
    }
    return hash;
}

struct TestBusContext {
    Cartridge cartridge{};
    IrqController irq{};
    Ppu ppu{};
    Timers timers{};
    DmaEngine dma{};
    Apu apu{};
    Bus bus;

    TestBusContext() : bus(cartridge, ppu, timers, dma, apu, irq) {}

    void reset() {
        irq.reset();
        ppu.reset();
        timers.reset();
        dma.reset();
        apu.reset();
        bus.reset();
    }
};

// --- Tests (same as host test_main.cpp) ---

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
    irq.write_register(kIe, IrqVBlank, BusWidth::Half);
    irq.write_register(kIme, 1, BusWidth::Half);
    irq.request(IrqVBlank);
    expect(irq.line_asserted(), "irq line should assert when IME and IE allow a pending interrupt");
    irq.write_register(kIf, IrqVBlank, BusWidth::Half);
    expect(!irq.line_asserted(), "writing IF should acknowledge the interrupt");
    ESP_LOGI(kTag, "  irq_controller: OK");
}

void test_cpu_arm_add() {
    auto emulator = new Emulator();
    emulator->reset();

    auto& cpu = emulator->cpu();
    auto& state = cpu.state();
    state.cpsr = static_cast<u32>(CpuMode::System);
    state.regs[15] = 0x03000000u;

    auto iwram = emulator->bus().iwram();
    write32(iwram, 0, 0xE3A01002u);  // MOV r1, #2
    write32(iwram, 4, 0xE2810001u);  // ADD r0, r1, #1

    cpu.step();
    cpu.step();

    expect(state.regs[0] == 3u, "ARM interpreter should execute MOV/ADD immediate");
    delete emulator;
    ESP_LOGI(kTag, "  cpu_arm_add: OK");
}

void test_cpu_thumb_add() {
    auto emulator = new Emulator();
    emulator->reset();

    auto& cpu = emulator->cpu();
    auto& state = cpu.state();
    state.cpsr = static_cast<u32>(CpuMode::System) | (1u << 5);
    state.regs[15] = 0x03000000u;

    auto iwram = emulator->bus().iwram();
    write16(iwram, 0, 0x2001u);  // MOVS r0, #1
    write16(iwram, 2, 0x3002u);  // ADDS r0, #2

    cpu.step();
    cpu.step();

    expect(state.regs[0] == 3u, "Thumb interpreter should execute MOVS/ADDS immediate");
    delete emulator;
    ESP_LOGI(kTag, "  cpu_thumb_add: OK");
}

void test_ppu_mode3_render_and_hash() {
    auto ppu = new Ppu();
    auto irq = new IrqController();
    ppu->reset();
    irq->reset();

    auto vram = new std::array<u8, kVramSize>();
    auto palette = new std::array<u8, kPaletteSize>();
    vram->fill(0);
    palette->fill(0);

    ppu->write_register(kDispcnt, 0x0003u, BusWidth::Half);
    for (u32 x = 0; x < kScreenWidth; ++x) {
        const auto color = static_cast<u16>(((x * 3u) ^ 0x55AAu) & 0xFFFFu);
        write16(std::span<u8>(vram->data(), vram->size()), x * 2u, color);
    }
    ppu->render_scanline(0, std::span<const u8>(vram->data(), vram->size()),
                         std::span<const u8>(palette->data(), palette->size()));

    const auto& framebuffer = ppu->framebuffer();
    const std::span<const u16> row(framebuffer.data(), kScreenWidth);
    expect(hash_words(row) == 0x9110C6E576A2850Aull, "mode 3 scanline hash should match golden output");

    ppu->advance_to(960, *irq);
    expect(ppu->is_hblank(), "PPU should enter HBlank at 960 cycles");
    delete vram;
    delete palette;
    delete ppu;
    delete irq;
    ESP_LOGI(kTag, "  ppu_mode3: OK");
}

void test_ppu_mode4_render_hash() {
    auto ppu = new Ppu();
    ppu->reset();

    auto vram = new std::array<u8, kVramSize>();
    auto palette = new std::array<u8, kPaletteSize>();
    vram->fill(0);
    palette->fill(0);

    ppu->write_register(kDispcnt, 0x0004u, BusWidth::Half);
    for (u32 index = 0; index < 256u; ++index) {
        const auto color = static_cast<u16>(((index * 97u) ^ 0x1234u) & 0xFFFFu);
        write16(std::span<u8>(palette->data(), palette->size()), index * 2u, color);
    }
    for (u32 x = 0; x < kScreenWidth; ++x) {
        (*vram)[x] = static_cast<u8>(((x * 5u) + 7u) & 0xFFu);
    }

    ppu->render_scanline(0, std::span<const u8>(vram->data(), vram->size()),
                         std::span<const u8>(palette->data(), palette->size()));
    const auto& framebuffer = ppu->framebuffer();
    const std::span<const u16> row(framebuffer.data(), kScreenWidth);
    expect(hash_words(row) == 0x6686428D3269F009ull, "mode 4 scanline hash should match golden output");
    delete vram;
    delete palette;
    delete ppu;
    ESP_LOGI(kTag, "  ppu_mode4: OK");
}

void test_timer_overflow_irq() {
    Timers timers;
    IrqController irq;
    Apu apu;

    timers.reset();
    irq.reset();
    apu.reset();

    timers.write_register(kTm0CntL, 0xFFFEu, BusWidth::Half, 0);
    timers.write_register(kTm0CntH, 0x00C0u, BusWidth::Half, 0);
    timers.advance_to(1, irq, apu);
    expect((irq.iflags() & IrqTimer0) == 0u, "timer 0 should not overflow before its exact cycle");
    timers.advance_to(2, irq, apu);
    expect((irq.iflags() & IrqTimer0) != 0u, "timer 0 should request an IRQ on overflow");
    ESP_LOGI(kTag, "  timer_overflow_irq: OK");
}

void test_dma_immediate_copy() {
    auto context = new TestBusContext();
    context->reset();

    write32(context->bus.ewram(), 0, 0x11223344u);
    write32(context->bus.ewram(), 4, 0x55667788u);

    context->dma.write_register(kDma0Sad, 0x02000000u, BusWidth::Word, 0);
    context->dma.write_register(kDma0Dad, 0x02000010u, BusWidth::Word, 0);
    context->dma.write_register(kDma0CntL, 2u, BusWidth::Half, 0);
    context->dma.write_register(kDma0CntH, 0x8400u, BusWidth::Half, 0);

    const auto cycles = context->dma.service_due(0, context->bus, context->irq);
    expect(cycles > 0, "DMA should consume bus cycles when transferring data");
    expect(context->bus.ewram()[0x10] == 0x44u && context->bus.ewram()[0x13] == 0x11u, "DMA should copy the first word");
    expect(context->bus.ewram()[0x14] == 0x88u && context->bus.ewram()[0x17] == 0x55u, "DMA should copy the second word");
    delete context;
    ESP_LOGI(kTag, "  dma_immediate: OK");
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

    timers.write_register(kTm0CntL, 0xFFFFu, BusWidth::Half, 0);
    timers.write_register(kTm0CntH, 0x0080u, BusWidth::Half, 0);
    timers.advance_to(1, irq, apu);

    const auto chunk = apu.consume_audio_chunk();
    expect(chunk.size() == 2u, "one timer tick should produce one stereo sample pair");
    expect(chunk[0] == 256 && chunk[1] == 256, "direct sound sample values should follow FIFO/timer cadence");
    expect(apu.take_fifo_request_a(), "FIFO A should request DMA refill when level falls below threshold");
    ESP_LOGI(kTag, "  audio_fifo: OK");
}

void test_run_frame() {
    auto emulator = new Emulator();
    emulator->reset();

    // Put a simple infinite loop at 0x03000000: B .
    auto iwram = emulator->bus().iwram();
    write32(iwram, 0, 0xEAFFFFFEu);  // B . (branch to self)
    auto& cpu = emulator->cpu();
    auto& state = cpu.state();
    state.cpsr = static_cast<u32>(CpuMode::System);
    state.regs[15] = 0x03000000u;

    emulator->run_frame();

    auto cycles = cpu.current_cycle();
    delete emulator;
    ESP_LOGI(kTag, "  run_frame: OK (cycles=%llu)", (unsigned long long)cycles);
    expect(cycles > 0, "run_frame should advance cycles");
}

}  // namespace

extern "C" void app_main(void) {
    ESP_LOGI(kTag, "=== GBA Emulator Core Tests (ESP32-S3 QEMU) ===");

    struct TestCase {
        const char* name;
        void (*func)();
    };

    const TestCase tests[] = {
        {"scheduler", test_scheduler},
        {"irq_controller", test_irq_controller},
        {"cpu_arm_add", test_cpu_arm_add},
        {"cpu_thumb_add", test_cpu_thumb_add},
        {"ppu_mode3", test_ppu_mode3_render_and_hash},
        {"ppu_mode4", test_ppu_mode4_render_hash},
        {"timer_overflow_irq", test_timer_overflow_irq},
        {"dma_immediate", test_dma_immediate_copy},
        {"audio_fifo", test_audio_fifo_timer_cadence},
        {"run_frame", test_run_frame},
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
