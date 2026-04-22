#include "gba/core/cpu.hpp"

#include <limits>

#ifndef GBA_PLATFORM_ESP32
#include <sstream>
#else
#include "esp_attr.h"
#endif

#include "gba/core/bus.hpp"
#include "gba/core/constants.hpp"
#include "gba/core/irq.hpp"

#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

namespace gba {

namespace {

constexpr u32 kFlagI = 1u << 7;
constexpr u32 kFlagF = 1u << 6;

[[nodiscard]] bool privileged_mode(CpuMode mode) {
    return mode != CpuMode::User;
}

[[nodiscard]] u32& spsr_for_mode(CpuState& state, CpuMode mode) {
    switch (mode) {
    case CpuMode::Supervisor:
        return state.spsr_svc;
    case CpuMode::Irq:
        return state.spsr_irq;
    case CpuMode::Abort:
        return state.spsr_abt;
    case CpuMode::Undefined:
        return state.spsr_und;
    case CpuMode::Fiq:
        return state.spsr_fiq;
    case CpuMode::User:
    case CpuMode::System:
        return state.spsr_svc;
    }
    return state.spsr_svc;
}

struct ShiftResult {
    u32 value = 0;
    bool carry = false;
};

[[nodiscard]] ShiftResult shift_lsl(u32 value, u32 amount, bool carry_in) {
    if (amount == 0) {
        return {value, carry_in};
    }
    if (amount < 32) {
        return {value << amount, test_bit(value, 32u - amount)};
    }
    if (amount == 32) {
        return {0, test_bit(value, 0)};
    }
    return {0, false};
}

[[nodiscard]] ShiftResult shift_lsr(u32 value, u32 amount, bool carry_in, bool immediate) {
    if (amount == 0) {
        if (immediate) {
            return {0, test_bit(value, 31)};
        }
        return {value, carry_in};
    }
    if (amount < 32) {
        return {value >> amount, test_bit(value, amount - 1u)};
    }
    if (amount == 32) {
        return {0, test_bit(value, 31)};
    }
    return {0, false};
}

[[nodiscard]] ShiftResult shift_asr(u32 value, u32 amount, bool carry_in, bool immediate) {
    if (amount == 0) {
        if (immediate) {
            const auto fill = test_bit(value, 31) ? 0xFFFFFFFFu : 0u;
            return {fill, test_bit(value, 31)};
        }
        return {value, carry_in};
    }
    if (amount < 32) {
        return {static_cast<u32>(static_cast<s32>(value) >> amount), test_bit(value, amount - 1u)};
    }
    const auto fill = test_bit(value, 31) ? 0xFFFFFFFFu : 0u;
    return {fill, test_bit(value, 31)};
}

[[nodiscard]] ShiftResult shift_ror(u32 value, u32 amount, bool carry_in, bool immediate) {
    if (amount == 0) {
        if (immediate) {
            const auto result = (carry_in ? 0x80000000u : 0u) | (value >> 1u);
            return {result, test_bit(value, 0)};
        }
        return {value, carry_in};
    }
    const auto rotate = amount & 31u;
    if (rotate == 0) {
        return {value, test_bit(value, 31)};
    }
    const auto result = rotate_right(value, rotate);
    return {result, test_bit(result, 31)};
}

[[nodiscard]] ShiftResult apply_shift(u32 value, u32 type, u32 amount, bool carry_in, bool immediate) {
    switch (type) {
    case 0:
        return shift_lsl(value, amount, carry_in);
    case 1:
        return shift_lsr(value, amount, carry_in, immediate);
    case 2:
        return shift_asr(value, amount, carry_in, immediate);
    case 3:
    default:
        return shift_ror(value, amount, carry_in, immediate);
    }
}

// Booth-encoding carry calculation for multiply long (from NanoBoyAdvance / zaydlang, calc84maniac)
[[nodiscard]] bool multiply_carry_lo(u32 multiplicand, u32 multiplier, u32 accum = 0) {
    multiplicand |= 1;  // low bit cannot propagate to carry
    u32 booth = static_cast<u32>(static_cast<s32>(multiplier << 31) >> 31);
    u32 carry = multiplicand * booth;
    u32 sum = carry + accum;
    int shift = 29;
    do {
        for (int i = 0; i < 4; i++, shift -= 2) {
            u32 next_booth = static_cast<u32>(static_cast<s32>(multiplier << shift) >> shift);
            u32 factor = next_booth - booth;
            booth = next_booth;
            u32 addend = multiplicand * factor;
            accum ^= carry ^ addend;
            sum += addend;
            carry = sum - accum;
        }
    } while (booth != multiplier);
    return (carry >> 31) != 0;
}

template<bool sign_extend>
[[nodiscard]] bool multiply_carry_hi(u32 multiplicand, u32 multiplier, u32 accum_hi = 0) {
    u32 mp, mr;
    if constexpr (sign_extend) {
        mp = static_cast<u32>(static_cast<s32>(multiplicand) >> 6);
        mr = static_cast<u32>(static_cast<s32>(multiplier) >> 26);
    } else {
        mp = multiplicand >> 6;
        mr = multiplier >> 26;
    }
    mp |= 1;  // low bit cannot propagate to carry
    u32 carry = ~accum_hi & 0x20000000u;
    u32 accum = accum_hi - 0x08000000u;
    u32 booth0 = static_cast<u32>(static_cast<s32>(mr << 27) >> 27);
    u32 booth1 = static_cast<u32>(static_cast<s32>(mr << 29) >> 29);
    u32 booth2 = static_cast<u32>(static_cast<s32>(mr << 31) >> 31);
    u32 factor0 = mr - booth0;
    u32 factor1 = booth0 - booth1;
    u32 factor2 = booth1 - booth2;
    u32 addend = mp * factor2;
    accum -= addend & 0x10000000u;
    addend = mp * factor1;
    accum -= addend & 0x40000000u;
    u32 sum = accum + (addend & 0x20000000u);
    accum -= carry;
    addend = mp * factor0;
    sum += addend & 0x40000000u;
    return ((sum ^ accum) >> 31) != 0;
}

}  // namespace

Arm7tdmi::Arm7tdmi(Bus& bus, IrqController& irq, TraceLogger* logger)
    : bus_(bus), irq_(irq), logger_(logger) {}

void Arm7tdmi::reset() {
    state_ = {};
    state_.cpsr = static_cast<u32>(CpuMode::Supervisor) | kFlagI | kFlagF;
    state_.next_fetch_access = AccessType::CodeFetch;
    current_cycle_ = 0;

    hle_swi_enabled_ = true;
    if (bus_.has_bios()) {
        // The bundled stub BIOS keeps SWI vector 0x08 as an infinite loop.
        // When a real BIOS is present, defer SWI behavior to BIOS code.
        const auto swi_vector = bus_.read(0x00000008u, BusWidth::Word, AccessType::CodeFetch, 0).value;
        hle_swi_enabled_ = swi_vector == 0xEAFFFFFEu;
    }
}

CpuState& Arm7tdmi::state() {
    return state_;
}

const CpuState& Arm7tdmi::state() const {
    return state_;
}

u64 Arm7tdmi::current_cycle() const {
    return current_cycle_;
}

void Arm7tdmi::set_current_cycle(u64 cycle) {
    current_cycle_ = cycle;
}

u64 IRAM_ATTR Arm7tdmi::cpu_run_until(u64 target_cycle) {
    while (current_cycle_ < target_cycle) {
        /* Inline step() to avoid per-instruction function call overhead */
        bus_.service_timers(current_cycle_);
        current_cycle_ += bus_.service_dma(current_cycle_);
        irq_.advance(current_cycle_);

        if (state_.halted || bus_.halted()) {
            state_.halted = true;
            const auto halt_wake_pending = static_cast<u16>(irq_.ie() & irq_.iflags()) != 0u;
            if (halt_wake_pending) {
                state_.halted = false;
                bus_.clear_halt();
            } else {
                ++current_cycle_;
                continue;
            }
        }

        if (irq_.line_asserted() && !test_bit(state_.cpsr, 7)) {
            enter_exception(ExceptionType::Irq, CpuMode::Irq, 0x18u, true, false,
                            state_.regs[15] + 4u);
            current_cycle_ += 3;
            continue;
        }

        if (thumb_state()) {
            const auto instruction = fetch_thumb();
            execute_thumb(instruction);
        } else {
            const auto instruction = fetch_arm();
            execute_arm(instruction);
        }
    }
    return current_cycle_;
}

u32 Arm7tdmi::step() {
    const auto start_cycle = current_cycle_;
    bus_.service_timers(current_cycle_);
    current_cycle_ += bus_.service_dma(current_cycle_);

    if (state_.halted || bus_.halted()) {
        state_.halted = true;
        const auto halt_wake_pending = static_cast<u16>(irq_.ie() & irq_.iflags()) != 0u;
        if (halt_wake_pending) {
            state_.halted = false;
            bus_.clear_halt();
        } else {
            ++current_cycle_;
            return static_cast<u32>(current_cycle_ - start_cycle);
        }
    }

    if (irq_.line_asserted() && !test_bit(state_.cpsr, 7)) {
        enter_exception(ExceptionType::Irq, CpuMode::Irq, 0x18u, true, false,
                        state_.regs[15] + 4u);
        current_cycle_ += 3;
        return static_cast<u32>(current_cycle_ - start_cycle);
    }

    if (thumb_state()) {
        const auto instruction = fetch_thumb();
        execute_thumb(instruction);
    } else {
        const auto instruction = fetch_arm();
        execute_arm(instruction);
    }

    return static_cast<u32>(current_cycle_ - start_cycle);
}

void Arm7tdmi::raise_exception(ExceptionType type) {
    switch (type) {
    case ExceptionType::Reset:
        enter_exception(type, CpuMode::Supervisor, 0x00u, true, true, 0);
        break;
    case ExceptionType::Undefined:
        enter_exception(type, CpuMode::Undefined, 0x04u, true, false,
                        state_.regs[15] + (thumb_state() ? 2u : 4u));
        break;
    case ExceptionType::SoftwareInterrupt:
        enter_exception(type, CpuMode::Supervisor, 0x08u, true, false, state_.regs[15]);
        break;
    case ExceptionType::PrefetchAbort:
        enter_exception(type, CpuMode::Abort, 0x0Cu, true, false, state_.regs[15] + 4u);
        break;
    case ExceptionType::DataAbort:
        enter_exception(type, CpuMode::Abort, 0x10u, true, false, state_.regs[15] + 8u);
        break;
    case ExceptionType::Irq:
        enter_exception(type, CpuMode::Irq, 0x18u, true, false, state_.regs[15] + 4u);
        break;
    case ExceptionType::Fiq:
        enter_exception(type, CpuMode::Fiq, 0x1Cu, true, true, state_.regs[15] + 4u);
        break;
    }
}

bool Arm7tdmi::thumb_state() const {
    return test_bit(state_.cpsr, 5);
}

CpuMode Arm7tdmi::mode() const {
    return static_cast<CpuMode>(state_.cpsr & 0x1Fu);
}

bool Arm7tdmi::condition_passed(u32 condition) const {
    const auto n = test_bit(state_.cpsr, 31);
    const auto z = test_bit(state_.cpsr, 30);
    const auto c = test_bit(state_.cpsr, 29);
    const auto v = test_bit(state_.cpsr, 28);

    switch (condition) {
    case 0x0:
        return z;
    case 0x1:
        return !z;
    case 0x2:
        return c;
    case 0x3:
        return !c;
    case 0x4:
        return n;
    case 0x5:
        return !n;
    case 0x6:
        return v;
    case 0x7:
        return !v;
    case 0x8:
        return c && !z;
    case 0x9:
        return !c || z;
    case 0xA:
        return n == v;
    case 0xB:
        return n != v;
    case 0xC:
        return !z && (n == v);
    case 0xD:
        return z || (n != v);
    case 0xE:
        return true;
    default:
        return false;
    }
}

u32 Arm7tdmi::fetch_arm() {
    const auto result = bus_.read(state_.regs[15], BusWidth::Word, state_.next_fetch_access, current_cycle_);
    current_cycle_ += result.cycles;
    state_.regs[15] += 4u;
    state_.next_fetch_access = AccessType::CodeFetch | AccessType::Sequential;
    return result.value;
}

u16 Arm7tdmi::fetch_thumb() {
    const auto result = bus_.read(state_.regs[15], BusWidth::Half, state_.next_fetch_access, current_cycle_);
    current_cycle_ += result.cycles;
    state_.regs[15] += 2u;
    state_.next_fetch_access = AccessType::CodeFetch | AccessType::Sequential;
    return static_cast<u16>(result.value & 0xFFFFu);
}

u32 Arm7tdmi::pc_visible() const {
    return state_.regs[15] + (thumb_state() ? 2u : 4u);
}

bool Arm7tdmi::handle_hle_swi(u32 comment) {
    if (!hle_swi_enabled_) {
        return false;
    }

    switch (comment) {
    case 0x02u: {
        const auto halt_result = bus_.write(kHaltCnt, 0, BusWidth::Byte, AccessType::Io, current_cycle_);
        current_cycle_ += halt_result.cycles;
        return true;
    }
    case 0x05u: {
        auto ie_result = bus_.read(kIe, BusWidth::Half, AccessType::Io, current_cycle_);
        current_cycle_ += ie_result.cycles;
        auto ie = static_cast<u16>(ie_result.value & 0xFFFFu);
        assign_bit(ie, 0u, true);
        auto write_ie = bus_.write(kIe, ie, BusWidth::Half, AccessType::Io, current_cycle_);
        current_cycle_ += write_ie.cycles;
        auto ime_result = bus_.read(kIme, BusWidth::Half, AccessType::Io, current_cycle_);
        current_cycle_ += ime_result.cycles;
        auto ime = static_cast<u16>(ime_result.value & 0xFFFFu);
        if (!ime) {
            auto write_ime = bus_.write(kIme, 1, BusWidth::Half, AccessType::Io, current_cycle_);
            current_cycle_ += write_ime.cycles;
        }
        auto halt_result = bus_.write(kHaltCnt, 0, BusWidth::Byte, AccessType::Io, current_cycle_);
        current_cycle_ += halt_result.cycles;
        state_.regs[0] = 1;
        state_.regs[1] = 1;
        return true;
    }
    case 0x0Bu: {
        auto src = state_.regs[0];
        auto dst = state_.regs[1];
        auto ctrl = state_.regs[2];
        const bool fill = (ctrl & 0x01000000u) != 0;
        const bool word_size = (ctrl & 0x04000000u) != 0;
        const u32 count = ctrl & 0x001FFFFFu;
        if (count == 0) {
            return true;
        }

        if (word_size) {
            src &= ~0x3u;
            dst &= ~0x3u;
        } else {
            src &= ~0x1u;
            dst &= ~0x1u;
        }

        u32 fill_value = 0;
        if (fill) {
            const auto fill_width = word_size ? BusWidth::Word : BusWidth::Half;
            auto r = bus_.read(src, fill_width, AccessType::NonSequential, current_cycle_);
            current_cycle_ += r.cycles;
            fill_value = r.value;
        }

        for (u32 i = 0; i < count; ++i) {
            if (word_size) {
                const auto value = fill ? fill_value : [&] {
                    auto r = bus_.read(src, BusWidth::Word, AccessType::NonSequential, current_cycle_);
                    current_cycle_ += r.cycles;
                    return r.value;
                }();
                auto w = bus_.write(dst, value, BusWidth::Word, AccessType::NonSequential, current_cycle_);
                current_cycle_ += w.cycles;
                if (!fill) src += 4;
                dst += 4;
            } else {
                const auto value = fill ? fill_value : [&] {
                    auto r = bus_.read(src, BusWidth::Half, AccessType::NonSequential, current_cycle_);
                    current_cycle_ += r.cycles;
                    return r.value;
                }();
                auto w = bus_.write(dst, value, BusWidth::Half, AccessType::NonSequential, current_cycle_);
                current_cycle_ += w.cycles;
                if (!fill) src += 2;
                dst += 2;
            }
        }
        return true;
    }
    case 0x0Cu: {
        auto src = state_.regs[0];
        auto dst = state_.regs[1];
        auto ctrl = state_.regs[2];
        const bool fill = (ctrl & 0x01000000u) != 0;
        const u32 count = ctrl & 0x001FFFFFu;
        if (count == 0) {
            return true;
        }

        src &= ~0x3u;
        dst &= ~0x3u;

        u32 fill_value = 0;
        if (fill) {
            auto r = bus_.read(src, BusWidth::Word, AccessType::NonSequential, current_cycle_);
            current_cycle_ += r.cycles;
            fill_value = r.value;
        }

        const auto words = count * 8u;
        for (u32 i = 0; i < words; ++i) {
            const auto value = fill ? fill_value : [&] {
                auto r = bus_.read(src, BusWidth::Word, AccessType::NonSequential, current_cycle_);
                current_cycle_ += r.cycles;
                return r.value;
            }();
            auto w = bus_.write(dst, value, BusWidth::Word, AccessType::NonSequential, current_cycle_);
            current_cycle_ += w.cycles;
            if (!fill) src += 4;
            dst += 4;
        }
        return true;
    }
    default:
        return false;
    }
}

u32 IRAM_ATTR Arm7tdmi::execute_arm(u32 instruction) {
    const auto condition = instruction >> 28u;
    if (!condition_passed(condition)) {
        return 1;
    }

    const auto carry_in = test_bit(state_.cpsr, 29);
    const auto read_reg = [&](u32 index) -> u32 { return index == 15u ? pc_visible() : state_.regs[index]; };
    const auto write_pc = [&](u32 value) {
        branch_to(value & ~0x3u, false);
    };
    const auto data_access = [&](AccessType access = AccessType::NonSequential) -> AccessType {
        const auto current_instruction = state_.regs[15] - 4u;
        if (current_instruction >= kBiosSize) {
            access |= AccessType::CpuOutsideBios;
        }
        return access;
    };
    const auto arm_open_bus_word = [&]() -> u32 { return bus_.peek_word(pc_visible()); };
    const auto read8 = [&](u32 address) -> u32 {
        const auto result = bus_.read(address, BusWidth::Byte, data_access(), current_cycle_);
        current_cycle_ += result.cycles;
        if (result.open_bus) {
            return (arm_open_bus_word() >> ((address & 3u) * 8u)) & 0xFFu;
        }
        return result.value & 0xFFu;
    };
    const auto read16 = [&](u32 address) -> u32 {
        const auto result = bus_.read(address, BusWidth::Half, data_access(), current_cycle_);
        current_cycle_ += result.cycles;
        const auto value =
            result.open_bus ? ((arm_open_bus_word() >> ((address & 2u) * 8u)) & 0xFFFFu)
                            : (result.value & 0xFFFFu);
        if ((address & 1u) != 0) {
            return rotate_right(value, 8u);
        }
        return value;
    };
    const auto read32 = [&](u32 address) -> u32 {
        const auto result = bus_.read(address, BusWidth::Word, data_access(), current_cycle_);
        current_cycle_ += result.cycles;
        const auto value = result.open_bus ? arm_open_bus_word() : result.value;
        return rotate_right(value, (address & 3u) * 8u);
    };
    const auto write8 = [&](u32 address, u32 value) {
        const auto result = bus_.write(address, value, BusWidth::Byte, AccessType::NonSequential, current_cycle_);
        current_cycle_ += result.cycles;
        current_cycle_ += bus_.service_dma(current_cycle_);
    };
    const auto write16 = [&](u32 address, u32 value) {
        const auto result = bus_.write(address, value, BusWidth::Half, AccessType::NonSequential, current_cycle_);
        current_cycle_ += result.cycles;
        current_cycle_ += bus_.service_dma(current_cycle_);
    };
    const auto write32 = [&](u32 address, u32 value) {
        const auto result = bus_.write(address, value, BusWidth::Word, AccessType::NonSequential, current_cycle_);
        current_cycle_ += result.cycles;
        current_cycle_ += bus_.service_dma(current_cycle_);
    };

    if ((instruction & 0x0FFFFFF0u) == 0x012FFF10u) {
        branch_to(read_reg(instruction & 0xFu), test_bit(read_reg(instruction & 0xFu), 0));
        current_cycle_ += 3;
        return 0;
    }

    if ((instruction & 0x0FC000F0u) == 0x00000090u) {
        const auto accumulate = test_bit(instruction, 21);
        const auto set_flags = test_bit(instruction, 20);
        const auto rd = (instruction >> 16u) & 0xFu;
        const auto rn = (instruction >> 12u) & 0xFu;
        const auto rs = (instruction >> 8u) & 0xFu;
        const auto rm = instruction & 0xFu;
        const auto product = read_reg(rm) * read_reg(rs);
        const auto result = accumulate ? product + read_reg(rn) : product;
        if (rd == 15u) {
            write_pc(result);
        } else {
            state_.regs[rd] = result;
        }
        if (set_flags) {
            update_nz(result);
        }
        current_cycle_ += 2;
        return 0;
    }

    if ((instruction & 0x0F8000F0u) == 0x00800090u) {
        const auto accumulate = test_bit(instruction, 21);
        const auto set_flags = test_bit(instruction, 20);
        const auto sign_extend_mul = test_bit(instruction, 22);
        const auto rd_hi = (instruction >> 16u) & 0xFu;
        const auto rd_lo = (instruction >> 12u) & 0xFu;
        const auto rs = (instruction >> 8u) & 0xFu;
        const auto rm = instruction & 0xFu;

        const auto lhs = read_reg(rm);
        const auto rhs = read_reg(rs);

        u64 product;
        if (sign_extend_mul) {
            product = static_cast<u64>(static_cast<s64>(sign_extend<32>(lhs)))
                                       * static_cast<s64>(sign_extend<32>(rhs));
        } else {
            product = static_cast<u64>(lhs) * static_cast<u64>(rhs);
        }

        u32 accum_lo = 0;
        u32 accum_hi = 0;
        u64 result;
        if (accumulate) {
            accum_lo = state_.regs[rd_lo];
            accum_hi = state_.regs[rd_hi];
            result = product + ((static_cast<u64>(accum_hi) << 32u) | static_cast<u64>(accum_lo));
        } else {
            result = product;
        }

        state_.regs[rd_lo] = static_cast<u32>(result);
        state_.regs[rd_hi] = static_cast<u32>(result >> 32u);

        if (set_flags) {
            assign_bit(state_.cpsr, 31, (result >> 63u) != 0);
            assign_bit(state_.cpsr, 30, result == 0u);
            // Booth-encoding carry calculation (from NanoBoyAdvance / zaydlang, calc84maniac)
            const auto carry = sign_extend_mul
                ? multiply_carry_hi<true>(lhs, rhs, accum_hi)
                : multiply_carry_hi<false>(lhs, rhs, accum_hi);
            assign_bit(state_.cpsr, 29, carry);
        }

        current_cycle_ += 3;
        return 0;
    }

    if ((instruction & 0x0E000090u) == 0x00000090u) {
        const auto pre = test_bit(instruction, 24);
        const auto up = test_bit(instruction, 23);
        const auto immediate = test_bit(instruction, 22);
        const auto write_back = test_bit(instruction, 21);
        const auto load = test_bit(instruction, 20);
        const auto rn = (instruction >> 16u) & 0xFu;
        const auto rd = (instruction >> 12u) & 0xFu;
        const auto sh = (instruction >> 5u) & 0x3u;
        const auto offset = immediate ? (((instruction >> 8u) & 0xFu) << 4u) | (instruction & 0xFu)
                                      : read_reg(instruction & 0xFu);

        const auto base = read_reg(rn);
        auto address = pre ? (up ? base + offset : base - offset) : base;

        if (load) {
            u32 value = 0;
            if (sh == 1u) {
                value = read16(address);
            } else if (sh == 2u) {
                value = static_cast<u32>(sign_extend<8>(read8(address)));
            } else {
                value = (address & 1u) != 0 ? static_cast<u32>(sign_extend<8>(read8(address)))
                                            : static_cast<u32>(sign_extend<16>(read16(address)));
            }

            if (rd == 15u) {
                write_pc(value);
            } else {
                state_.regs[rd] = value;
            }
            ++current_cycle_;
        } else if (sh == 1u) {
            write16(address, read_reg(rd));
        }

        if (!pre) {
            address = up ? base + offset : base - offset;
        }
        if (write_back || !pre) {
            state_.regs[rn] = address;
        }
        return 0;
    }

    if ((instruction & 0x0FBF0FFFu) == 0x010F0000u) {
        const auto rd = (instruction >> 12u) & 0xFu;
        state_.regs[rd] = test_bit(instruction, 22) ? spsr_for_mode(state_, mode()) : state_.cpsr;
        return 0;
    }

    if ((instruction & 0x0DB0F000u) == 0x0120F000u || (instruction & 0x0FB0F000u) == 0x0320F000u) {
        const auto use_imm = test_bit(instruction, 25);
        const auto field_mask = (instruction >> 16u) & 0xFu;
        u32 value = 0;
        if (use_imm) {
            const auto imm = instruction & 0xFFu;
            const auto rotate = ((instruction >> 8u) & 0xFu) * 2u;
            value = rotate_right(imm, rotate);
        } else {
            value = read_reg(instruction & 0xFu);
        }

        u32 mask = 0;
        if (test_bit(field_mask, 0)) {
            mask |= 0x000000FFu;
        }
        if (test_bit(field_mask, 1)) {
            mask |= 0x0000FF00u;
        }
        if (test_bit(field_mask, 2)) {
            mask |= 0x00FF0000u;
        }
        if (test_bit(field_mask, 3)) {
            mask |= 0xFF000000u;
        }

        if (test_bit(instruction, 22) && privileged_mode(mode())) {
            auto& spsr = spsr_for_mode(state_, mode());
            spsr = (spsr & ~mask) | (value & mask);
        } else if (privileged_mode(mode())) {
            const auto old_mode = mode();
            const auto new_cpsr = (state_.cpsr & ~mask) | (value & mask);
            const auto new_mode_val = static_cast<CpuMode>(new_cpsr & 0x1Fu);
            if (new_mode_val != old_mode) {
                state_.cpsr = (state_.cpsr & ~0x1Fu) | static_cast<u32>(old_mode);
                switch_mode(new_mode_val);
            }
            state_.cpsr = new_cpsr;
        }
        return 0;
    }

    if (((instruction >> 25u) & 0x7u) == 0x5u) {
        const auto offset = sign_extend<26>((instruction & 0x00FFFFFFu) << 2u);
        if (test_bit(instruction, 24)) {
            state_.regs[14] = state_.regs[15];
        }
        branch_to(pc_visible() + static_cast<u32>(offset), false);
        current_cycle_ += 2;
        return 0;
    }

    if (((instruction >> 26u) & 0x3u) == 0x1u) {
        const auto immediate = test_bit(instruction, 25);
        const auto pre = test_bit(instruction, 24);
        const auto up = test_bit(instruction, 23);
        const auto byte = test_bit(instruction, 22);
        const auto write_back = test_bit(instruction, 21);
        const auto load = test_bit(instruction, 20);
        const auto rn = (instruction >> 16u) & 0xFu;
        const auto rd = (instruction >> 12u) & 0xFu;

        u32 offset = 0;
        if (immediate) {
            const auto rm = instruction & 0xFu;
            const auto shift_type = (instruction >> 5u) & 0x3u;
            const auto shift_imm = (instruction >> 7u) & 0x1Fu;
            offset = apply_shift(read_reg(rm), shift_type, shift_imm, carry_in, true).value;
        } else {
            offset = instruction & 0x0FFFu;
        }

        const auto base = read_reg(rn);
        auto address = pre ? (up ? base + offset : base - offset) : base;

        if (load) {
            const auto value = byte ? read8(address) : read32(address);
            if (rd == 15u) {
                write_pc(value);
            } else {
                state_.regs[rd] = value;
            }
            ++current_cycle_;
        } else {
            const auto value = read_reg(rd);
            if (byte) {
                write8(address, value);
            } else {
                write32(address, value);
            }
        }

        if (!pre) {
            address = up ? base + offset : base - offset;
        }
        if ((write_back || !pre) && !(load && rd == rn)) {
            state_.regs[rn] = address;
        }
        return 0;
    }

    if (((instruction >> 25u) & 0x7u) == 0x4u) {
        const auto pre = test_bit(instruction, 24);
        const auto up = test_bit(instruction, 23);
        const auto write_back = test_bit(instruction, 21);
        const auto load = test_bit(instruction, 20);
        const auto s_bit = test_bit(instruction, 22);
        const auto rn = (instruction >> 16u) & 0xFu;
        const auto register_list = instruction & 0xFFFFu;
        const auto count = static_cast<u32>(std::popcount(register_list));

        if (count == 0) {
            raise_exception(ExceptionType::Undefined);
            current_cycle_ += 2;
            return 0;
        }

        const auto base = read_reg(rn);
        auto address = up ? (base + (pre ? 4u : 0u)) : (base - (4u * count) + (pre ? 0u : 4u));
        auto access = AccessType::NonSequential;

        const auto is_user_bank = s_bit && !load;
        const auto is_user_bank_ldm = s_bit && load && !test_bit(register_list, 15);

        for (u32 reg = 0; reg < 16u; ++reg) {
            if (!test_bit(register_list, reg)) {
                continue;
            }

            if (load) {
                const auto result = bus_.read(address, BusWidth::Word, access, current_cycle_);
                current_cycle_ += result.cycles;
                const auto value = result.value;
                if (reg == 15u) {
                    if (s_bit && privileged_mode(mode())) {
                        const auto old_mode = mode();
                        const auto new_cpsr = spsr_for_mode(state_, mode());
                        const auto new_mode_val = static_cast<CpuMode>(new_cpsr & 0x1Fu);
                        if (new_mode_val != old_mode) {
                            switch_mode(new_mode_val);
                        }
                        state_.cpsr = new_cpsr;
                    }
                    branch_to(value, test_bit(state_.cpsr, 5));
                } else if (is_user_bank_ldm && reg >= 8u && reg <= 14u) {
                    write_user_reg(reg, value);
                } else {
                    state_.regs[reg] = value;
                }
            } else {
                u32 val;
                if (is_user_bank && reg >= 8u && reg <= 14u) {
                    val = read_user_reg(reg);
                } else {
                    val = reg == 15u ? pc_visible() : state_.regs[reg];
                }
                const auto result = bus_.write(address, val, BusWidth::Word, access, current_cycle_);
                current_cycle_ += result.cycles;
            }
            access = AccessType::Sequential;
            address += 4u;
        }

        if (write_back && !(load && test_bit(register_list, rn))) {
            const auto delta = 4u * count;
            state_.regs[rn] = up ? base + delta : base - delta;
        }
        ++current_cycle_;
        return 0;
    }

    if ((instruction & 0x0F000000u) == 0x0F000000u) {
        if (handle_hle_swi(instruction & 0x00FFFFFFu)) {
            current_cycle_ += 3;
            return 0;
        }
        raise_exception(ExceptionType::SoftwareInterrupt);
        current_cycle_ += 3;
        return 0;
    }

    if (((instruction >> 26u) & 0x3u) == 0x0u) {
        const auto opcode = (instruction >> 21u) & 0xFu;
        const auto set_flags = test_bit(instruction, 20);
        const auto immediate = test_bit(instruction, 25);
        const auto rn = (instruction >> 16u) & 0xFu;
        const auto rd = (instruction >> 12u) & 0xFu;
        bool shift_by_register = false;
        const auto read_dp_reg = [&](u32 index) -> u32 {
            if (index != 15u) {
                return state_.regs[index];
            }
            return shift_by_register ? (pc_visible() + 4u) : pc_visible();
        };

        ShiftResult operand2{};
        if (immediate) {
            const auto imm = instruction & 0xFFu;
            const auto rotate = ((instruction >> 8u) & 0xFu) * 2u;
            operand2.value = rotate_right(imm, rotate);
            operand2.carry = rotate == 0 ? carry_in : test_bit(operand2.value, 31);
        } else {
            const auto rm = instruction & 0xFu;
            const auto shift_type = (instruction >> 5u) & 0x3u;
            shift_by_register = test_bit(instruction, 4);
            u32 amount = 0;
            if (shift_by_register) {
                amount = read_dp_reg((instruction >> 8u) & 0xFu) & 0xFFu;
                ++current_cycle_;
            } else {
                amount = (instruction >> 7u) & 0x1Fu;
            }
            operand2 = apply_shift(read_dp_reg(rm), shift_type, amount, carry_in, !shift_by_register);
        }

        const auto lhs = read_dp_reg(rn);
        u32 result = 0;
        bool write_result = true;

        switch (opcode) {
        case 0x0:
            result = lhs & operand2.value;
            break;
        case 0x1:
            result = lhs ^ operand2.value;
            break;
        case 0x2:
            result = lhs - operand2.value;
            if (set_flags) {
                update_nzc_sub(lhs, operand2.value, static_cast<u64>(result));
            }
            break;
        case 0x3:
            result = operand2.value - lhs;
            if (set_flags) {
                update_nzc_sub(operand2.value, lhs, static_cast<u64>(result));
            }
            break;
        case 0x4: {
            const auto wide = static_cast<u64>(lhs) + operand2.value;
            result = static_cast<u32>(wide);
            if (set_flags) {
                update_nzc_add(lhs, operand2.value, wide);
            }
            break;
        }
        case 0x5: {
            const auto wide = static_cast<u64>(lhs) + operand2.value + (carry_in ? 1u : 0u);
            result = static_cast<u32>(wide);
            if (set_flags) {
                update_nzcv_adc(lhs, operand2.value, carry_in, result);
            }
            break;
        }
        case 0x6: {
            result = lhs - operand2.value - (carry_in ? 0u : 1u);
            if (set_flags) {
                update_nzcv_sbc(lhs, operand2.value, carry_in, result);
            }
            break;
        }
        case 0x7: {
            result = operand2.value - lhs - (carry_in ? 0u : 1u);
            if (set_flags) {
                update_nzcv_sbc(operand2.value, lhs, carry_in, result);
            }
            break;
        }
        case 0x8:
            write_result = false;
            result = lhs & operand2.value;
            update_nz(result);
            assign_bit(state_.cpsr, 29, operand2.carry);
            break;
        case 0x9:
            write_result = false;
            result = lhs ^ operand2.value;
            update_nz(result);
            assign_bit(state_.cpsr, 29, operand2.carry);
            break;
        case 0xA:
            write_result = false;
            result = lhs - operand2.value;
            update_nzc_sub(lhs, operand2.value, static_cast<u64>(result));
            break;
        case 0xB: {
            write_result = false;
            const auto wide = static_cast<u64>(lhs) + operand2.value;
            result = static_cast<u32>(wide);
            update_nzc_add(lhs, operand2.value, wide);
            break;
        }
        case 0xC:
            result = lhs | operand2.value;
            break;
        case 0xD:
            result = operand2.value;
            break;
        case 0xE:
            result = lhs & ~operand2.value;
            break;
        case 0xF:
            result = ~operand2.value;
            break;
        default:
            write_result = false;
            break;
        }

        if (write_result) {
            if (rd == 15u) {
                if (set_flags && privileged_mode(mode())) {
                    const auto old_mode = mode();
                    const auto new_cpsr = spsr_for_mode(state_, mode());
                    const auto new_mode_val = static_cast<CpuMode>(new_cpsr & 0x1Fu);
                    if (new_mode_val != old_mode) {
                        switch_mode(new_mode_val);
                    }
                    state_.cpsr = new_cpsr;
                    branch_to(result, test_bit(state_.cpsr, 5));
                } else {
                    write_pc(result);
                }
            } else {
                state_.regs[rd] = result;
                if (set_flags) {
                    update_nz(result);
                    if (opcode != 0x2 && opcode != 0x3 && opcode != 0x4 && opcode != 0x5 && opcode != 0x6 &&
                        opcode != 0x7) {
                        assign_bit(state_.cpsr, 29, operand2.carry);
                    }
                }
            }
        }

        return 0;
    }

    if (logger_ != nullptr) {
#ifndef GBA_PLATFORM_ESP32
        std::ostringstream message;
        message << "Undefined ARM instruction 0x" << std::hex << instruction;
        logger_->log("cpu", message.str());
#endif
    }
    raise_exception(ExceptionType::Undefined);
    current_cycle_ += 3;
    return 0;
}

u32 IRAM_ATTR Arm7tdmi::execute_thumb(u16 instruction) {
    const auto carry_in = test_bit(state_.cpsr, 29);
    const auto thumb_pc = [&]() -> u32 { return pc_visible() & ~0x2u; };
    const auto data_access = [&](AccessType access = AccessType::NonSequential) -> AccessType {
        const auto current_instruction = state_.regs[15] - 2u;
        if (current_instruction >= kBiosSize) {
            access |= AccessType::CpuOutsideBios;
        }
        return access;
    };
    const auto thumb_open_bus_word = [&]() -> u32 { return bus_.peek_word(pc_visible()); };

    const auto read32 = [&](u32 address) -> u32 {
        const auto result = bus_.read(address, BusWidth::Word, data_access(), current_cycle_);
        current_cycle_ += result.cycles;
        const auto value = result.open_bus ? thumb_open_bus_word() : result.value;
        return rotate_right(value, (address & 3u) * 8u);
    };
    const auto read16 = [&](u32 address) -> u32 {
        const auto result = bus_.read(address, BusWidth::Half, data_access(), current_cycle_);
        current_cycle_ += result.cycles;
        const auto value =
            result.open_bus ? ((thumb_open_bus_word() >> ((address & 2u) * 8u)) & 0xFFFFu)
                            : (result.value & 0xFFFFu);
        if ((address & 1u) != 0) {
            return rotate_right(value, 8u);
        }
        return value;
    };
    const auto read8 = [&](u32 address) -> u32 {
        const auto result = bus_.read(address, BusWidth::Byte, data_access(), current_cycle_);
        current_cycle_ += result.cycles;
        if (result.open_bus) {
            return (thumb_open_bus_word() >> ((address & 3u) * 8u)) & 0xFFu;
        }
        return result.value & 0xFFu;
    };
    const auto write32 = [&](u32 address, u32 value) {
        const auto result = bus_.write(address, value, BusWidth::Word, AccessType::NonSequential, current_cycle_);
        current_cycle_ += result.cycles;
        current_cycle_ += bus_.service_dma(current_cycle_);
    };
    const auto write16 = [&](u32 address, u32 value) {
        const auto result = bus_.write(address, value, BusWidth::Half, AccessType::NonSequential, current_cycle_);
        current_cycle_ += result.cycles;
        current_cycle_ += bus_.service_dma(current_cycle_);
    };
    const auto write8 = [&](u32 address, u32 value) {
        const auto result = bus_.write(address, value, BusWidth::Byte, AccessType::NonSequential, current_cycle_);
        current_cycle_ += result.cycles;
        current_cycle_ += bus_.service_dma(current_cycle_);
    };
    const auto reg = [&](u32 index) -> u32& { return state_.regs[index]; };

    if ((instruction & 0xF800u) == 0x1800u || (instruction & 0xE000u) == 0x0000u) {
        const auto subcode = (instruction >> 11u) & 0x3u;
        if (subcode != 0x3u) {
            const auto offset = (instruction >> 6u) & 0x1Fu;
            const auto rs = (instruction >> 3u) & 0x7u;
            const auto rd = instruction & 0x7u;
            const auto shifted = apply_shift(reg(rs), subcode, offset, carry_in, true);
            reg(rd) = shifted.value;
            update_nz(shifted.value);
            assign_bit(state_.cpsr, 29, shifted.carry);
            return 0;
        }

        const auto immediate = test_bit(instruction, 10);
        const auto subtract = test_bit(instruction, 9);
        const auto operand = immediate ? ((instruction >> 6u) & 0x7u) : reg((instruction >> 6u) & 0x7u);
        const auto rs = (instruction >> 3u) & 0x7u;
        const auto rd = instruction & 0x7u;
        if (subtract) {
            const auto result = reg(rs) - operand;
            reg(rd) = result;
            update_nzc_sub(reg(rs), operand, static_cast<u64>(result));
        } else {
            const auto wide = static_cast<u64>(reg(rs)) + operand;
            reg(rd) = static_cast<u32>(wide);
            update_nzc_add(reg(rs), operand, wide);
        }
        return 0;
    }

    if ((instruction & 0xE000u) == 0x2000u) {
        const auto op = (instruction >> 11u) & 0x3u;
        const auto rd = (instruction >> 8u) & 0x7u;
        const auto imm = instruction & 0xFFu;
        switch (op) {
        case 0:
            reg(rd) = imm;
            update_nz(reg(rd));
            break;
        case 1:
            update_nzc_sub(reg(rd), imm, static_cast<u64>(reg(rd) - imm));
            break;
        case 2: {
            const auto wide = static_cast<u64>(reg(rd)) + imm;
            reg(rd) = static_cast<u32>(wide);
            update_nz(reg(rd));
            assign_bit(state_.cpsr, 29, (wide >> 32u) != 0);
            break;
        }
        case 3: {
            const auto wide = static_cast<u64>(reg(rd)) - imm;
            reg(rd) = static_cast<u32>(wide);
            update_nz(reg(rd));
            assign_bit(state_.cpsr, 29, reg(rd) <= imm);
            break;
        }
        }
        return 0;
    }

    if ((instruction & 0xFC00u) == 0x4000u) {
        const auto alu_op = (instruction >> 6u) & 0xFu;
        const auto rs = (instruction >> 3u) & 0x7u;
        const auto rd = instruction & 0x7u;
        u32 internal_cycles = 0;
        switch (alu_op) {
        case 0x0:
            reg(rd) &= reg(rs);
            update_nz(reg(rd));
            break;
        case 0x1:
            reg(rd) ^= reg(rs);
            update_nz(reg(rd));
            break;
        case 0x2: {
            const auto shift = reg(rs) & 0xFFu;
            const auto shifted = shift_lsl(reg(rd), shift, carry_in);
            reg(rd) = shifted.value;
            update_nz(reg(rd));
            assign_bit(state_.cpsr, 29, shifted.carry);
            internal_cycles = 1;
            break;
        }
        case 0x3: {
            const auto shift = reg(rs) & 0xFFu;
            const auto shifted = shift_lsr(reg(rd), shift, carry_in, false);
            reg(rd) = shifted.value;
            update_nz(reg(rd));
            assign_bit(state_.cpsr, 29, shifted.carry);
            internal_cycles = 1;
            break;
        }
        case 0x4: {
            const auto shift = reg(rs) & 0xFFu;
            const auto shifted = shift_asr(reg(rd), shift, carry_in, false);
            reg(rd) = shifted.value;
            update_nz(reg(rd));
            assign_bit(state_.cpsr, 29, shifted.carry);
            internal_cycles = 1;
            break;
        }
        case 0x5: {
            const auto lhs = reg(rd);
            const auto rhs = reg(rs);
            const auto wide = static_cast<u64>(lhs) + rhs + (carry_in ? 1u : 0u);
            reg(rd) = static_cast<u32>(wide);
            update_nzcv_adc(lhs, rhs, carry_in, reg(rd));
            break;
        }
        case 0x6: {
            const auto old = reg(rd);
            const auto rhs = reg(rs);
            reg(rd) = old - rhs - (carry_in ? 0u : 1u);
            update_nzcv_sbc(old, rhs, carry_in, reg(rd));
            break;
        }
        case 0x7: {
            const auto shift = reg(rs) & 0xFFu;
            const auto shifted = shift_ror(reg(rd), shift, carry_in, false);
            reg(rd) = shifted.value;
            update_nz(reg(rd));
            assign_bit(state_.cpsr, 29, shifted.carry);
            internal_cycles = 1;
            break;
        }
        case 0x8:
            update_nz(reg(rd) & reg(rs));
            break;
        case 0x9: {
            const auto result = 0 - reg(rs);
            reg(rd) = result;
            update_nzc_sub(0, reg(rs), static_cast<u64>(result));
            break;
        }
        case 0xA: {
            const auto result = reg(rd) - reg(rs);
            update_nzc_sub(reg(rd), reg(rs), static_cast<u64>(result));
            break;
        }
        case 0xB: {
            const auto wide = static_cast<u64>(reg(rd)) + reg(rs);
            update_nzc_add(reg(rd), reg(rs), wide);
            break;
        }
        case 0xC:
            reg(rd) |= reg(rs);
            update_nz(reg(rd));
            break;
        case 0xD: {
            const auto product = reg(rd) * reg(rs);
            reg(rd) = product;
            update_nz(product);
            internal_cycles = 3;
            break;
        }
        case 0xE:
            reg(rd) &= ~reg(rs);
            update_nz(reg(rd));
            break;
        case 0xF:
            reg(rd) = ~reg(rs);
            update_nz(reg(rd));
            break;
        }
        current_cycle_ += internal_cycles;
        return 0;
    }

    if ((instruction & 0xFC00u) == 0x4400u) {
        const auto op = (instruction >> 8u) & 0x3u;
        const auto h1 = test_bit(instruction, 7);
        const auto h2 = test_bit(instruction, 6);
        const auto rs = ((h2 ? 1u : 0u) << 3u) | ((instruction >> 3u) & 0x7u);
        const auto rd = ((h1 ? 1u : 0u) << 3u) | (instruction & 0x7u);
        const auto read_hi_reg = [&](u32 index) -> u32 {
            return index == 15u ? pc_visible() : reg(index);
        };
        bool refill_pipeline = false;
        switch (op) {
        case 0: {
            const auto result = read_hi_reg(rd) + read_hi_reg(rs);
            if (rd == 15u) {
                branch_to(result, true);
                refill_pipeline = true;
            } else {
                reg(rd) = result;
            }
            break;
        }
        case 1: {
            const auto lhs = read_hi_reg(rd);
            const auto rhs = read_hi_reg(rs);
            const auto result = lhs - rhs;
            update_nzc_sub(lhs, rhs, static_cast<u64>(result));
            break;
        }
        case 2: {
            const auto value = read_hi_reg(rs);
            if (rd == 15u) {
                branch_to(value, true);
                refill_pipeline = true;
            } else {
                reg(rd) = value;
            }
            break;
        }
        case 3: {
            const auto target = read_hi_reg(rs);
            branch_to(target, test_bit(target, 0));
            refill_pipeline = true;
            break;
        }
        }
        if (refill_pipeline) {
            current_cycle_ += 2;
        }
        return 0;
    }

    if ((instruction & 0xF800u) == 0x4800u) {
        const auto rd = (instruction >> 8u) & 0x7u;
        const auto word = instruction & 0xFFu;
        reg(rd) = read32((thumb_pc() & ~0x3u) + (word * 4u));
        ++current_cycle_;
        return 0;
    }

    if ((instruction & 0xF000u) == 0x5000u) {
        const auto op = (instruction >> 9u) & 0x7u;
        const auto rm = (instruction >> 6u) & 0x7u;
        const auto rn = (instruction >> 3u) & 0x7u;
        const auto rd = instruction & 0x7u;
        const auto address = reg(rn) + reg(rm);
        switch (op) {
        case 0:
            write32(address, reg(rd));
            break;
        case 1:
            write16(address, reg(rd));
            break;
        case 2:
            write8(address, reg(rd));
            break;
        case 3:
            reg(rd) = static_cast<u32>(sign_extend<8>(read8(address)));
            break;
        case 4:
            reg(rd) = read32(address);
            break;
        case 5:
            reg(rd) = read16(address);
            break;
        case 6:
            reg(rd) = read8(address);
            break;
        case 7:
            reg(rd) = (address & 1u) != 0 ? static_cast<u32>(sign_extend<8>(read8(address)))
                                          : static_cast<u32>(sign_extend<16>(read16(address)));
            break;
        default:
            break;
       }
       ++current_cycle_;
       return 0;
    }

    if ((instruction & 0xE000u) == 0x6000u) {
        const auto byte = test_bit(instruction, 12);
        const auto load = test_bit(instruction, 11);
        const auto offset = (instruction >> 6u) & 0x1Fu;
        const auto rn = (instruction >> 3u) & 0x7u;
        const auto rd = instruction & 0x7u;
        const auto address = reg(rn) + (byte ? offset : offset * 4u);
        if (load) {
            reg(rd) = byte ? read8(address) : read32(address);
        } else if (byte) {
            write8(address, reg(rd));
        } else {
            write32(address, reg(rd));
        }
        ++current_cycle_;
        return 0;
    }

    if ((instruction & 0xF000u) == 0x8000u) {
        const auto load = test_bit(instruction, 11);
        const auto offset = ((instruction >> 6u) & 0x1Fu) * 2u;
        const auto rn = (instruction >> 3u) & 0x7u;
        const auto rd = instruction & 0x7u;
        const auto address = reg(rn) + offset;
        if (load) {
            reg(rd) = read16(address);
        } else {
            write16(address, reg(rd));
        }
        ++current_cycle_;
        return 0;
    }

    if ((instruction & 0xF000u) == 0x9000u) {
        const auto load = test_bit(instruction, 11);
        const auto rd = (instruction >> 8u) & 0x7u;
        const auto offset = (instruction & 0xFFu) * 4u;
        const auto address = reg(13) + offset;
        if (load) {
            reg(rd) = read32(address);
        } else {
            write32(address, reg(rd));
        }
        ++current_cycle_;
        return 0;
    }

    if ((instruction & 0xF000u) == 0xA000u) {
        const auto sp = test_bit(instruction, 11);
        const auto rd = (instruction >> 8u) & 0x7u;
        const auto offset = (instruction & 0xFFu) * 4u;
        reg(rd) = (sp ? reg(13) : thumb_pc()) + offset;
        ++current_cycle_;
        return 0;
    }

    if ((instruction & 0xFF00u) == 0xB000u) {
        const auto subtract = test_bit(instruction, 7);
        const auto offset = (instruction & 0x7Fu) * 4u;
        reg(13) = subtract ? reg(13) - offset : reg(13) + offset;
        ++current_cycle_;
        return 0;
    }

    if ((instruction & 0xF600u) == 0xB400u) {
        const auto load = test_bit(instruction, 11);
        const auto include_pc_lr = test_bit(instruction, 8);
        auto address = reg(13);
        auto access = AccessType::NonSequential;

        if (!load) {
            const auto count =
                static_cast<u32>(std::popcount(static_cast<u16>(instruction & 0xFFu))) + (include_pc_lr ? 1u : 0u);
            address = reg(13) - (4u * count);
            auto write_address = address;
            for (u32 r = 0; r < 8u; ++r) {
                if (test_bit(instruction, r)) {
                    const auto result = bus_.write(write_address, reg(r), BusWidth::Word, access, current_cycle_);
                    current_cycle_ += result.cycles;
                    access = AccessType::Sequential;
                    write_address += 4u;
                }
            }
            if (include_pc_lr) {
                const auto result = bus_.write(write_address, reg(14), BusWidth::Word, access, current_cycle_);
                current_cycle_ += result.cycles;
            }
            reg(13) = address;
        } else {
            auto read_address = reg(13);
            for (u32 r = 0; r < 8u; ++r) {
                if (test_bit(instruction, r)) {
                    const auto result = bus_.read(read_address, BusWidth::Word, access, current_cycle_);
                    current_cycle_ += result.cycles;
                    reg(r) = result.value;
                    access = AccessType::Sequential;
                    read_address += 4u;
                }
            }
            if (include_pc_lr) {
                const auto result = bus_.read(read_address, BusWidth::Word, access, current_cycle_);
                current_cycle_ += result.cycles;
                branch_to(result.value, true);
                read_address += 4u;
            }
            reg(13) = read_address;
        }
        ++current_cycle_;
        return 0;
    }

    if ((instruction & 0xF000u) == 0xC000u) {
        const auto load = test_bit(instruction, 11);
        const auto rn = (instruction >> 8u) & 0x7u;
        const auto register_list = instruction & 0xFFu;
        auto address = reg(rn);
        auto access = AccessType::NonSequential;

        for (u32 r = 0; r < 8u; ++r) {
            if (!test_bit(register_list, r)) {
                continue;
            }
            if (load) {
                const auto result = bus_.read(address, BusWidth::Word, access, current_cycle_);
                current_cycle_ += result.cycles;
                reg(r) = result.value;
            } else {
                const auto result = bus_.write(address, reg(r), BusWidth::Word, access, current_cycle_);
                current_cycle_ += result.cycles;
            }
            address += 4u;
            access = AccessType::Sequential;
        }
        reg(rn) = address;
        ++current_cycle_;
        return 0;
    }

    if ((instruction & 0xF000u) == 0xD000u) {
        if ((instruction & 0x0F00u) == 0x0F00u) {
            if (handle_hle_swi(instruction & 0x00FFu)) {
                current_cycle_ += 3;
                return 0;
            }
            raise_exception(ExceptionType::SoftwareInterrupt);
            current_cycle_ += 3;
            return 0;
        }

        const auto condition = (instruction >> 8u) & 0xFu;
        if (condition_passed(condition)) {
            const auto offset = sign_extend<9>((instruction & 0xFFu) << 1u);
            branch_to(state_.regs[15] + 2u + static_cast<u32>(offset), true);
            current_cycle_ += 2;
        }
        return 0;
    }

    if ((instruction & 0xF800u) == 0xE000u) {
        const auto offset = sign_extend<12>((instruction & 0x7FFu) << 1u);
        branch_to(state_.regs[15] + 2u + static_cast<u32>(offset), true);
        current_cycle_ += 2;
        return 0;
    }

    if ((instruction & 0xF800u) == 0xF000u) {
        const auto offset = sign_extend<23>((instruction & 0x7FFu) << 12u);
        reg(14) = pc_visible() + static_cast<u32>(offset);
        return 0;
    }

    if ((instruction & 0xF800u) == 0xF800u) {
        const auto target = reg(14) + ((instruction & 0x7FFu) << 1u);
        reg(14) = (state_.regs[15]) | 1u;
        branch_to(target, true);
        current_cycle_ += 2;
        return 0;
    }

    if (logger_ != nullptr) {
#ifndef GBA_PLATFORM_ESP32
        std::ostringstream message;
        message << "Undefined Thumb instruction 0x" << std::hex << instruction;
        logger_->log("cpu", message.str());
#endif
    }
    raise_exception(ExceptionType::Undefined);
    current_cycle_ += 3;
    return 0;
}

u32 Arm7tdmi::read_user_reg(u32 reg) {
    if (reg >= 8u && reg <= 12u) {
        return state_.banked_usr_r8_r12[reg - 8u];
    }
    if (reg == 13u) {
        return state_.banked_usr_r13_r14[0];
    }
    if (reg == 14u) {
        return state_.banked_usr_r13_r14[1];
    }
    return state_.regs[reg];
}

void Arm7tdmi::write_user_reg(u32 reg, u32 value) {
    if (reg >= 8u && reg <= 12u) {
        state_.banked_usr_r8_r12[reg - 8u] = value;
    } else if (reg == 13u) {
        state_.banked_usr_r13_r14[0] = value;
    } else if (reg == 14u) {
        state_.banked_usr_r13_r14[1] = value;
    } else {
        state_.regs[reg] = value;
    }
}

void Arm7tdmi::switch_mode(CpuMode new_mode) {
    const auto old_mode = mode();
    if (old_mode == new_mode) {
        return;
    }

    if (old_mode == CpuMode::Fiq) {
        for (std::size_t i = 0; i < 7u; ++i) {
            state_.banked_fiq_r8_r14[i] = state_.regs[8u + i];
        }
    } else {
        for (std::size_t i = 0; i < 5u; ++i) {
            state_.banked_usr_r8_r12[i] = state_.regs[8u + i];
        }
    }

    switch (old_mode) {
    case CpuMode::User:
    case CpuMode::System:
        state_.banked_usr_r13_r14[0] = state_.regs[13];
        state_.banked_usr_r13_r14[1] = state_.regs[14];
        break;
    case CpuMode::Supervisor:
        state_.banked_svc_r13_r14[0] = state_.regs[13];
        state_.banked_svc_r13_r14[1] = state_.regs[14];
        break;
    case CpuMode::Irq:
        state_.banked_irq_r13_r14[0] = state_.regs[13];
        state_.banked_irq_r13_r14[1] = state_.regs[14];
        break;
    case CpuMode::Abort:
        state_.banked_abt_r13_r14[0] = state_.regs[13];
        state_.banked_abt_r13_r14[1] = state_.regs[14];
        break;
    case CpuMode::Undefined:
        state_.banked_und_r13_r14[0] = state_.regs[13];
        state_.banked_und_r13_r14[1] = state_.regs[14];
        break;
    case CpuMode::Fiq:
        break;
    }

    state_.cpsr = (state_.cpsr & ~0x1Fu) | static_cast<u32>(new_mode);

    if (new_mode == CpuMode::Fiq) {
        for (std::size_t i = 0; i < 7u; ++i) {
            state_.regs[8u + i] = state_.banked_fiq_r8_r14[i];
        }
    } else {
        for (std::size_t i = 0; i < 5u; ++i) {
            state_.regs[8u + i] = state_.banked_usr_r8_r12[i];
        }
    }

    switch (new_mode) {
    case CpuMode::User:
    case CpuMode::System:
        state_.regs[13] = state_.banked_usr_r13_r14[0];
        state_.regs[14] = state_.banked_usr_r13_r14[1];
        break;
    case CpuMode::Supervisor:
        state_.regs[13] = state_.banked_svc_r13_r14[0];
        state_.regs[14] = state_.banked_svc_r13_r14[1];
        break;
    case CpuMode::Irq:
        state_.regs[13] = state_.banked_irq_r13_r14[0];
        state_.regs[14] = state_.banked_irq_r13_r14[1];
        break;
    case CpuMode::Abort:
        state_.regs[13] = state_.banked_abt_r13_r14[0];
        state_.regs[14] = state_.banked_abt_r13_r14[1];
        break;
    case CpuMode::Undefined:
        state_.regs[13] = state_.banked_und_r13_r14[0];
        state_.regs[14] = state_.banked_und_r13_r14[1];
        break;
    case CpuMode::Fiq:
        state_.regs[13] = state_.banked_fiq_r8_r14[5];
        state_.regs[14] = state_.banked_fiq_r8_r14[6];
        break;
    }
}

void Arm7tdmi::enter_exception(ExceptionType type, CpuMode target_mode, u32 vector, bool mask_irq, bool mask_fiq,
                               u32 return_address) {
    (void)type;
    const auto old_cpsr = state_.cpsr;
    switch_mode(target_mode);
    spsr_for_mode(state_, target_mode) = old_cpsr;
    state_.regs[14] = return_address;
    assign_bit(state_.cpsr, 5, false);
    assign_bit(state_.cpsr, 7, mask_irq);
    assign_bit(state_.cpsr, 6, mask_fiq);
    state_.halted = false;
    branch_to(vector, false);
}

void Arm7tdmi::branch_to(u32 address, bool thumb) {
    assign_bit(state_.cpsr, 5, thumb);
    state_.regs[15] = thumb ? (address & ~1u) : (address & ~0x3u);
    state_.next_fetch_access = AccessType::CodeFetch;
}

void Arm7tdmi::update_nz(u32 value) {
    assign_bit(state_.cpsr, 31, test_bit(value, 31));
    assign_bit(state_.cpsr, 30, value == 0);
}

void Arm7tdmi::update_nzc_add(u32 lhs, u32 rhs, u64 result) {
    const auto value = static_cast<u32>(result);
    update_nz(value);
    assign_bit(state_.cpsr, 29, (result >> 32u) != 0);
    const auto overflow = (~(lhs ^ rhs) & (lhs ^ value) & 0x80000000u) != 0;
    assign_bit(state_.cpsr, 28, overflow);
}

void Arm7tdmi::update_nzc_sub(u32 lhs, u32 rhs, u64 result) {
    const auto value = static_cast<u32>(result);
    update_nz(value);
    assign_bit(state_.cpsr, 29, lhs >= rhs);
    const auto overflow = ((lhs ^ rhs) & (lhs ^ value) & 0x80000000u) != 0;
    assign_bit(state_.cpsr, 28, overflow);
}

void Arm7tdmi::update_nzcv_adc(u32 lhs, u32 rhs, bool carry_in, u32 result) {
    update_nz(result);

    const auto wide = static_cast<u64>(lhs) + static_cast<u64>(rhs) + (carry_in ? 1u : 0u);
    assign_bit(state_.cpsr, 29, (wide >> 32u) != 0);

    const auto signed_result = static_cast<s64>(static_cast<s32>(lhs))
                             + static_cast<s64>(static_cast<s32>(rhs))
                             + static_cast<s64>(carry_in ? 1 : 0);
    const bool overflow = signed_result > static_cast<s64>(std::numeric_limits<s32>::max())
                       || signed_result < static_cast<s64>(std::numeric_limits<s32>::min());
    assign_bit(state_.cpsr, 28, overflow);
}

void Arm7tdmi::update_nzcv_sbc(u32 lhs, u32 rhs, bool carry_in, u32 result) {
    update_nz(result);

    const auto borrow = carry_in ? 0u : 1u;
    const auto rhs_wide = static_cast<u64>(rhs) + static_cast<u64>(borrow);
    assign_bit(state_.cpsr, 29, static_cast<u64>(lhs) >= rhs_wide);

    const auto signed_result = static_cast<s64>(static_cast<s32>(lhs))
                             - static_cast<s64>(static_cast<s32>(rhs))
                             - static_cast<s64>(borrow);
    const bool overflow = signed_result > static_cast<s64>(std::numeric_limits<s32>::max())
                       || signed_result < static_cast<s64>(std::numeric_limits<s32>::min());
    assign_bit(state_.cpsr, 28, overflow);
}

}  // namespace gba
