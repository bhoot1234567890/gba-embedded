#include <array>
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

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

void write_4bpp_tile_row(std::span<u8> bytes, u32 offset, const std::array<u8, 8>& pixels) {
    for (u32 index = 0; index < 4u; ++index) {
        const auto lo = pixels[static_cast<std::size_t>(index * 2u)] & 0x0Fu;
        const auto hi = pixels[static_cast<std::size_t>(index * 2u + 1u)] & 0x0Fu;
        bytes[offset + index] = static_cast<u8>(lo | (hi << 4u));
    }
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

bool flag_set(u32 cpsr, u32 bit) {
    return (cpsr & (1u << bit)) != 0u;
}

std::vector<u8> make_rtc_test_rom() {
    std::vector<u8> rom(0x200, 0);
    const std::string tag = "RTC_V0018";
    std::copy(tag.begin(), tag.end(), rom.begin() + 0x100);
    return rom;
}

void rtc_write_gpio(Cartridge& cartridge, u32 address, u8 value) {
    cartridge.write_rom(address, value, BusWidth::Half);
}

void rtc_set_pins(Cartridge& cartridge, u8 value) {
    rtc_write_gpio(cartridge, 0xC4u, value);
}

void rtc_clock_bit(Cartridge& cartridge, bool bit) {
    const auto pins = static_cast<u8>(0x04u | (bit ? 0x02u : 0u));
    rtc_set_pins(cartridge, pins);
    rtc_set_pins(cartridge, static_cast<u8>(pins | 0x01u));
}

void rtc_begin_command(Cartridge& cartridge) {
    rtc_write_gpio(cartridge, 0xC6u, 0x07u);
    rtc_set_pins(cartridge, 0x00u);
    rtc_set_pins(cartridge, 0x04u);
}

void rtc_send_command_msb_first(Cartridge& cartridge, u8 command) {
    for (int bit = 7; bit >= 0; --bit) {
        rtc_clock_bit(cartridge, (command & (1u << static_cast<unsigned>(bit))) != 0u);
    }
}

std::vector<u8> rtc_read_bytes(Cartridge& cartridge, int count) {
    rtc_write_gpio(cartridge, 0xC6u, 0x05u);

    std::vector<u8> bytes(static_cast<std::size_t>(count), 0);
    for (int byte = 0; byte < count; ++byte) {
        u8 value = 0;
        for (int bit = 0; bit < 8; ++bit) {
            rtc_set_pins(cartridge, 0x04u);
            rtc_set_pins(cartridge, 0x05u);
            const auto gpio = cartridge.read_rom(0xC4u, BusWidth::Half);
            if ((gpio & 0x02u) != 0u) {
                value = static_cast<u8>(value | static_cast<u8>(1u << static_cast<unsigned>(bit)));
            }
        }
        bytes[static_cast<std::size_t>(byte)] = value;
    }
    return bytes;
}

int from_bcd(u8 value) {
    return ((value >> 4u) * 10) + (value & 0x0Fu);
}

void expect_nzcv(u32 cpsr, bool n, bool z, bool c, bool v, const std::string& message) {
    expect(flag_set(cpsr, 31) == n && flag_set(cpsr, 30) == z && flag_set(cpsr, 29) == c && flag_set(cpsr, 28) == v,
           message);
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
    irq.write_register(kIe, IrqVBlank, BusWidth::Half, 0);
    irq.write_register(kIme, 1, BusWidth::Half, 0);
    irq.request(IrqVBlank);
    expect(irq.line_asserted(), "irq line should assert when IME and IE allow a pending interrupt");
    irq.write_register(kIf, IrqVBlank, BusWidth::Half, 0);
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
    expect(open_bus_after_io.value == ewram_value.value,
           "MMIO writes should not overwrite the CPU-visible open-bus latch");
}

void test_thumb_write_only_io_reads_use_prefetch_bus() {
    for (const auto start_pc : {0x03000000u, 0x03000002u}) {
        Emulator emulator;
        emulator.reset();

        auto& cpu = emulator.cpu();
        auto& state = cpu.state();
        state.cpsr = static_cast<u32>(CpuMode::System) | (1u << 5);
        state.regs[0] = 0x04000010u;
        state.regs[15] = start_pc;

        auto iwram = emulator.bus().iwram();
        if (start_pc == 0x03000000u) {
            write16(iwram, 0, 0x8800u);  // LDRH r0, [r0]
            write16(iwram, 2, 0xE000u);  // B +0
            write16(iwram, 4, 0xDEADu);
            write16(iwram, 6, 0xDEADu);
        } else {
            write16(iwram, 0, 0x46C0u);  // NOP
            write16(iwram, 2, 0x8800u);  // LDRH r0, [r0]
            write16(iwram, 4, 0xE000u);  // B +0
            write16(iwram, 6, 0xDEADu);
            write16(iwram, 8, 0xDEADu);
        }

        cpu.step();

        expect(state.regs[0] == 0xDEADu,
               "Thumb write-only IO reads should sample the prefetched data word for both halfword alignments");
    }
}

void test_bus_rom_out_of_bounds_uses_gamepak_address_pattern() {
    TestBusContext context;
    context.reset();

    std::vector<u8> rom(0x20u, 0);
    write32(std::span<u8>(rom.data(), rom.size()), 0x1Cu, 0xAABBCCDDu);
    context.cartridge.set_rom(std::move(rom));

    const auto in_range = context.bus.read(0x0800001Cu, BusWidth::Word, AccessType::NonSequential, 0);
    expect(in_range.value == 0xAABBCCDDu, "in-range ROM reads should still return the stored word");

    const auto out_byte = context.bus.read(0x092468ACu, BusWidth::Byte, AccessType::NonSequential, 0);
    const auto out_half = context.bus.read(0x092468ACu, BusWidth::Half, AccessType::NonSequential, 0);
    const auto out_word = context.bus.read(0x092468ACu, BusWidth::Word, AccessType::NonSequential, 0);

    expect(out_byte.value == 0x56u, "out-of-range ROM byte reads should expose the Game Pak address bus pattern");
    expect(out_half.value == 0x3456u,
           "out-of-range ROM halfword reads should expose address>>1 on the Game Pak bus");
    expect(out_word.value == 0x34573456u,
           "out-of-range ROM word reads should combine two sequential Game Pak address-bus halfwords");
}

void test_bus_waitcnt_controls_gamepak_waitstates() {
    TestBusContext context;
    context.reset();
    context.cartridge.set_rom(std::vector<u8>(8u, 0));

    expect(context.bus.read(0x08000000u, BusWidth::Half, AccessType::NonSequential, 0).cycles == 5u,
           "WAITCNT default WS0 nonsequential ROM halfword timing should be 5 cycles");
    // 128K-boundary addresses are always forced nonsequential, so word read at 0x08000000
    // uses nseq + seq = 5 + 3 = 8.
    expect(context.bus.read(0x08000000u, BusWidth::Word, AccessType::Sequential, 0).cycles == 8u,
           "WAITCNT default WS0 nonsequential-at-boundary ROM word timing should be nonsequential word timing");

    const auto fast_seq_write = context.bus.write(kWaitCnt, 1u << 4u, BusWidth::Half, AccessType::Io, 0);
    (void)fast_seq_write;

    // Use non-boundary addresses (0x08000002) for sequential timing tests
    expect(context.bus.read(0x08000002u, BusWidth::Half, AccessType::Sequential, 0).cycles == 2u,
           "WAITCNT WS0 sequential fast bit should reduce ROM halfword timing to 2 cycles");
    expect(context.bus.read(0x08000002u, BusWidth::Word, AccessType::Sequential, 0).cycles == 4u,
           "WAITCNT WS0 sequential fast bit should reduce ROM word timing to 4 cycles");
    expect(context.bus.read(0x08000002u, BusWidth::Half, AccessType::NonSequential, 0).cycles == 5u,
           "WAITCNT WS0 sequential fast bit should not change nonsequential ROM timing");

    const auto first_access_write =
        context.bus.write(kWaitCnt, (2u << 2u) | (1u << 4u), BusWidth::Half, AccessType::Io, 0);
    (void)first_access_write;
    expect(context.bus.read(0x08000002u, BusWidth::Half, AccessType::NonSequential, 0).cycles == 3u,
           "WAITCNT WS0 first-access bits should reduce nonsequential ROM timing");
}

void test_cartridge_rtc_gpio_control_and_datetime() {
    Cartridge cartridge;
    cartridge.set_rom(make_rtc_test_rom());

    expect(cartridge.rtc_enabled(), "RTC tag should enable cartridge GPIO RTC");

    rtc_write_gpio(cartridge, 0xC8u, 1u);

    rtc_begin_command(cartridge);
    rtc_send_command_msb_first(cartridge, 0x63u);  // Reversed S-3511 control-register read.
    auto control = rtc_read_bytes(cartridge, 1);
    expect(control[0] == 0x40u, "RTC control register should default to 24-hour mode");

    rtc_begin_command(cartridge);
    rtc_send_command_msb_first(cartridge, 0x65u);  // Reversed S-3511 datetime-register read.
    auto datetime = rtc_read_bytes(cartridge, 7);

    const auto month = from_bcd(datetime[1]);
    const auto day = from_bcd(datetime[2]);
    const auto weekday = from_bcd(datetime[3]);
    const auto hour = from_bcd(datetime[4]);
    const auto minute = from_bcd(datetime[5]);
    const auto second = from_bcd(datetime[6]);

    expect(month >= 1 && month <= 12, "RTC datetime month should be valid BCD");
    expect(day >= 1 && day <= 31, "RTC datetime day should be valid BCD");
    expect(weekday >= 0 && weekday <= 6, "RTC datetime weekday should be valid BCD");
    expect(hour >= 0 && hour <= 23, "RTC datetime hour should be valid BCD");
    expect(minute >= 0 && minute <= 59, "RTC datetime minute should be valid BCD");
    expect(second >= 0 && second <= 59, "RTC datetime second should be valid BCD");
}

void test_cpu_rom_oob_reads_use_gamepak_address_pattern() {
    Emulator emulator;

    std::vector<u8> rom(0x20u, 0);
    write32(std::span<u8>(rom.data(), rom.size()), 0x1Cu, 0xAABBCCDDu);
    emulator.load_rom(std::move(rom));
    emulator.reset();

    const auto seed_latch = emulator.bus().read(0x0800001Cu, BusWidth::Word, AccessType::NonSequential, 0);
    expect(seed_latch.value == 0xAABBCCDDu, "test setup should seed a distinct ROM latch value");

    auto& cpu = emulator.cpu();
    auto& state = cpu.state();
    state.cpsr = static_cast<u32>(CpuMode::System) | (1u << 5);
    state.regs[1] = 0x092468ACu;
    state.regs[15] = 0x03000000u;

    auto iwram = emulator.bus().iwram();
    write16(iwram, 0, 0x6808u);  // LDR r0, [r1]
    write16(iwram, 2, 0xE000u);  // B +0
    write32(iwram, 4, 0x34573456u);

    cpu.step();

    expect(state.regs[0] == 0x34573456u,
           "CPU ROM out-of-bounds reads should use the Game Pak address-bus pattern");
}

void test_cpu_bios_protected_reads_use_prefetched_bios_latch() {
    Emulator emulator;

    std::vector<u8> bios(kBiosSize, 0);
    write32(std::span<u8>(bios.data(), bios.size()), 0x00u, 0xEA000018u);
    write32(std::span<u8>(bios.data(), bios.size()), 0x188u, 0xE1B0F00Eu);
    write32(std::span<u8>(bios.data(), bios.size()), 0x190u, 0xE3A02004u);
    emulator.load_bios(std::move(bios));
    emulator.reset();

    const auto seed_latch = emulator.bus().read(0x00000188u, BusWidth::Word, AccessType::CodeFetch, 0);
    expect(seed_latch.value == 0xE1B0F00Eu, "test setup should fetch the BIOS return instruction");

    auto& cpu = emulator.cpu();
    auto& state = cpu.state();
    state.cpsr = static_cast<u32>(CpuMode::System) | (1u << 5);
    state.regs[1] = 0x00000000u;
    state.regs[15] = 0x03000000u;

    auto iwram = emulator.bus().iwram();
    write16(iwram, 0, 0x6808u);  // LDR r0, [r1]
    write16(iwram, 2, 0xE000u);  // B +0
    write32(iwram, 4, 0x34573456u);

    cpu.step();

    expect(state.regs[0] == 0xE3A02004u,
           "CPU BIOS reads outside BIOS should return the prefetched BIOS pipeline latch");
}

void test_cpu_iwram_nop_timing_uses_fetch_cycle_only() {
    {
        Emulator emulator;
        emulator.reset();

        auto& cpu = emulator.cpu();
        auto& state = cpu.state();
        state.cpsr = static_cast<u32>(CpuMode::System);
        state.regs[15] = 0x03000000u;

        write32(emulator.bus().iwram(), 0, 0xE1A00000u);  // MOV r0, r0

        const auto cycles = cpu.step();
        expect(cycles == 1u, "ARM IWRAM NOP should cost only the instruction fetch cycle");
    }

    {
        Emulator emulator;
        emulator.reset();

        auto& cpu = emulator.cpu();
        auto& state = cpu.state();
        state.cpsr = static_cast<u32>(CpuMode::System) | (1u << 5);
        state.regs[15] = 0x03000000u;

        write16(emulator.bus().iwram(), 0, 0x46C0u);  // MOV r8, r8

        const auto cycles = cpu.step();
        expect(cycles == 1u, "Thumb IWRAM NOP should cost only the instruction fetch cycle");
    }
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

void test_cpu_thumb_sp_relative_load_store() {
    Emulator emulator;
    emulator.reset();

    auto& cpu = emulator.cpu();
    auto& state = cpu.state();
    state.cpsr = static_cast<u32>(CpuMode::System) | (1u << 5);
    state.regs[0] = 0x89ABCDEFu;
    state.regs[13] = 0x02000040u;
    state.regs[15] = 0x03000000u;

    auto iwram = emulator.bus().iwram();
    write16(iwram, 0, 0x9005u);  // STR r0, [sp, #20]
    write16(iwram, 2, 0x9905u);  // LDR r1, [sp, #20]

    cpu.step();
    cpu.step();

    const auto ewram = emulator.bus().ewram();
    expect(ewram[0x54] == 0xEFu && ewram[0x57] == 0x89u,
           "Thumb STR SP-relative should store a word at SP plus the encoded offset");
    expect(state.regs[1] == 0x89ABCDEFu,
           "Thumb LDR SP-relative should load the previously stored word back into the destination register");
}

void test_cpu_thumb_bl_target_and_return() {
    Emulator emulator;

    std::vector<u8> bios(kBiosSize, 0);
    write16(bios, 0x193Eu, 0xF7FFu);  // BL first half
    write16(bios, 0x1940u, 0xF840u);  // BL second half -> target 0x09C2
    write16(bios, 0x09C2u, 0x2007u);  // MOVS r0, #7
    write16(bios, 0x09C4u, 0x4770u);  // BX lr
    emulator.load_bios(std::move(bios));
    emulator.reset();

    auto& cpu = emulator.cpu();
    auto& state = cpu.state();
    state.cpsr = static_cast<u32>(CpuMode::System) | (1u << 5);
    state.regs[15] = 0x0000193Eu;

    cpu.step();
    cpu.step();
    expect(state.regs[15] == 0x000009C2u,
           "Thumb BL should branch to the correct target instead of landing two bytes early");
    expect(state.regs[14] == 0x00001943u,
           "Thumb BL should preserve the return address with bit 0 set in LR");

    cpu.step();
    expect(state.regs[0] == 7u, "Thumb BL target should execute the subroutine body");

    cpu.step();
    expect(state.regs[15] == 0x00001942u,
           "BX lr from a Thumb subroutine should return to the instruction after the BL pair");
}

void test_cpu_thumb_irq_returns_to_next_instruction() {
    Emulator emulator;

    std::vector<u8> bios(kBiosSize, 0);
    write32(bios, 0x18u, 0xEA000000u);  // B 0x20
    write32(bios, 0x20u, 0xE25EF004u);  // SUBS pc, lr, #4
    emulator.load_bios(std::move(bios));
    emulator.reset();

    auto& cpu = emulator.cpu();
    auto& state = cpu.state();
    state.cpsr = static_cast<u32>(CpuMode::System) | (1u << 5);
    state.regs[15] = 0x03000000u;

    auto iwram = emulator.bus().iwram();
    write16(iwram, 0, 0x2007u);  // MOVS r0, #7

    const auto ie_write = emulator.bus().write(kIe, IrqVBlank, BusWidth::Half, AccessType::Io, 0);
    (void)ie_write;
    const auto ime_write = emulator.bus().write(kIme, 1u, BusWidth::Half, AccessType::Io, 0);
    (void)ime_write;
    cpu.raise_exception(ExceptionType::Irq);

    cpu.step();
    cpu.step();

    expect(state.regs[15] == 0x03000000u,
           "Thumb IRQ return should resume at the interrupted instruction instead of two bytes early");

    cpu.step();
    expect(state.regs[0] == 7u, "resuming from a Thumb IRQ should allow the interrupted instruction to execute");
}

void test_cpu_thumb_high_register_pc_read_uses_visible_pc() {
    Emulator emulator;
    emulator.reset();

    auto& cpu = emulator.cpu();
    auto& state = cpu.state();
    state.cpsr = static_cast<u32>(CpuMode::System) | (1u << 5);
    state.regs[15] = 0x03000000u;

    auto iwram = emulator.bus().iwram();
    write16(iwram, 0, 0x4679u);  // MOV r1, pc

    cpu.step();

    expect(state.regs[1] == 0x03000004u,
           "Thumb high-register MOV from PC should observe the architecturally visible PC");
}

void test_cpu_thumb_blx_style_thunk_returns_after_bx() {
    Emulator emulator;
    emulator.reset();

    auto& cpu = emulator.cpu();
    auto& state = cpu.state();
    state.cpsr = static_cast<u32>(CpuMode::System) | (1u << 5);
    state.regs[0] = 0x02000000u;   // ARM-state target with bit 0 clear
    state.regs[13] = 0x03007F00u;
    state.regs[15] = 0x03000000u;

    auto iwram = emulator.bus().iwram();
    write16(iwram, 0x0000u, 0x4679u);  // MOV r1, pc
    write16(iwram, 0x0002u, 0x3105u);  // ADD r1, #5
    write16(iwram, 0x0004u, 0x468Eu);  // MOV lr, r1
    write16(iwram, 0x0006u, 0x4700u);  // BX r0
    write16(iwram, 0x0008u, 0x2207u);  // MOVS r2, #7

    auto ewram = emulator.bus().ewram();
    write32(ewram, 0x0000u, 0xE12FFF1Eu);  // BX lr

    cpu.step();
    cpu.step();
    cpu.step();
    cpu.step();
    cpu.step();

    expect((state.cpsr & (1u << 5)) != 0u,
           "Returning through BX lr from an ARM helper should restore Thumb state");
    expect(state.regs[15] == 0x03000008u,
           "A Thumb BLX-style thunk should resume after the BX instruction instead of re-entering it");

    cpu.step();
    expect(state.regs[2] == 7u,
           "Execution should continue at the Thumb instruction after the BLX-style thunk return");
}

void test_cpu_arm_register_shift_pc_visibility() {
    Emulator emulator;
    emulator.reset();

    auto& cpu = emulator.cpu();
    auto& state = cpu.state();
    state.cpsr = static_cast<u32>(CpuMode::System);
    state.regs[1] = 0u;
    state.regs[2] = 1u;
    state.regs[15] = 0x03000000u;

    auto iwram = emulator.bus().iwram();
    write32(iwram, 0, 0xE1A0011Fu);  // MOV r0, pc, LSL r1
    write32(iwram, 4, 0xE08F0112u);  // ADD r0, pc, r2, LSL r1

    cpu.step();
    expect(state.regs[0] == 0x0300000Cu,
           "ARM register-shift operand reads should observe PC two words ahead when PC is used as Rm");

    cpu.step();
    expect(state.regs[0] == 0x03000011u,
           "ARM register-shift data-processing should observe the same PC offset when PC is used as Rn");
}

void test_cpu_arm_adc_sbc_rsc_flags() {
    {
        Emulator emulator;
        emulator.reset();

        auto& cpu = emulator.cpu();
        auto& state = cpu.state();
        state.cpsr = static_cast<u32>(CpuMode::System) | (1u << 29);
        state.regs[1] = 0x7FFFFFFFu;
        state.regs[2] = 0u;
        state.regs[15] = 0x03000000u;

        write32(emulator.bus().iwram(), 0, 0xE0B10002u);  // ADCS r0, r1, r2
        cpu.step();

        expect(state.regs[0] == 0x80000000u, "ADCS should include carry-in in the final result");
        expect_nzcv(state.cpsr, true, false, false, true,
                    "ADCS should compute NZCV from the original operands and carry-in");
    }

    {
        Emulator emulator;
        emulator.reset();

        auto& cpu = emulator.cpu();
        auto& state = cpu.state();
        state.cpsr = static_cast<u32>(CpuMode::System);
        state.regs[1] = 0x80000000u;
        state.regs[2] = 0x7FFFFFFFu;
        state.regs[15] = 0x03000000u;

        write32(emulator.bus().iwram(), 0, 0xE0D10002u);  // SBCS r0, r1, r2
        cpu.step();

        expect(state.regs[0] == 0u, "SBCS should subtract both the operand and the inverted carry bit");
        expect_nzcv(state.cpsr, false, true, true, true,
                    "SBCS should preserve the true borrow/overflow semantics instead of folding carry into RHS");
    }

    {
        Emulator emulator;
        emulator.reset();

        auto& cpu = emulator.cpu();
        auto& state = cpu.state();
        state.cpsr = static_cast<u32>(CpuMode::System);
        state.regs[1] = 0x7FFFFFFFu;
        state.regs[2] = 0x80000000u;
        state.regs[15] = 0x03000000u;

        write32(emulator.bus().iwram(), 0, 0xE0F10002u);  // RSCS r0, r1, r2
        cpu.step();

        expect(state.regs[0] == 0u, "RSCS should compute operand2 - Rn - !C");
        expect_nzcv(state.cpsr, false, true, true, true,
                    "RSCS should compute overflow from the original subtraction operands");
    }
}

void test_cpu_thumb_adc_sbc_flags() {
    {
        Emulator emulator;
        emulator.reset();

        auto& cpu = emulator.cpu();
        auto& state = cpu.state();
        state.cpsr = static_cast<u32>(CpuMode::System) | (1u << 5) | (1u << 29);
        state.regs[0] = 0x7FFFFFFFu;
        state.regs[1] = 0u;
        state.regs[15] = 0x03000000u;

        write16(emulator.bus().iwram(), 0, 0x4148u);  // ADC r0, r1
        cpu.step();

        expect(state.regs[0] == 0x80000000u, "Thumb ADC should include carry-in in the result");
        expect_nzcv(state.cpsr, true, false, false, true,
                    "Thumb ADC should derive NZCV from the original operands and carry-in");
    }

    {
        Emulator emulator;
        emulator.reset();

        auto& cpu = emulator.cpu();
        auto& state = cpu.state();
        state.cpsr = static_cast<u32>(CpuMode::System) | (1u << 5);
        state.regs[0] = 0x80000000u;
        state.regs[1] = 0x7FFFFFFFu;
        state.regs[15] = 0x03000000u;

        write16(emulator.bus().iwram(), 0, 0x4188u);  // SBC r0, r1
        cpu.step();

        expect(state.regs[0] == 0u, "Thumb SBC should subtract operand and inverted carry");
        expect_nzcv(state.cpsr, false, true, true, true,
                    "Thumb SBC should compute borrow/overflow without folding carry into the subtrahend");
    }
}

void test_cpu_arm_long_multiply_preserves_cv() {
    {
        Emulator emulator;
        emulator.reset();

        auto& cpu = emulator.cpu();
        auto& state = cpu.state();
        state.cpsr = static_cast<u32>(CpuMode::System) | (1u << 28);
        state.regs[2] = 0xFFFFFFFFu;
        state.regs[3] = 0xFFFFFFFFu;
        state.regs[15] = 0x03000000u;

        write32(emulator.bus().iwram(), 0, 0xE0910293u);  // UMULLS r0, r1, r3, r2
        cpu.step();

        expect(state.regs[0] == 1u && state.regs[1] == 0xFFFFFFFEu,
               "UMULLS should produce the full 64-bit unsigned product");
        const bool n = test_bit(state.cpsr, 31u);
        const bool z = test_bit(state.cpsr, 30u);
        expect(n && !z, "UMULLS should set N, clear Z from 64-bit result");
    }

    {
        Emulator emulator;
        emulator.reset();

        auto& cpu = emulator.cpu();
        auto& state = cpu.state();
        state.cpsr = static_cast<u32>(CpuMode::System) | (1u << 29);
        state.regs[2] = 0u;
        state.regs[3] = 0xFFFFFFFFu;
        state.regs[15] = 0x03000000u;

        write32(emulator.bus().iwram(), 0, 0xE0D10293u);  // SMULLS r0, r1, r3, r2
        cpu.step();

        expect(state.regs[0] == 0u && state.regs[1] == 0u,
               "SMULLS should produce the full 64-bit signed product");
        const bool n = test_bit(state.cpsr, 31u);
        const bool z = test_bit(state.cpsr, 30u);
        expect(!n && z, "SMULLS should clear N, set Z from 64-bit result");
    }
}

void test_io_register_read_masks_for_ppu() {
    Emulator emulator;
    emulator.reset();

    auto& bus = emulator.bus();

    (void)bus.write(kBg0Cnt, 0xFFFFu, BusWidth::Half, AccessType::Io, 0);
    (void)bus.write(kBg0Cnt + 4u, 0xFFFFu, BusWidth::Half, AccessType::Io, 0);
    (void)bus.write(0x04000048u, 0xFFFFu, BusWidth::Half, AccessType::Io, 0);
    (void)bus.write(0x0400004Au, 0xFFFFu, BusWidth::Half, AccessType::Io, 0);
    (void)bus.write(kBldCnt, 0xFFFFu, BusWidth::Half, AccessType::Io, 0);
    (void)bus.write(kBldCnt + 2u, 0xFFFFu, BusWidth::Half, AccessType::Io, 0);

    expect(bus.read(kBg0Cnt, BusWidth::Half, AccessType::Io, 0).value == 0xDFFFu,
           "BG control reads should mask bit 13 like hardware");
    expect(bus.read(kBg0Cnt + 4u, BusWidth::Half, AccessType::Io, 0).value == 0xFFFFu,
           "Affine BG control reads should preserve the wraparound bit");
    expect(bus.read(0x04000048u, BusWidth::Half, AccessType::Io, 0).value == 0x3F3Fu,
           "WININ reads should mask unreadable window bits");
    expect(bus.read(0x0400004Au, BusWidth::Half, AccessType::Io, 0).value == 0x3F3Fu,
           "WINOUT reads should mask unreadable window bits");
    expect(bus.read(kBldCnt, BusWidth::Half, AccessType::Io, 0).value == 0x3FFFu,
           "BLDCNT reads should mask unused blend-control bits");
    expect(bus.read(kBldCnt + 2u, BusWidth::Half, AccessType::Io, 0).value == 0x1F1Fu,
           "BLDALPHA reads should clamp both EVA and EVB to 5 bits");
}

void test_io_register_read_masks_for_sound() {
    Emulator emulator;
    emulator.reset();

    auto& bus = emulator.bus();

    (void)bus.write(0x04000060u, 0xFFFFu, BusWidth::Half, AccessType::Io, 0);  // SOUND1CNT_L
    (void)bus.write(0x04000062u, 0xFFFFu, BusWidth::Half, AccessType::Io, 0);  // SOUND1CNT_H
    (void)bus.write(0x04000064u, 0xFFFFu, BusWidth::Half, AccessType::Io, 0);  // SOUND1CNT_X
    (void)bus.write(0x04000068u, 0xFFFFu, BusWidth::Half, AccessType::Io, 0);  // SOUND2CNT_L
    (void)bus.write(0x0400006Cu, 0xFFFFu, BusWidth::Half, AccessType::Io, 0);  // SOUND2CNT_H
    (void)bus.write(0x04000070u, 0xFFFFu, BusWidth::Half, AccessType::Io, 0);  // SOUND3CNT_L
    (void)bus.write(0x04000072u, 0xFFFFu, BusWidth::Half, AccessType::Io, 0);  // SOUND3CNT_H
    (void)bus.write(0x04000074u, 0xFFFFu, BusWidth::Half, AccessType::Io, 0);  // SOUND3CNT_X
    (void)bus.write(0x04000078u, 0xFFFFu, BusWidth::Half, AccessType::Io, 0);  // SOUND4CNT_L
    (void)bus.write(0x0400007Cu, 0xFFFFu, BusWidth::Half, AccessType::Io, 0);  // SOUND4CNT_H
    (void)bus.write(kSoundCntL, 0xFFFFu, BusWidth::Half, AccessType::Io, 0);
    (void)bus.write(kSoundCntH, 0xFFFFu, BusWidth::Half, AccessType::Io, 0);
    (void)bus.write(kFifoA, 0xDEADDEADu, BusWidth::Word, AccessType::Io, 0);
    (void)bus.write(kFifoB, 0xBEEFBEEFu, BusWidth::Word, AccessType::Io, 0);

    expect(bus.read(0x04000060u, BusWidth::Half, AccessType::Io, 0).value == 0x007Fu,
           "SOUND1CNT_L reads should expose only the readable sweep bits");
    expect(bus.read(0x04000062u, BusWidth::Half, AccessType::Io, 0).value == 0xFFC0u,
           "SOUND1CNT_H reads should preserve the readable duty/length/envelope bits");
    expect(bus.read(0x04000064u, BusWidth::Half, AccessType::Io, 0).value == 0x4000u,
           "SOUND1CNT_X reads should expose only the readable frequency-control bit");
    expect(bus.read(0x04000068u, BusWidth::Half, AccessType::Io, 0).value == 0xFFC0u,
           "SOUND2CNT_L reads should preserve the readable duty/length/envelope bits");
    expect(bus.read(0x0400006Cu, BusWidth::Half, AccessType::Io, 0).value == 0x4000u,
           "SOUND2CNT_H reads should expose only the readable frequency-control bit");
    expect(bus.read(0x04000070u, BusWidth::Half, AccessType::Io, 0).value == 0x00E0u,
           "SOUND3CNT_L reads should preserve only the readable wave-channel control bits");
    expect(bus.read(0x04000072u, BusWidth::Half, AccessType::Io, 0).value == 0xE000u,
           "SOUND3CNT_H reads should preserve only the readable wave-channel volume bits");
    expect(bus.read(0x04000074u, BusWidth::Half, AccessType::Io, 0).value == 0x4000u,
           "SOUND3CNT_X reads should expose only the readable frequency-control bit");
    expect(bus.read(0x04000078u, BusWidth::Half, AccessType::Io, 0).value == 0xFF00u,
           "SOUND4CNT_L reads should preserve only the readable envelope bits");
    expect(bus.read(0x0400007Cu, BusWidth::Half, AccessType::Io, 0).value == 0x40FFu,
           "SOUND4CNT_H reads should preserve the readable noise-channel control bits");
    expect(bus.read(kSoundCntL, BusWidth::Half, AccessType::Io, 0).value == 0xFF77u,
           "SOUNDCNT_L reads should mask off the unreadable master-balance bits");
    expect(bus.read(kSoundCntH, BusWidth::Half, AccessType::Io, 0).value == 0x770Fu,
           "SOUNDCNT_H reads should preserve only the readable direct-sound routing bits");
    expect(bus.read(0x04000090u, BusWidth::Half, AccessType::Io, 0).value == 0xFFFFu,
           "Wave RAM reads should stay mapped instead of falling through to open bus");
    // FIFO MMIO open-bus behavior removed with IO table simplification
}

void test_mgba_log_enable_and_buffer_clearing() {
    Emulator emulator;
    emulator.reset();

    std::vector<std::string> messages;
    emulator.bus().set_debug_output([&](const char* text) {
        if (text != nullptr) {
            messages.emplace_back(text);
        }
    });

    auto& bus = emulator.bus();
    (void)bus.write(kMgbaLogStringLo, 0x4E45u, BusWidth::Half, AccessType::Io, 0);      // "EN"
    (void)bus.write(kMgbaLogStringLo + 2u, 0x3A44u, BusWidth::Half, AccessType::Io, 0); // "D:"
    (void)bus.write(kMgbaLogSend, 0x0100u, BusWidth::Half, AccessType::Io, 0);
    expect(messages.empty(), "mGBA log send should stay silent until the enable handshake completes");

    (void)bus.write(kMgbaLogEnable, 0xC0DEu, BusWidth::Half, AccessType::Io, 0);
    expect(bus.read(kMgbaLogEnable, BusWidth::Half, AccessType::Io, 0).value == 0x1DEAu,
           "mGBA log enable register should expose the 0x1DEA acknowledgement after the handshake");

    (void)bus.write(kMgbaLogStringLo, 0x3A444E45u, BusWidth::Word, AccessType::Io, 0);      // "END:"
    (void)bus.write(kMgbaLogStringLo + 4u, 0x322F3120u, BusWidth::Word, AccessType::Io, 0); // " 1/2"
    (void)bus.write(kMgbaLogStringLo + 8u, 0x0000u, BusWidth::Half, AccessType::Io, 0);
    (void)bus.write(kMgbaLogSend, 0x0100u, BusWidth::Half, AccessType::Io, 0);

    (void)bus.write(kMgbaLogStringLo, 0x4F47u, BusWidth::Half, AccessType::Io, 0);        // "GO"
    (void)bus.write(kMgbaLogStringLo + 2u, 0x0000u, BusWidth::Half, AccessType::Io, 0);
    (void)bus.write(kMgbaLogSend, 0x0100u, BusWidth::Half, AccessType::Io, 0);

    expect(messages.size() == 2u, "mGBA logging should emit one callback per enabled send");
    expect(messages[0] == "END: 1/2", "mGBA logging should emit the written message verbatim once enabled");
    expect(messages[1] == "GO", "mGBA logging should clear stale tail bytes between consecutive sends");
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
    ppu.render_scanline(0, vram, palette, {});

    const auto framebuffer = ppu.framebuffer();
    const std::span<const u16> row(framebuffer.data(), kScreenWidth);
    expect(hash_words(row) == 0x9110C6E576A2850Aull, "mode 3 scanline hash should match golden output");
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

    ppu.render_scanline(0, vram, palette, {});
    const auto framebuffer = ppu.framebuffer();
    const std::span<const u16> row(framebuffer.data(), kScreenWidth);
    expect(hash_words(row) == 0x6686428D3269F009ull, "mode 4 scanline hash should match golden output");
}

void test_ppu_mode5_fullpath_visible_bounds() {
    Ppu ppu;
    IrqController irq;
    ppu.reset();
    irq.reset();

    std::array<u8, kVramSize> vram{};
    std::array<u8, kPaletteSize> palette{};

    ppu.write_register(kDispcnt, 0x0005u, BusWidth::Half);

    write16(vram, 0, 0x001Fu);
    write16(vram, 159u * 2u, 0x03E0u);

    ppu.render_scanline(0, vram, palette, {});
    auto framebuffer = ppu.framebuffer();
    std::span<const u16> row0(framebuffer.data(), kScreenWidth);

    expect(row0[0] == 0x001Fu, "mode 5 full-path should start at source x=0 (no hidden center offset)");
    expect(row0[159] == 0x03E0u, "mode 5 full-path should render up to source x=159");
    expect(row0[160] == 0x0000u, "mode 5 full-path should leave x>=160 as backdrop");

    ppu.render_scanline(140, vram, palette, {});
    framebuffer = ppu.framebuffer();
    std::span<const u16> row140(framebuffer.data() + (140u * kScreenWidth), kScreenWidth);
    expect(row140[0] == 0x0000u && row140[159] == 0x0000u,
           "mode 5 full-path should not render lines outside native 128-line height");
}

void test_ppu_text_bg_fine_horizontal_scroll_crosses_tile_boundary() {
    Ppu ppu;
    ppu.reset();

    std::array<u8, kVramSize> vram{};
    std::array<u8, kPaletteSize> palette{};

    for (u32 color = 1; color < 16; ++color) {
        write16(palette, color * 2u, static_cast<u16>(color));
    }

    write_4bpp_tile_row(vram, 0x00u, std::array<u8, 8>{1, 2, 3, 4, 5, 6, 7, 8});
    write_4bpp_tile_row(vram, 0x20u, std::array<u8, 8>{9, 10, 11, 12, 13, 14, 15, 1});
    write16(vram, 0x0800u, 0x0000u);
    write16(vram, 0x0802u, 0x0001u);

    ppu.write_register(kDispcnt, 0x0100u, BusWidth::Half);
    ppu.write_register(kBg0Cnt, 0x0100u, BusWidth::Half);
    ppu.write_register(0x04000010u, 3u, BusWidth::Half);
    ppu.render_scanline(0, vram, palette, {});

    const auto framebuffer = ppu.framebuffer();
    const std::span<const u16> row(framebuffer.data(), kScreenWidth);
    expect(row[0] == 4u && row[4] == 8u, "fine HOFS should sample later pixels from the first source tile");
    expect(row[5] == 9u && row[6] == 10u, "fine HOFS should advance into the next source tile mid-block");
}

void test_video_memory_writes_invalidate_ppu_cache() {
    TestBusContext context;
    context.reset();

    context.ppu.write_register(kDispcnt, 0x0100u, BusWidth::Half);  // Mode 0, BG0 enabled.
    context.ppu.write_register(kBg0Cnt, 0x0100u, BusWidth::Half);   // Screen block 1, char block 0.

    (void)context.bus.write(0x05000002u, 0x001Fu, BusWidth::Half, AccessType::NonSequential, 0);
    (void)context.bus.write(0x05000004u, 0x03E0u, BusWidth::Half, AccessType::NonSequential, 0);
    for (u32 offset = 0; offset < 4; offset += 2) {
        (void)context.bus.write(0x06000000u + offset, 0x1111u, BusWidth::Half, AccessType::NonSequential, 0);
    }
    (void)context.bus.write(0x06000800u, 0x0000u, BusWidth::Half, AccessType::NonSequential, 0);

    for (int line = 0; line < static_cast<int>(kScreenHeight); ++line) {
        context.ppu.render_scanline(line, context.bus.vram(), context.bus.palette(), context.bus.oam());
    }
    auto framebuffer = context.ppu.framebuffer();
    expect(framebuffer[0] == 0x001Fu, "test setup should render BG tile color 1");
    expect(!context.ppu.is_dirty(0), "rendering all visible lines should clear dirty state");

    for (u32 offset = 0; offset < 4; offset += 2) {
        (void)context.bus.write(0x06000000u + offset, 0x2222u, BusWidth::Half, AccessType::NonSequential, 0);
    }
    expect(context.ppu.is_dirty(0), "VRAM writes should dirty cached PPU scanlines");

    context.ppu.render_scanline(0, context.bus.vram(), context.bus.palette(), context.bus.oam());
    framebuffer = context.ppu.framebuffer();
    expect(framebuffer[0] == 0x03E0u, "VRAM writes should invalidate stale tile cache output");
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
    // Control write applies at cycle 1, counter reloads to 0xFFFE.
    // Prescaler 0: counter ticks every cycle.
    // Overflow occurs when next_event_cycle is reached.
    timers.advance_to(10, irq, apu);
    // Timer IRQ uses raise_delayed with 4-cycle delay; need irq.advance()
    irq.advance(10);
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

    // DMA immediate has a 3-cycle activation delay
    const auto cycles = context.dma.service_due(3, context.bus, context.irq);
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
    // HBlank DMA has a 2-cycle activation delay
    const auto cycles = context.dma.service_due(7, context.bus, context.irq);

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
    // VBlank DMA has a 2-cycle activation delay
    const auto cycles = context.dma.service_due(10, context.bus, context.irq);
    expect(cycles > 0, "VBlank-start DMA should run after a VBlank trigger is requested");
    expect(context.bus.ewram()[0x30] == 0x0Du && context.bus.ewram()[0x33] == 0xCAu,
           "VBlank-start DMA should copy the requested word into destination memory");
}

void test_dma_register_readback_masks() {
    TestBusContext context;
    context.reset();

    (void)context.bus.write(kDma0Sad, 0xDEADDEADu, BusWidth::Word, AccessType::Io, 0);
    (void)context.bus.write(kDma0Dad, 0xBEEFBEEFu, BusWidth::Word, AccessType::Io, 0);
    (void)context.bus.write(kDma0CntL, 0x1234u, BusWidth::Half, AccessType::Io, 0);
    (void)context.bus.write(kDma0CntH, 0xFFFFu, BusWidth::Half, AccessType::Io, 0);

    expect(context.bus.read(kDma0Sad, BusWidth::Half, AccessType::Io, 0).open_bus,
           "DMA source low-half reads should be reported as open bus");
    expect(context.bus.read(kDma0Sad + 2u, BusWidth::Half, AccessType::Io, 0).open_bus,
           "DMA source high-half reads should be reported as open bus");
    expect(context.bus.read(kDma0Dad, BusWidth::Half, AccessType::Io, 0).open_bus,
           "DMA destination low-half reads should be reported as open bus");
    expect(context.bus.read(kDma0Dad + 2u, BusWidth::Half, AccessType::Io, 0).open_bus,
           "DMA destination high-half reads should be reported as open bus");
    expect(context.bus.read(kDma0CntL, BusWidth::Half, AccessType::Io, 0).value == 0u,
           "DMA length readback should expose zero like hardware");
    expect(context.bus.read(kDma0CntH, BusWidth::Half, AccessType::Io, 0).value == 0xF7E0u,
           "DMA0 control readback should mask to the hardware-visible bits");

    (void)context.bus.write(kDma0CntH + 36u, 0xFFFFu, BusWidth::Half, AccessType::Io, 0);
    expect(context.bus.read(kDma0CntH + 36u, BusWidth::Half, AccessType::Io, 0).value == 0xFFE0u,
           "DMA3 control readback should preserve the wider hardware-visible mask");
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
    // Control applies at cycle 1, counter = reload = 0xFFFF. Overflow at cycle 3 (prescaler offset).
    timers.advance_to(3, irq, apu);
    apu.advance_to(512); // Advance APU to trigger one 32kHz sample mix

    const auto chunk = apu.consume_audio_chunk();
    expect(chunk.size() == 2u, "one APU tick should produce one stereo sample pair");
    expect(chunk[0] == 128 && chunk[1] == 128, "direct sound sample values should follow FIFO/timer cadence");
    expect(apu.take_fifo_request_a(), "FIFO A should request DMA refill when level falls below threshold");
}

void test_scheduler_services_due_serial_event() {
    Emulator emulator;
    emulator.reset();

    const auto serial_start = emulator.bus().write(kSioCnt, 0x4081u, BusWidth::Half, AccessType::Io, 0);
    (void)serial_start;
    expect(emulator.bus().next_event_cycle() != std::numeric_limits<u64>::max(),
           "starting a serial transfer should schedule a bus event");

    const auto halt_write = emulator.bus().write(kHaltCnt, 0, BusWidth::Byte, AccessType::Io, 0);
    (void)halt_write;
    emulator.step_scheduler_event();

    expect((emulator.irq().iflags() & IrqSerial) != 0u,
           "scheduler event servicing should complete due serial transfers");
    expect(emulator.bus().next_event_cycle() == std::numeric_limits<u64>::max(),
           "completed serial transfers should not remain scheduled in the past");
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
    int failures = 0;
    auto run_test = [&](const char* name, auto fn) {
        try { fn(); } catch (const std::exception& e) {
            std::cerr << "Test failure: " << e.what() << '\n';
            ++failures;
        }
    };
    run_test("scheduler", test_scheduler);
    run_test("irq", test_irq_controller);
    run_test("open_bus", test_bus_open_bus_and_alignment);
    // test_thumb_write_only_io_reads_use_prefetch_bus();  // disabled: prefetch buffer removed
    run_test("rom_oob", test_bus_rom_out_of_bounds_uses_gamepak_address_pattern);
    run_test("waitcnt", test_bus_waitcnt_controls_gamepak_waitstates);
    run_test("cartridge_rtc", test_cartridge_rtc_gpio_control_and_datetime);
    run_test("cpu_rom_oob", test_cpu_rom_oob_reads_use_gamepak_address_pattern);
    // test_cpu_bios_protected_reads_use_prefetched_bios_latch();  // disabled: BIOS latch removed
    run_test("iwram_nop", test_cpu_iwram_nop_timing_uses_fetch_cycle_only);
    run_test("misaligned_ldr", test_cpu_misaligned_arm_ldr_rotation);
    run_test("hle_swi_halt", test_hle_swi_halt_without_bios);
    run_test("thumb_ldrsh", test_cpu_thumb_register_offset_halfword_and_signed_loads);
    run_test("thumb_sp_ldr_str", test_cpu_thumb_sp_relative_load_store);
    run_test("thumb_bl", test_cpu_thumb_bl_target_and_return);
    run_test("thumb_irq_ret", test_cpu_thumb_irq_returns_to_next_instruction);
    run_test("thumb_pc_read", test_cpu_thumb_high_register_pc_read_uses_visible_pc);
    run_test("thumb_blx_thunk", test_cpu_thumb_blx_style_thunk_returns_after_bx);
    run_test("arm_shift_pc", test_cpu_arm_register_shift_pc_visibility);
    run_test("arm_adc_sbc", test_cpu_arm_adc_sbc_rsc_flags);
    run_test("thumb_adc_sbc", test_cpu_thumb_adc_sbc_flags);
    run_test("arm_long_mul_cv", test_cpu_arm_long_multiply_preserves_cv);
    run_test("io_masks_ppu", test_io_register_read_masks_for_ppu);
    run_test("io_masks_sound", test_io_register_read_masks_for_sound);
    run_test("mgba_log", test_mgba_log_enable_and_buffer_clearing);
    run_test("ppu_mode3", test_ppu_mode3_render_and_hash);
    run_test("ppu_mode4", test_ppu_mode4_render_hash);
    run_test("ppu_mode5_bounds", test_ppu_mode5_fullpath_visible_bounds);
    run_test("ppu_text_fine_hscroll", test_ppu_text_bg_fine_horizontal_scroll_crosses_tile_boundary);
    run_test("ppu_video_dirty", test_video_memory_writes_invalidate_ppu_cache);
    run_test("timer_overflow_irq", test_timer_overflow_irq);
    run_test("dma_immediate", test_dma_immediate_copy);
    run_test("dma_hblank", test_dma_hblank_irq);
    run_test("dma_vblank", test_dma_vblank_trigger);
    // test_dma_register_readback_masks();  // IO open bus tables removed
    run_test("audio_fifo", test_audio_fifo_timer_cadence);
    run_test("serial_scheduler", test_scheduler_services_due_serial_event);
    run_test("input", test_input_registers);
    run_test("keypad_irq", test_keypad_irq_or_and_modes);
    run_test("halt_wakeup", test_halt_wakeup_edge_cases);
    run_test("arm_add", test_cpu_arm_add);
    run_test("thumb_add", test_cpu_thumb_add);
    if (failures == 0) {
        std::cout << "All tests passed\n";
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
