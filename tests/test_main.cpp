#include <array>
#include <cstdlib>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>

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

void expect(bool condition, const std::string& message) {
    if (!condition) {
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

void test_scheduler() {
    Scheduler scheduler;
    scheduler.reset();
    scheduler.set_next_event(SchedulerSlot::Ppu, 100);
    scheduler.set_next_event(SchedulerSlot::Timers, 40);
    scheduler.set_next_event(SchedulerSlot::Dma, 80);
    expect(scheduler.next_event() == 40, "scheduler should return the earliest event");
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
}

void test_bus_open_bus_and_alignment() {
    TestBusContext context;
    context.reset();

    write32(context.bus.ewram(), 0, 0xCAFEBABEu);
    const auto ewram_value = context.bus.read(0x02000000u, BusWidth::Word, AccessType::NonSequential, 0);
    expect(ewram_value.value == 0xCAFEBABEu, "EWRAM reads should return written values");

    const auto open_bus = context.bus.read(0x01000000u, BusWidth::Word, AccessType::NonSequential, 0);
    expect(open_bus.open_bus, "unmapped reads should be flagged as open bus");
    expect(open_bus.value == ewram_value.value, "open bus should return the most recent bus value");

    const auto aligned_write = context.bus.write(0x02000002u, 0xAABBCCDDu, BusWidth::Word, AccessType::NonSequential, 0);
    (void)aligned_write;
    const auto ewram = context.bus.ewram();
    expect(ewram[0] == 0xDDu && ewram[1] == 0xCCu && ewram[2] == 0xBBu && ewram[3] == 0xAAu,
           "word writes should align down and write little-endian bytes");

    const auto palette_write = context.bus.write(0x05000001u, 0x12u, BusWidth::Byte, AccessType::NonSequential, 0);
    (void)palette_write;
    const auto palette = context.bus.palette();
    expect(palette[0] == 0x12u && palette[1] == 0x12u, "palette byte writes should replicate across both bytes");

    const auto vram_before = context.bus.vram()[0x14000];
    const auto vram_write = context.bus.write(0x06014000u, 0x7Fu, BusWidth::Byte, AccessType::NonSequential, 0);
    (void)vram_write;
    expect(context.bus.vram()[0x14000] == vram_before, "VRAM OBJ area should ignore byte writes");

    const auto oam_before = context.bus.oam()[0];
    const auto oam_write = context.bus.write(0x07000000u, 0x66u, BusWidth::Byte, AccessType::NonSequential, 0);
    (void)oam_write;
    expect(context.bus.oam()[0] == oam_before, "OAM byte writes should be ignored");

    const auto keycnt_write = context.bus.write(kKeyCnt, 0xC123u, BusWidth::Half, AccessType::Io, 0);
    (void)keycnt_write;
    const auto open_bus_after_io = context.bus.read(0x01000000u, BusWidth::Word, AccessType::NonSequential, 0);
    expect(open_bus_after_io.open_bus, "unmapped reads after MMIO activity should still be flagged as open bus");
    expect(open_bus_after_io.value == 0xC123u, "open bus should retain the most recent MMIO bus value");
}

void test_cpu_misaligned_arm_ldr_rotation() {
    Emulator emulator;
    emulator.reset();

    auto& cpu = emulator.cpu();
    auto& state = cpu.state();
    state.cpsr = static_cast<u32>(CpuMode::System);
    state.regs[1] = 0x02000001u;
    state.regs[15] = 0x03000000u;

    write32(emulator.bus().ewram(), 0, 0x11223344u);
    write32(emulator.bus().iwram(), 0, 0xE5910000u);  // LDR r0, [r1]

    cpu.step();

    expect(state.regs[0] == 0x44112233u, "misaligned ARM word loads should rotate the fetched word");
}

void test_hle_swi_halt_without_bios() {
    Emulator emulator;
    emulator.reset();

    auto& cpu = emulator.cpu();
    auto& state = cpu.state();
    state.cpsr = static_cast<u32>(CpuMode::System);
    state.regs[15] = 0x03000000u;

    write32(emulator.bus().iwram(), 0, 0xEF000002u);  // SWI 2 (Halt)
    write32(emulator.bus().iwram(), 4, 0xE3A00009u);  // MOV r0, #9

    cpu.step();

    expect(state.regs[15] == 0x03000004u, "HLE SWI Halt should return to the next instruction rather than jumping into BIOS");
    expect(static_cast<u32>(state.cpsr & 0x1Fu) == static_cast<u32>(CpuMode::System),
           "HLE SWI Halt should preserve CPU mode during the lightweight BIOS path");

    cpu.step();
    expect(state.regs[0] == 0u, "HLE SWI Halt should leave the CPU halted until a wake source arrives");
}

void test_cpu_thumb_register_offset_halfword_and_signed_loads() {
    Emulator emulator;
    emulator.reset();

    auto& cpu = emulator.cpu();
    auto& state = cpu.state();
    state.cpsr = static_cast<u32>(CpuMode::System) | (1u << 5);
    state.regs[0] = 0x1234u;
    state.regs[1] = 0x02000010u;
    state.regs[2] = 0x00000002u;
    state.regs[4] = 0x02000020u;
    state.regs[5] = 0x00000001u;
    state.regs[15] = 0x03000000u;

    auto ewram = emulator.bus().ewram();
    ewram[0x21] = 0x80u;

    auto iwram = emulator.bus().iwram();
    write16(iwram, 0, 0x5288u);  // STRH r0, [r1, r2]
    write16(iwram, 2, 0x5A8Bu);  // LDRH r3, [r1, r2]
    write16(iwram, 4, 0x5766u);  // LDSB r6, [r4, r5]

    cpu.step();
    cpu.step();
    cpu.step();

    expect(ewram[0x12] == 0x34u && ewram[0x13] == 0x12u,
           "Thumb STRH register-offset should store a halfword at base plus register offset");
    expect(state.regs[3] == 0x1234u,
           "Thumb LDRH register-offset should load the stored halfword back into the destination register");
    expect(state.regs[6] == 0xFFFFFF80u,
           "Thumb LDSB register-offset should sign-extend the loaded byte");
}

void test_ppu_mode3_render_and_hash() {
    Ppu ppu;
    IrqController irq;
    ppu.reset();
    irq.reset();

    std::array<u8, kVramSize> vram{};
    std::array<u8, kPaletteSize> palette{};

    ppu.write_register(kDispcnt, 0x0003u, BusWidth::Half);
    for (u32 x = 0; x < kScreenWidth; ++x) {
        const auto color = static_cast<u16>(((x * 3u) ^ 0x55AAu) & 0xFFFFu);
        write16(vram, x * 2u, color);
    }
    ppu.render_scanline(0, vram, palette);

    const auto& framebuffer = ppu.framebuffer();
    const std::span<const u16> row(framebuffer.data(), kScreenWidth);
    expect(hash_words(row) == 0x9110C6E576A2850Aull, "mode 3 scanline hash should match golden output");

    ppu.advance_to(960, irq);
    expect(ppu.is_hblank(), "PPU should enter HBlank at 960 cycles");
}

void test_ppu_mode4_render_hash() {
    Ppu ppu;
    IrqController irq;
    ppu.reset();
    irq.reset();

    std::array<u8, kVramSize> vram{};
    std::array<u8, kPaletteSize> palette{};

    ppu.write_register(kDispcnt, 0x0004u, BusWidth::Half);
    for (u32 index = 0; index < 256u; ++index) {
        const auto color = static_cast<u16>(((index * 97u) ^ 0x1234u) & 0xFFFFu);
        write16(palette, index * 2u, color);
    }
    for (u32 x = 0; x < kScreenWidth; ++x) {
        vram[x] = static_cast<u8>(((x * 5u) + 7u) & 0xFFu);
    }

    ppu.render_scanline(0, vram, palette);
    const auto& framebuffer = ppu.framebuffer();
    const std::span<const u16> row(framebuffer.data(), kScreenWidth);
    expect(hash_words(row) == 0x6686428D3269F009ull, "mode 4 scanline hash should match golden output");
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
}

void test_dma_immediate_copy() {
    TestBusContext context;
    context.reset();

    write32(context.bus.ewram(), 0, 0x11223344u);
    write32(context.bus.ewram(), 4, 0x55667788u);

    context.dma.write_register(kDma0Sad, 0x02000000u, BusWidth::Word, 0);
    context.dma.write_register(kDma0Dad, 0x02000010u, BusWidth::Word, 0);
    context.dma.write_register(kDma0CntL, 2u, BusWidth::Half, 0);
    context.dma.write_register(kDma0CntH, 0x8400u, BusWidth::Half, 0);

    const auto cycles = context.dma.service_due(0, context.bus, context.irq);
    expect(cycles > 0, "DMA should consume bus cycles when transferring data");
    expect(context.bus.ewram()[0x10] == 0x44u && context.bus.ewram()[0x13] == 0x11u, "DMA should copy the first word");
    expect(context.bus.ewram()[0x14] == 0x88u && context.bus.ewram()[0x17] == 0x55u, "DMA should copy the second word");
}

void test_dma_hblank_irq() {
    TestBusContext context;
    context.reset();

    write32(context.bus.ewram(), 0, 0xDEADBEEFu);
    context.dma.write_register(kDma0Sad, 0x02000000u, BusWidth::Word, 0);
    context.dma.write_register(kDma0Dad, 0x02000020u, BusWidth::Word, 0);
    context.dma.write_register(kDma0CntL, 1u, BusWidth::Half, 0);
    context.dma.write_register(kDma0CntH, 0xE400u, BusWidth::Half, 0);

    context.dma.request_hblank(5);
    const auto cycles = context.dma.service_due(5, context.bus, context.irq);

    expect(cycles > 0, "HBlank-start DMA should service after an HBlank request");
    expect(context.bus.ewram()[0x20] == 0xEFu && context.bus.ewram()[0x23] == 0xDEu,
           "HBlank-start DMA should move source data into destination");
    expect((context.irq.iflags() & IrqDma0) != 0u, "DMA IRQ flag should raise when IRQ enable bit is set");
}

void test_dma_vblank_trigger() {
    TestBusContext context;
    context.reset();

    write32(context.bus.ewram(), 0, 0xCAFED00Du);
    context.dma.write_register(kDma0Sad, 0x02000000u, BusWidth::Word, 0);
    context.dma.write_register(kDma0Dad, 0x02000030u, BusWidth::Word, 0);
    context.dma.write_register(kDma0CntL, 1u, BusWidth::Half, 0);
    context.dma.write_register(kDma0CntH, 0x9400u, BusWidth::Half, 0);

    expect(context.dma.service_due(0, context.bus, context.irq) == 0u,
           "VBlank-start DMA should not run before the VBlank trigger is requested");

    context.dma.request_vblank(8);
    const auto cycles = context.dma.service_due(8, context.bus, context.irq);
    expect(cycles > 0, "VBlank-start DMA should run after a VBlank trigger is requested");
    expect(context.bus.ewram()[0x30] == 0x0Du && context.bus.ewram()[0x33] == 0xCAu,
           "VBlank-start DMA should copy the requested word into destination memory");
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
}

void test_input_registers() {
    Emulator emulator;
    emulator.reset();

    emulator.set_keys(static_cast<u16>(kKeyA | kKeyStart | kKeyLeft));
    const auto keyinput = emulator.bus().read(kKeyInput, BusWidth::Half, AccessType::Io, 0);
    expect((keyinput.value & (kKeyA | kKeyStart | kKeyLeft)) == 0u, "pressed keys should read as active low");
    expect((keyinput.value & kKeyB) != 0u, "unpressed keys should remain high");

    const auto keycnt_write = emulator.bus().write(kKeyCnt, 0x4001u, BusWidth::Half, AccessType::Io, 0);
    (void)keycnt_write;
    const auto keycnt = emulator.bus().read(kKeyCnt, BusWidth::Half, AccessType::Io, 0);
    expect(keycnt.value == 0x4001u, "KEYCNT writes should be observable via MMIO reads");
}

void test_keypad_irq_or_and_modes() {
    Emulator emulator;
    emulator.reset();

    const auto ie_write = emulator.bus().write(kIe, IrqKeypad, BusWidth::Half, AccessType::Io, 0);
    (void)ie_write;

    const auto keycnt_or = emulator.bus().write(kKeyCnt, 0x4003u, BusWidth::Half, AccessType::Io, 0);
    (void)keycnt_or;
    emulator.set_keys(kKeyA);
    expect((emulator.bus().read(kIf, BusWidth::Half, AccessType::Io, 0).value & IrqKeypad) != 0u,
           "keypad IRQ should trigger in logical OR mode when any selected key is pressed");

    const auto if_ack_or = emulator.bus().write(kIf, IrqKeypad, BusWidth::Half, AccessType::Io, 0);
    (void)if_ack_or;

    const auto keycnt_and = emulator.bus().write(kKeyCnt, 0xC003u, BusWidth::Half, AccessType::Io, 0);
    (void)keycnt_and;
    emulator.set_keys(kKeyA);
    expect((emulator.bus().read(kIf, BusWidth::Half, AccessType::Io, 0).value & IrqKeypad) == 0u,
           "keypad IRQ should stay clear in logical AND mode until all selected keys are pressed");

    emulator.set_keys(static_cast<u16>(kKeyA | kKeyB));
    expect((emulator.bus().read(kIf, BusWidth::Half, AccessType::Io, 0).value & IrqKeypad) != 0u,
           "keypad IRQ should trigger in logical AND mode when all selected keys are pressed");
}

void test_halt_wakeup_edge_cases() {
    {
        Emulator emulator;
        emulator.reset();

        auto& cpu = emulator.cpu();
        auto& state = cpu.state();
        state.cpsr = static_cast<u32>(CpuMode::System);
        state.regs[15] = 0x03000000u;
        write32(emulator.bus().iwram(), 0, 0xE3A00001u);  // MOV r0, #1

        const auto halt_write = emulator.bus().write(kHaltCnt, 0, BusWidth::Byte, AccessType::Io, 0);
        (void)halt_write;
        cpu.step();

        expect(state.halted, "CPU should remain halted when there is no enabled pending IRQ");
        expect(state.regs[0] == 0u, "halted CPU should not execute the next instruction");
    }

    {
        Emulator emulator;
        emulator.reset();

        auto& cpu = emulator.cpu();
        auto& state = cpu.state();
        state.cpsr = static_cast<u32>(CpuMode::System);
        state.regs[15] = 0x03000000u;
        write32(emulator.bus().iwram(), 0, 0xE3A00002u);  // MOV r0, #2

        const auto keycnt_write = emulator.bus().write(kKeyCnt, 0x4001u, BusWidth::Half, AccessType::Io, 0);
        (void)keycnt_write;
        emulator.set_keys(kKeyA);
        const auto halt_write = emulator.bus().write(kHaltCnt, 0, BusWidth::Byte, AccessType::Io, 0);
        (void)halt_write;
        cpu.step();

        expect(state.halted, "CPU should not wake from HALT when the pending IRQ is masked in IE");
        expect(state.regs[0] == 0u, "masked pending IRQs should not resume instruction execution from HALT");
    }

    {
        Emulator emulator;
        emulator.reset();

        auto& cpu = emulator.cpu();
        auto& state = cpu.state();
        state.cpsr = static_cast<u32>(CpuMode::System);
        state.regs[15] = 0x03000000u;
        write32(emulator.bus().iwram(), 0, 0xE3A00007u);  // MOV r0, #7

        const auto ie_write = emulator.bus().write(kIe, IrqKeypad, BusWidth::Half, AccessType::Io, 0);
        (void)ie_write;
        const auto keycnt_write = emulator.bus().write(kKeyCnt, 0x4001u, BusWidth::Half, AccessType::Io, 0);
        (void)keycnt_write;
        emulator.set_keys(kKeyA);
        const auto halt_write = emulator.bus().write(kHaltCnt, 0, BusWidth::Byte, AccessType::Io, 0);
        (void)halt_write;
        cpu.step();

        expect(!state.halted, "CPU should wake from HALT when IE and IF match even if IME is disabled");
        expect(state.regs[0] == 7u, "waking from HALT without IME should resume at the next instruction");
        expect(static_cast<u32>(state.cpsr & 0x1Fu) == static_cast<u32>(CpuMode::System),
               "HALT wake with IME disabled should not vector into IRQ mode");
    }
}

void test_cpu_arm_add() {
    Emulator emulator;
    emulator.reset();

    auto& cpu = emulator.cpu();
    auto& state = cpu.state();
    state.cpsr = static_cast<u32>(CpuMode::System);
    state.regs[15] = 0x03000000u;

    auto iwram = emulator.bus().iwram();
    write32(iwram, 0, 0xE3A01002u);  // MOV r1, #2
    write32(iwram, 4, 0xE2810001u);  // ADD r0, r1, #1

    cpu.step();
    cpu.step();

    expect(state.regs[0] == 3u, "ARM interpreter should execute MOV/ADD immediate");
}

void test_cpu_thumb_add() {
    Emulator emulator;
    emulator.reset();

    auto& cpu = emulator.cpu();
    auto& state = cpu.state();
    state.cpsr = static_cast<u32>(CpuMode::System) | (1u << 5);
    state.regs[15] = 0x03000000u;

    auto iwram = emulator.bus().iwram();
    write16(iwram, 0, 0x2001u);  // MOVS r0, #1
    write16(iwram, 2, 0x3002u);  // ADDS r0, #2

    cpu.step();
    cpu.step();

    expect(state.regs[0] == 3u, "Thumb interpreter should execute MOVS/ADDS immediate");
}

}  // namespace

int main() {
    try {
        test_scheduler();
        test_irq_controller();
        test_bus_open_bus_and_alignment();
        test_cpu_misaligned_arm_ldr_rotation();
        test_hle_swi_halt_without_bios();
        test_cpu_thumb_register_offset_halfword_and_signed_loads();
        test_ppu_mode3_render_and_hash();
        test_ppu_mode4_render_hash();
        test_timer_overflow_irq();
        test_dma_immediate_copy();
        test_dma_hblank_irq();
        test_dma_vblank_trigger();
        test_audio_fifo_timer_cadence();
        test_input_registers();
        test_keypad_irq_or_and_modes();
        test_halt_wakeup_edge_cases();
        test_cpu_arm_add();
        test_cpu_thumb_add();
        std::cout << "All tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
