#include "gba/core/cpu.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
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

#ifndef GBA_TRACE_TIMERS
#define GBA_TRACE_TIMERS 0
#endif

constexpr u32 kFlagI = 1u << 7;
constexpr u32 kFlagF = 1u << 6;

#ifndef GBA_PLATFORM_ESP32
[[nodiscard]] bool trace_rtc_pc_enabled() {
    static const bool enabled = std::getenv("GBA_RTC_TRACE_PC") != nullptr;
    return enabled;
}

[[nodiscard]] bool is_gpio_trace_address(u32 address) {
    const auto normalized = address & 0x0FFFFFFEu;
    return normalized >= 0x080000C4u && normalized <= 0x080000C8u;
}

[[nodiscard]] const char* bus_width_name(BusWidth width) {
    switch (width) {
    case BusWidth::Byte:
        return "8";
    case BusWidth::Half:
        return "16";
    case BusWidth::Word:
        return "32";
    }
    return "?";
}
#endif

void trace_gpio_cpu(const char* mode, const char* op, u32 instruction_pc, u32 visible_pc, u32 address, BusWidth width,
                    u32 value) {
#ifndef GBA_PLATFORM_ESP32
    if (trace_rtc_pc_enabled() && is_gpio_trace_address(address)) {
        std::fprintf(stderr, "GPIOCPU %s %s%s instr=%08X visible=%08X addr=%08X value=%08X\n",
                     mode,
                     op,
                     bus_width_name(width),
                     instruction_pc,
                     visible_pc,
                     address,
                     value);
    }
#else
    (void)mode;
    (void)op;
    (void)instruction_pc;
    (void)visible_pc;
    (void)address;
    (void)width;
    (void)value;
#endif
}

// Precomputed condition-check LUT: index = (condition << 4) | NZCV flags
constexpr auto kCondLut = [] {
    std::array<bool, 256> lut{};
    for (u32 cond = 0; cond < 16; ++cond) {
        for (u32 flags = 0; flags < 16; ++flags) {
            const bool n = (flags >> 3) & 1;
            const bool z = (flags >> 2) & 1;
            const bool c = (flags >> 1) & 1;
            const bool v = flags & 1;
            bool pass = false;
            switch (cond) {
            case 0x0: pass = z; break;
            case 0x1: pass = !z; break;
            case 0x2: pass = c; break;
            case 0x3: pass = !c; break;
            case 0x4: pass = n; break;
            case 0x5: pass = !n; break;
            case 0x6: pass = v; break;
            case 0x7: pass = !v; break;
            case 0x8: pass = c && !z; break;
            case 0x9: pass = !c || z; break;
            case 0xA: pass = n == v; break;
            case 0xB: pass = n != v; break;
            case 0xC: pass = !z && (n == v); break;
            case 0xD: pass = z || (n != v); break;
            case 0xE: pass = true; break;
            default: pass = false; break;
            }
            lut[(cond << 4) | flags] = pass;
        }
    }
    return lut;
}();

[[nodiscard]] bool privileged_mode(CpuMode mode) {
    return mode != CpuMode::User;
}

[[nodiscard]] constexpr std::size_t r13_r14_bank_index(CpuMode mode) {
    switch (mode) {
    case CpuMode::User:
    case CpuMode::System:
        return 0;
    case CpuMode::Supervisor:
        return 1;
    case CpuMode::Irq:
        return 2;
    case CpuMode::Abort:
        return 3;
    case CpuMode::Undefined:
        return 4;
    case CpuMode::Fiq:
        return 0;
    }
    return 0;
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

[[nodiscard]] u32 signed_multiply_cycles(u32 multiplier) {
    if ((multiplier & 0xFFFFFF00u) == 0u || (multiplier & 0xFFFFFF00u) == 0xFFFFFF00u) {
        return 1;
    }
    if ((multiplier & 0xFFFF0000u) == 0u || (multiplier & 0xFFFF0000u) == 0xFFFF0000u) {
        return 2;
    }
    if ((multiplier & 0xFF000000u) == 0u || (multiplier & 0xFF000000u) == 0xFF000000u) {
        return 3;
    }
    return 4;
}

[[nodiscard]] u32 unsigned_multiply_cycles(u32 multiplier) {
    if ((multiplier & 0xFFFFFF00u) == 0u) {
        return 1;
    }
    if ((multiplier & 0xFFFF0000u) == 0u) {
        return 2;
    }
    if ((multiplier & 0xFF000000u) == 0u) {
        return 3;
    }
    return 4;
}

[[nodiscard]] u32 gamepak_sequential_half_cycles(u32 waitcnt, u32 address) {
    const auto page = (address >> 24u) & 0x0Fu;
    if (page < 0x08u || page >= 0x0Eu) {
        return 0;
    }
    if (page < 0x0Au) {
        return test_bit(waitcnt, 4u) ? 2u : 3u;
    }
    if (page < 0x0Cu) {
        return test_bit(waitcnt, 7u) ? 2u : 5u;
    }
    return test_bit(waitcnt, 10u) ? 2u : 9u;
}

[[nodiscard]] u32 gamepak_nonsequential_half_cycles(u32 waitcnt, u32 address) {
    static constexpr std::array<u32, 4> nseq{5u, 4u, 3u, 9u};
    const auto page = (address >> 24u) & 0x0Fu;
    if (page < 0x08u || page >= 0x0Eu) {
        return 0;
    }
    if (page < 0x0Au) {
        return nseq[(waitcnt >> 2u) & 0x3u];
    }
    if (page < 0x0Cu) {
        return nseq[(waitcnt >> 5u) & 0x3u];
    }
    return nseq[(waitcnt >> 8u) & 0x3u];
}

[[nodiscard]] u32 sequential_code_cycles(u32 waitcnt, u32 address, BusWidth width) {
    const auto region = address & 0x0F000000u;
    if (address < 0x00004000u) {
        return 1u;
    }
    if (region == 0x02000000u) {
        return width == BusWidth::Word ? 6u : 3u;
    }
    if (region == 0x03000000u || region == 0x04000000u || region == 0x07000000u) {
        return 1u;
    }
    if (region == 0x05000000u || region == 0x06000000u) {
        return width == BusWidth::Word ? 2u : 1u;
    }
    if (address >= 0x08000000u && address < 0x0E000000u) {
        const auto half_cycles = gamepak_sequential_half_cycles(waitcnt, address);
        return width == BusWidth::Word ? half_cycles * 2u : half_cycles;
    }
    return 1u;
}

[[nodiscard]] u32 thumb_branch_refill_cycles(u32 waitcnt, u32 address) {
    return 2u * sequential_code_cycles(waitcnt, address, BusWidth::Half);
}

[[nodiscard]] int exception_prefetch_adjust(u32 waitcnt, u32 address) {
    if (test_bit(waitcnt, 14u) && address >= 0x08000000u && address < 0x0E000000u) {
        return 1 - static_cast<int>(gamepak_nonsequential_half_cycles(waitcnt, address) -
                                    gamepak_sequential_half_cycles(waitcnt, address));
    }
    return 1;
}

[[nodiscard]] u32 arm_exception_refill_cycles(u32 waitcnt, u32 address) {
    const auto cycles = static_cast<int>((2u * sequential_code_cycles(waitcnt, address, BusWidth::Word)) + 1u);
    return static_cast<u32>(cycles + exception_prefetch_adjust(waitcnt, address));
}

[[nodiscard]] u32 thumb_exception_refill_cycles(u32 waitcnt, u32 address) {
    const auto cycles = static_cast<int>(thumb_branch_refill_cycles(waitcnt, address) + 1u);
    return static_cast<u32>(cycles + exception_prefetch_adjust(waitcnt, address));
}

[[nodiscard]] u32 integer_sqrt(u32 value) {
    u32 result = 0;
    u32 bit = 1u << 30u;
    while (bit > value) {
        bit >>= 2u;
    }
    while (bit != 0u) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1u) + bit;
        } else {
            result >>= 1u;
        }
        bit >>= 2u;
    }
    return result;
}

[[nodiscard]] u32 bios_atan2_units(s32 y, s32 x) {
    if (x == 0 && y == 0) {
        return 0;
    }

    static constexpr std::array<s32, 16> kCordicAngles{
        8192, 4836, 2555, 1297, 651, 326, 163, 81,
        41, 20, 10, 5, 3, 1, 1, 0,
    };

    s64 x_acc = x;
    s64 y_acc = y;
    s32 angle = 0;
    if (x_acc < 0) {
        x_acc = -x_acc;
        y_acc = -y_acc;
        angle = 0x8000;
    }

    for (std::size_t i = 0; i < kCordicAngles.size(); ++i) {
        const auto x_shift = x_acc >> i;
        const auto y_shift = y_acc >> i;
        if (y_acc > 0) {
            x_acc += y_shift;
            y_acc -= x_shift;
            angle += kCordicAngles[i];
        } else {
            x_acc -= y_shift;
            y_acc += x_shift;
            angle -= kCordicAngles[i];
        }
    }
    return static_cast<u32>(angle) & 0xFFFFu;
}

#ifdef GBA_ENABLE_HLE_BIOS

const std::array<s16, 512> kBgAffineSinLut = {{
    0x0000, 0x0324, 0x0648, 0x096C, 0x0C8F, 0x0FB3, 0x12D5, 0x15F7,
    0x1918, 0x1C38, 0x1F57, 0x2275, 0x2592, 0x28AE, 0x2BC8, 0x2EE1,
    -0x7FFD, -0x7CDC, -0x79BC, -0x769E, -0x7382, -0x7067, -0x6D4E, -0x6A38,
    -0x6723, -0x6412, -0x6102, -0x5DF5, -0x5AEB, -0x57E3, -0x54DF, -0x51DD,
    0x4EE0, 0x4BE5, 0x48EE, 0x45FB, 0x430B, 0x4020, 0x3D39, 0x3A56,
    0x3777, 0x349D, 0x31C7, 0x2EF6, 0x2C2A, 0x2962, 0x26A0, 0x23E2,
    0x212A, 0x1E77, 0x1BC9, 0x1921, 0x167E, 0x13E1, 0x114A, 0x0EB8,
    0x0C2D, 0x09A7, 0x0728, 0x04AF, 0x023C, -0x002B, -0x00F2, -0x01B2,
    -0x026D, -0x0321, -0x03CF, -0x0476, -0x0517, -0x05B2, -0x0646, -0x06D4,
    -0x075B, -0x07DB, -0x0855, -0x08C7, -0x0933, -0x0998, -0x09F6, -0x0A4C,
    -0x0A9C, -0x0AE4, -0x0B25, -0x0B5E, -0x0B90, -0x0BBB, -0x0BDE, -0x0BFA,
    -0x0C0E, -0x0C1B, -0x0C20, -0x0C1E, -0x0C15, -0x0C04, -0x0BEC, -0x0BCD,
    -0x0BA6, -0x0B79, -0x0B44, -0x0B08, -0x0AC5, -0x0A7B, -0x0A2A, -0x09D2,
    -0x0973, -0x090D, -0x08A0, -0x082D, -0x07B3, -0x0732, -0x06AB, -0x061D,
    -0x0589, -0x04EF, -0x044F, -0x03A9, -0x02FD, -0x024C, -0x0195, -0x00D9,
    -0x0018, 0x00A8, 0x01D3, 0x0303, 0x0438, 0x0572, 0x06B0, 0x07F3,
    0x093A, 0x0A86, 0x0BD5, 0x0D29, 0x0E80, 0x0FDB, 0x113A, 0x129C,
    0x1401, 0x1569, 0x16D4, 0x1842, 0x19B2, 0x1B25, 0x1C9A, 0x1E11,
    0x1F8A, 0x2105, 0x2281, 0x23FF, 0x257E, 0x26FE, 0x287F, 0x2A01,
    0x2B84, 0x2D07, 0x2E8B, 0x300E, 0x3192, 0x3316, 0x3499, 0x361C,
    0x379F, 0x3921, 0x3AA2, 0x3C23, 0x3DA2, 0x3F20, 0x409D, 0x4219,
    0x4394, 0x450D, 0x4684, 0x47FA, 0x496E, 0x4AE0, 0x4C51, 0x4DBF,
    0x4F2B, 0x5096, 0x51FD, 0x5363, 0x54C6, 0x5626, 0x5784, 0x58DF,
    0x5A38, 0x5B8D, 0x5CE0, 0x5E30, 0x5F7C, 0x60C6, 0x620C, 0x634F,
    0x648F, 0x65CB, 0x6704, 0x683A, 0x696C, 0x6A9A, 0x6BC5, 0x6CEC,
    0x6E10, 0x6F2F, 0x704B, 0x7163, 0x7277, 0x7387, 0x7493, 0x759B,
    0x76A0, 0x77A0, 0x789C, 0x7994, 0x7A87, 0x686E, 0x686E, 0x686E,
    0x686E, 0x686E, 0x686E, 0x686E, 0x686E, 0x686E, 0x686E, 0x686E,
    -0x686E, -0x7994, -0x789C, -0x77A0, -0x76A0, -0x759B, -0x7493, -0x7387,
    -0x7277, -0x7163, -0x704B, -0x6F2F, -0x6E10, -0x6CEC, -0x6BC5, -0x6A9A,
    -0x696C, -0x683A, -0x6704, -0x65CB, -0x648F, -0x634F, -0x620C, -0x60C6,
    -0x5F7C, -0x5E30, -0x5CE0, -0x5B8D, -0x5A38, -0x58DF, -0x5784, -0x5626,
    -0x54C6, -0x5363, -0x51FD, -0x5096, -0x4F2B, -0x4DBF, -0x4C51, -0x4AE0,
    -0x496E, -0x47FA, -0x4684, -0x450D, -0x4394, -0x4219, -0x409D, -0x3F20,
    -0x3DA2, -0x3C23, -0x3AA2, -0x3921, -0x379F, -0x361C, -0x3499, -0x3316,
    -0x3192, -0x300E, -0x2E8B, -0x2D07, -0x2B84, -0x2A01, -0x287F, -0x26FE,
    -0x257E, -0x23FF, -0x2281, -0x2105, -0x1F8A, -0x1E11, -0x1C9A, -0x1B25,
    -0x19B2, -0x1842, -0x16D4, -0x1569, -0x1401, -0x129C, -0x113A, -0x0FDB,
    -0x0E80, -0x0D29, -0x0BD5, -0x0A86, -0x093A, -0x07F3, -0x06B0, -0x0572,
    -0x0438, -0x0303, -0x01D3, -0x00A8, 0x0018, 0x00D9, 0x0195, 0x024C,
    0x02FD, 0x03A9, 0x044F, 0x04EF, 0x0589, 0x061D, 0x06AB, 0x0732,
    0x07B3, 0x082D, 0x08A0, 0x090D, 0x0973, 0x09D2, 0x0A2A, 0x0A7B,
    0x0AC5, 0x0B08, 0x0B44, 0x0B79, 0x0BA6, 0x0BCD, 0x0BEC, 0x0C04,
    0x0C15, 0x0C1E, 0x0C20, 0x0C1B, 0x0C0E, 0x0BFA, 0x0BDE, 0x0BBB,
    0x0B90, 0x0B5E, 0x0B25, 0x0AE4, 0x0A9C, 0x0A4C, 0x09F6, 0x0998,
    0x0933, 0x08C7, 0x0855, 0x07DB, 0x075B, 0x06D4, 0x0646, 0x05B2,
    0x0517, 0x0476, 0x03CF, 0x0321, 0x026D, 0x01B2, 0x00F2, 0x002B,
    -0x023C, -0x04AF, -0x0728, -0x09A7, -0x0C2D, -0x0EB8, -0x114A, -0x13E1,
    -0x167E, -0x1921, -0x1BC9, -0x1E77, -0x212A, -0x23E2, -0x26A0, -0x2962,
    -0x2C2A, -0x2EF6, -0x31C7, -0x349D, -0x3777, -0x3A56, -0x3D39, -0x4020,
    -0x430B, -0x45FB, -0x48EE, -0x4BE5, -0x4EE0, 0x51DD, 0x54DF, 0x57E3,
    0x5AEB, 0x5DF5, 0x6102, 0x6412, 0x6723, 0x6A38, 0x6D4E, 0x7067,
    0x7382, 0x769E, 0x79BC, 0x7CDC, 0x7FFD, -0x2EE1, -0x2BC8, -0x28AE,
    -0x2592, -0x2275, -0x1F57, -0x1C38, -0x1918, -0x15F7, -0x12D5, -0x0FB3,
    -0x0C8F, -0x096C, -0x0648, -0x0324
}};

[[nodiscard]] constexpr u32 lz77_decompressed_size(u32 header) {
    return header >> 8u;
}

[[nodiscard]] constexpr u32 rl_decompressed_size(u32 header) {
    return header >> 8u;
}

[[nodiscard]] constexpr u32 diff_decompressed_size(u32 header) {
    return header >> 8u;
}

[[nodiscard]] constexpr u32 huff_data_size_bits(u32 header) {
    return header & 0xFu;
}

[[nodiscard]] constexpr u32 huff_decompressed_size(u32 header) {
    return header >> 8u;
}

void hle_lz77_uncomp(Arm7tdmi& cpu, Bus& bus, bool vram_mode) {
    u64 cycles = cpu.current_cycle();
    const u32 src = cpu.state().regs[0];
    u32 dst = cpu.state().regs[1];
    if (vram_mode) dst &= ~1u;

    const auto header = bus.read(src, BusWidth::Word, AccessType::NonSequential, cycles);
    cycles += header.cycles;
    const u32 size = lz77_decompressed_size(header.value);
    if (size == 0) { cpu.set_current_cycle(cycles); return; }

    u32 src_offset = src + 4u;
    u32 bytes_written = 0;

    while (bytes_written < size) {
        const auto flag_result = bus.read(src_offset, BusWidth::Byte, AccessType::NonSequential, cycles);
        cycles += flag_result.cycles;
        u8 flags = static_cast<u8>(flag_result.value);
        src_offset += 1u;

        for (int bit = 0; bit < 8 && bytes_written < size; ++bit) {
            if ((flags & 0x80u) == 0u) {
                const auto data_result = bus.read(src_offset, BusWidth::Byte, AccessType::NonSequential, cycles);
                cycles += data_result.cycles;
                u8 byte_val = static_cast<u8>(data_result.value);
                src_offset += 1u;

                if (vram_mode) {
                    u32 write_val;
                    if (dst & 1u) {
                        const auto prev_result = bus.read(dst & ~1u, BusWidth::Half, AccessType::NonSequential, cycles);
                        cycles += prev_result.cycles;
                        write_val = (prev_result.value & 0xFF00u) | byte_val;
                    } else {
                        write_val = byte_val | (byte_val << 8u);
                    }
                    const auto w = bus.write(dst & ~1u, write_val, BusWidth::Half, AccessType::NonSequential, cycles);
                    cycles += w.cycles;
                } else {
                    const auto w = bus.write(dst, byte_val, BusWidth::Byte, AccessType::NonSequential, cycles);
                    cycles += w.cycles;
                }
                dst += vram_mode ? 2u : 1u;
                bytes_written += 1u;
            } else {
                const auto lo_result = bus.read(src_offset, BusWidth::Byte, AccessType::NonSequential, cycles);
                cycles += lo_result.cycles;
                const auto hi_result = bus.read(src_offset + 1u, BusWidth::Byte, AccessType::NonSequential, cycles);
                cycles += hi_result.cycles;
                u8 lo = static_cast<u8>(lo_result.value);
                u8 hi = static_cast<u8>(hi_result.value);
                src_offset += 2u;

                u32 disp = lo | ((hi & 0xF0u) << 4u);
                u32 count = (hi & 0x0Fu) + 3u;

                if (count > size - bytes_written)
                    count = size - bytes_written;

                if (vram_mode) {
                    const u32 copy_src = dst - (disp + 1u) * 2u;
                    for (u32 i = 0; i < count; ++i) {
                        const auto r = bus.read(copy_src + i * 2u, BusWidth::Byte, AccessType::NonSequential, cycles);
                        cycles += r.cycles;
                        u8 byte_val = static_cast<u8>(r.value);
                        u32 write_val;
                        if (dst & 1u) {
                            const auto prev_result = bus.read(dst & ~1u, BusWidth::Half, AccessType::NonSequential, cycles);
                            cycles += prev_result.cycles;
                            write_val = (prev_result.value & 0xFF00u) | byte_val;
                        } else {
                            write_val = byte_val | (byte_val << 8u);
                        }
                        const auto w = bus.write(dst & ~1u, write_val, BusWidth::Half, AccessType::NonSequential, cycles);
                        cycles += w.cycles;
                        dst += 2u;
                    }
                } else {
                    const u32 copy_src = dst - disp - 1u;
                    for (u32 i = 0; i < count; ++i) {
                        const auto r = bus.read(copy_src + i, BusWidth::Byte, AccessType::NonSequential, cycles);
                        cycles += r.cycles;
                        const auto w = bus.write(dst + i, r.value, BusWidth::Byte, AccessType::NonSequential, cycles);
                        cycles += w.cycles;
                    }
                    dst += count;
                }
                bytes_written += count;
            }
            flags <<= 1u;
        }
    }
    cpu.set_current_cycle(cycles);
}

void hle_rl_uncomp(Arm7tdmi& cpu, Bus& bus, bool vram_mode) {
    u64 cycles = cpu.current_cycle();
    const u32 src = cpu.state().regs[0];
    u32 dst = cpu.state().regs[1];
    if (vram_mode) dst &= ~1u;

    const auto header = bus.read(src, BusWidth::Word, AccessType::NonSequential, cycles);
    cycles += header.cycles;
    const u32 size = rl_decompressed_size(header.value);
    if (size == 0) { cpu.set_current_cycle(cycles); return; }

    u32 src_offset = src + 4u;
    u32 bytes_written = 0;

    auto write_byte_vram = [&](u8 val) {
        if (vram_mode) {
            u32 write_val;
            if (dst & 1u) {
                const auto prev_result = bus.read(dst & ~1u, BusWidth::Half, AccessType::NonSequential, cycles);
                cycles += prev_result.cycles;
                write_val = (prev_result.value & 0xFF00u) | val;
            } else {
                write_val = val | (val << 8u);
            }
            const auto w = bus.write(dst & ~1u, write_val, BusWidth::Half, AccessType::NonSequential, cycles);
            cycles += w.cycles;
            dst += 2u;
        } else {
            const auto w = bus.write(dst, val, BusWidth::Byte, AccessType::NonSequential, cycles);
            cycles += w.cycles;
            dst += 1u;
        }
    };

    while (bytes_written < size) {
        const auto flag_result = bus.read(src_offset, BusWidth::Byte, AccessType::NonSequential, cycles);
        cycles += flag_result.cycles;
        const u8 flag = static_cast<u8>(flag_result.value);
        src_offset += 1u;

        const bool compressed = (flag & 0x80u) != 0u;
        u32 length = (flag & 0x7Fu);

        if (compressed) {
            length += 3u;
            const auto data_result = bus.read(src_offset, BusWidth::Byte, AccessType::NonSequential, cycles);
            cycles += data_result.cycles;
            u8 val = static_cast<u8>(data_result.value);
            src_offset += 1u;

            if (length > size - bytes_written)
                length = size - bytes_written;

            for (u32 i = 0; i < length; ++i) {
                write_byte_vram(val);
            }
            bytes_written += length;
        } else {
            length += 1u;
            if (length > size - bytes_written)
                length = size - bytes_written;

            for (u32 i = 0; i < length; ++i) {
                const auto data_result = bus.read(src_offset + i, BusWidth::Byte, AccessType::NonSequential, cycles);
                cycles += data_result.cycles;
                u8 val = static_cast<u8>(data_result.value);
                write_byte_vram(val);
            }
            src_offset += length;
            bytes_written += length;
        }
    }
    cpu.set_current_cycle(cycles);
}

void hle_huff_uncomp(Arm7tdmi& cpu, Bus& bus) {
    u64 cycles = cpu.current_cycle();
    const u32 src = cpu.state().regs[0];
    const u32 dst_addr_base = cpu.state().regs[1];

    const auto header = bus.read(src, BusWidth::Word, AccessType::NonSequential, cycles);
    cycles += header.cycles;
    const u32 data_size_bits = huff_data_size_bits(header.value);
    const u32 size = huff_decompressed_size(header.value);
    if (size == 0) { cpu.set_current_cycle(cycles); return; }

    u32 src_offset = src + 4u;

    const auto tree_size_result = bus.read(src_offset, BusWidth::Byte, AccessType::NonSequential, cycles);
    cycles += tree_size_result.cycles;
    const u32 tree_size = static_cast<u8>(tree_size_result.value);
    src_offset += 1u;

    u32 root_offset = src_offset;
    u32 bitstream_offset = src_offset + static_cast<u32>(tree_size + 1u) * 2u;

    u32 dest = dst_addr_base;
    u32 dest_word = 0;
    int dest_shift = 0;
    u32 bytes_written = 0;

    u32 bitstream_word = 0;
    int bits_left = 0;
    u32 bitstream_addr = bitstream_offset;

    auto read_node = [&](u32 node_index) -> u32 {
        const auto r = bus.read(root_offset + node_index * 2u, BusWidth::Half, AccessType::NonSequential, cycles);
        cycles += r.cycles;
        return r.value & 0xFFFFu;
    };

    auto next_bit = [&]() -> bool {
        if (bits_left == 0) {
            const auto r = bus.read(bitstream_addr, BusWidth::Word, AccessType::NonSequential, cycles);
            cycles += r.cycles;
            bitstream_word = r.value;
            bits_left = 32;
            bitstream_addr += 4u;
        }
        bool bit = (bitstream_word & 0x80000000u) != 0u;
        bitstream_word <<= 1u;
        --bits_left;
        return bit;
    };

    while (bytes_written < size) {
        u32 node_index = 0;
        for (;;) {
            u32 node = read_node(node_index);
            bool bit = next_bit();
            bool is_data;
            u32 child_offset;

            if (bit) {
                is_data = (node & 0x4000u) != 0u;
                child_offset = node & 0x3Fu;
            } else {
                is_data = (node & 0x8000u) != 0u;
                child_offset = (node >> 6u) & 0x3Fu;
            }

            if (is_data) {
                u8 data = static_cast<u8>(node & 0xFFu);
                if (data_size_bits == 8) {
                    dest_word |= static_cast<u32>(data) << dest_shift;
                    dest_shift += 8;
                    bytes_written += 1u;
                } else {
                    dest_word |= static_cast<u32>(data & 0xFu) << dest_shift;
                    dest_shift += 4;
                    bytes_written += 1u;
                }

                if (dest_shift >= 32 || bytes_written == size) {
                    const auto w = bus.write(dest, dest_word, BusWidth::Word, AccessType::NonSequential, cycles);
                    cycles += w.cycles;
                    dest += 4u;
                    dest_word = 0;
                    dest_shift = 0;
                }
                break;
            }
            node_index = child_offset;
        }
    }

    if (dest_shift > 0) {
        const auto w = bus.write(dest, dest_word, BusWidth::Word, AccessType::NonSequential, cycles);
        cycles += w.cycles;
    }
    cpu.set_current_cycle(cycles);
}

void hle_bit_unpack(Arm7tdmi& cpu, Bus& bus) {
    u64 cycles = cpu.current_cycle();
    const u32 src = cpu.state().regs[0];
    const u32 dst_addr_base = cpu.state().regs[1];
    const u32 info_addr = cpu.state().regs[2];

    const auto info_len_result = bus.read(info_addr, BusWidth::Half, AccessType::NonSequential, cycles);
    cycles += info_len_result.cycles;
    const u32 src_len = info_len_result.value & 0xFFFFu;

    const auto info_widths_result = bus.read(info_addr + 2u, BusWidth::Half, AccessType::NonSequential, cycles);
    cycles += info_widths_result.cycles;
    const u32 src_width = (info_widths_result.value & 0xFFu);
    const u32 dst_width = ((info_widths_result.value >> 8u) & 0xFFu);

    const auto info_offset_result = bus.read(info_addr + 4u, BusWidth::Word, AccessType::NonSequential, cycles);
    cycles += info_offset_result.cycles;
    u32 data_offset = info_offset_result.value & 0x7FFFFFFFu;
    const bool zero_data = (info_offset_result.value & 0x80000000u) != 0u;

    if (src_width == 0 || dst_width == 0) { cpu.set_current_cycle(cycles); return; }
    if (src_len == 0) { cpu.set_current_cycle(cycles); return; }

    u32 src_mask = (1u << src_width) - 1u;
    u32 dst_mask = (1u << dst_width) - 1u;

    u32 src_bit = 0;
    u32 src_addr = src;
    u32 src_word = 0;

    u32 dst_word = 0;
    u32 dst_bit = 0;
    u32 dst_addr = dst_addr_base;

    for (u32 i = 0; i < src_len; ++i) {
        if (src_bit < src_width) {
            const auto r = bus.read(src_addr, BusWidth::Word, AccessType::NonSequential, cycles);
            cycles += r.cycles;
            src_word = r.value;
            src_addr += 4u;
            src_bit = 32;
        }

        u32 unit = (src_word >> (32u - src_width)) & src_mask;
        src_word <<= src_width;
        src_bit -= src_width;

        if (unit != 0 || zero_data)
            unit += data_offset;
        unit &= dst_mask;

        dst_word |= unit << (32u - dst_width - dst_bit);
        dst_bit += dst_width;

        if (dst_bit >= 32) {
            const auto w = bus.write(dst_addr, dst_word, BusWidth::Word, AccessType::NonSequential, cycles);
            cycles += w.cycles;
            dst_addr += 4u;
            dst_word = 0;
            dst_bit = 0;
        }
    }

    if (dst_bit > 0) {
        const auto w = bus.write(dst_addr, dst_word, BusWidth::Word, AccessType::NonSequential, cycles);
        cycles += w.cycles;
    }
    cpu.set_current_cycle(cycles);
}

void hle_diff_8bit_unfilter(Arm7tdmi& cpu, Bus& bus, bool vram_mode) {
    u64 cycles = cpu.current_cycle();
    const u32 src = cpu.state().regs[0];
    const u32 dst = cpu.state().regs[1];

    const auto header = bus.read(src, BusWidth::Word, AccessType::NonSequential, cycles);
    cycles += header.cycles;
    const u32 size = diff_decompressed_size(header.value);
    if (size == 0) { cpu.set_current_cycle(cycles); return; }

    u32 src_offset = src + 4u;
    u8 accumulator = 0;

    for (u32 i = 0; i < size; ++i) {
        const auto data_result = bus.read(src_offset, BusWidth::Byte, AccessType::NonSequential, cycles);
        cycles += data_result.cycles;
        u8 val = static_cast<u8>(data_result.value);
        src_offset += 1u;

        accumulator = static_cast<u8>(accumulator + val);

        if (vram_mode) {
            const u32 dest_addr = dst + i * 2u;
            u32 write_val;
            if (dest_addr & 1u) {
                const auto prev_result = bus.read(dest_addr & ~1u, BusWidth::Half, AccessType::NonSequential, cycles);
                cycles += prev_result.cycles;
                write_val = (prev_result.value & 0xFF00u) | accumulator;
            } else {
                write_val = accumulator | (accumulator << 8u);
            }
            const auto w = bus.write(dest_addr & ~1u, write_val, BusWidth::Half, AccessType::NonSequential, cycles);
            cycles += w.cycles;
        } else {
            const auto w = bus.write(dst + i, accumulator, BusWidth::Byte, AccessType::NonSequential, cycles);
            cycles += w.cycles;
        }
    }
    cpu.set_current_cycle(cycles);
}

void hle_diff_16bit_unfilter(Arm7tdmi& cpu, Bus& bus) {
    u64 cycles = cpu.current_cycle();
    const u32 src = cpu.state().regs[0];
    const u32 dst = cpu.state().regs[1];

    const auto header = bus.read(src, BusWidth::Word, AccessType::NonSequential, cycles);
    cycles += header.cycles;
    const u32 size = diff_decompressed_size(header.value);
    if (size == 0) { cpu.set_current_cycle(cycles); return; }

    u32 src_offset = src + 4u;
    u16 accumulator = 0;

    for (u32 i = 0; i < size; ++i) {
        const auto data_result = bus.read(src_offset, BusWidth::Half, AccessType::NonSequential, cycles);
        cycles += data_result.cycles;
        u16 val = static_cast<u16>(data_result.value);
        src_offset += 2u;

        accumulator = static_cast<u16>(accumulator + val);

        const auto w = bus.write(dst + i * 2u, accumulator, BusWidth::Half, AccessType::NonSequential, cycles);
        cycles += w.cycles;
    }
    cpu.set_current_cycle(cycles);
}

void hle_bg_affine_set(Arm7tdmi& cpu, Bus& bus) {
    u64 cycles = cpu.current_cycle();
    const u32 src = cpu.state().regs[0];
    const u32 dst = cpu.state().regs[1];
    const u32 count = cpu.state().regs[2];

    for (u32 i = 0; i < count; ++i) {
        const u32 s = src + i * 20u;

        auto read32 = [&](u32 addr) -> s32 {
            const auto r = bus.read(addr, BusWidth::Word, AccessType::NonSequential, cycles);
            cycles += r.cycles;
            return static_cast<s32>(r.value);
        };
        auto read16s = [&](u32 addr) -> s32 {
            const auto r = bus.read(addr, BusWidth::Half, AccessType::NonSequential, cycles);
            cycles += r.cycles;
            return sign_extend<16>(static_cast<u32>(r.value));
        };
        auto read16u = [&](u32 addr) -> u32 {
            const auto r = bus.read(addr, BusWidth::Half, AccessType::NonSequential, cycles);
            cycles += r.cycles;
            return r.value & 0xFFFFu;
        };
        auto write16 = [&](u32 addr, s32 val) {
            const auto w = bus.write(addr, static_cast<u32>(val & 0xFFFFu), BusWidth::Half, AccessType::NonSequential, cycles);
            cycles += w.cycles;
        };
        auto write32 = [&](u32 addr, s32 val) {
            const auto w = bus.write(addr, static_cast<u32>(val), BusWidth::Word, AccessType::NonSequential, cycles);
            cycles += w.cycles;
        };

        const s32 cx = read32(s);
        const s32 cy = read32(s + 4u);
        const s32 dx = read16s(s + 8u);
        const s32 dy = read16s(s + 10u);
        const s32 sx = read16s(s + 12u);
        const s32 sy = read16s(s + 14u);
        const u32 angle_raw = read16u(s + 16u);
        const u16 angle = static_cast<u16>((angle_raw >> 8u) & 0xFFu);

        const auto sin_val = kBgAffineSinLut[static_cast<std::size_t>(angle * 2u)];
        const auto cos_val = kBgAffineSinLut[static_cast<std::size_t>(angle * 2u + 128u)];

        const s32 scaled_cos = (static_cast<s32>(cos_val) * sx) >> 14;
        const s32 scaled_sin = (static_cast<s32>(sin_val) * sx) >> 14;
        const s32 scaled_neg_sin = (static_cast<s32>(-sin_val) * sy) >> 14;
        const s32 scaled_cos_y = (static_cast<s32>(cos_val) * sy) >> 14;

        s32 pa = scaled_cos;
        s32 pb = scaled_sin;
        s32 pc = scaled_neg_sin;
        s32 pd = scaled_cos_y;

        s32 start_x = (cx << 8) - (pa * dx + pb * dy);
        s32 start_y = (cy << 8) - (pc * dx + pd * dy);

        const u32 d = dst + i * 20u;

        write16(d, pa);
        write16(d + 2u, pb);
        write16(d + 4u, pc);
        write16(d + 6u, pd);
        write32(d + 8u, start_x);
        write32(d + 12u, start_y);
    }
    cpu.set_current_cycle(cycles);
}

void hle_obj_affine_set(Arm7tdmi& cpu, Bus& bus) {
    u64 cycles = cpu.current_cycle();
    const u32 src = cpu.state().regs[0];
    const u32 dst = cpu.state().regs[1];
    const u32 count = cpu.state().regs[2];
    const u32 offset = cpu.state().regs[3];

    for (u32 i = 0; i < count; ++i) {
        const u32 s = src + i * 8u;

        auto read16s = [&](u32 addr) -> s32 {
            const auto r = bus.read(addr, BusWidth::Half, AccessType::NonSequential, cycles);
            cycles += r.cycles;
            return sign_extend<16>(static_cast<u32>(r.value));
        };
        auto read16u = [&](u32 addr) -> u32 {
            const auto r = bus.read(addr, BusWidth::Half, AccessType::NonSequential, cycles);
            cycles += r.cycles;
            return r.value & 0xFFFFu;
        };
        auto write16 = [&](u32 addr, s32 val) {
            const auto w = bus.write(addr, static_cast<u32>(val & 0xFFFFu), BusWidth::Half, AccessType::NonSequential, cycles);
            cycles += w.cycles;
        };

        const s32 sx = read16s(s);
        const s32 sy = read16s(s + 2u);
        const u32 angle_raw = read16u(s + 4u);
        const u16 angle = static_cast<u16>((angle_raw >> 8u) & 0xFFu);

        const auto sin_val = kBgAffineSinLut[static_cast<std::size_t>(angle * 2u)];
        const auto cos_val = kBgAffineSinLut[static_cast<std::size_t>(angle * 2u + 128u)];

        s32 pa = (static_cast<s32>(cos_val) * sx) >> 14;
        s32 pb = (static_cast<s32>(sin_val) * sx) >> 14;
        s32 pc = (static_cast<s32>(-sin_val) * sy) >> 14;
        s32 pd = (static_cast<s32>(cos_val) * sy) >> 14;

        const u32 d = dst + i * offset;

        write16(d, pa);
        write16(d + 2u, pb);
        write16(d + 4u, pc);
        write16(d + 6u, pd);
    }
    cpu.set_current_cycle(cycles);
}

void hle_sound_bias(Arm7tdmi& cpu, Bus& bus) {
    u64 cycles = cpu.current_cycle();
    const u32 bias_level = cpu.state().regs[0] & 0x3FFu;
    const u32 ramp_to = (bias_level == 0) ? 0u : 0x200u;

    const auto current_result = bus.read(kSoundBias, BusWidth::Half, AccessType::Io, cycles);
    cycles += current_result.cycles;
    u16 current = static_cast<u16>(current_result.value);
    const u16 target = static_cast<u16>((current & 0xFF00u) | ramp_to);

    constexpr u32 kDelay = 8;
    u32 steps = 0;
    const u32 max_steps = 512;

    while ((current & 0x3FFu) != ramp_to && steps < max_steps) {
        if ((current & 0x3FFu) < ramp_to)
            current = static_cast<u16>(current + 1u);
        else
            current = static_cast<u16>(current - 1u);
        cycles += kDelay;
        ++steps;
    }

    current = target;
    const auto w = bus.write(kSoundBias, current, BusWidth::Half, AccessType::Io, cycles);
    cycles += w.cycles;
    cpu.set_current_cycle(cycles);
}

void hle_midi_key2freq(Arm7tdmi& cpu, Bus& bus) {
    u64 cycles = cpu.current_cycle();
    const u32 wa = cpu.state().regs[0];
    const u32 mk = cpu.state().regs[1] & 0xFFu;
    const u32 fp = cpu.state().regs[2] & 0xFFu;

    if (wa >= kBiosSize) {
        cpu.state().regs[0] = 0;
        return;
    }

    const auto data_result = bus.read(wa, BusWidth::Byte, AccessType::NonSequential, cycles);
    cycles += data_result.cycles;
    u32 a = data_result.value & 0xFFu;

    const s32 key = (static_cast<s32>(mk) - 64) * 256 + static_cast<s32>(fp);
    const s32 freq = (static_cast<s32>(a) * key) / 256;

    cpu.state().regs[0] = static_cast<u32>(freq);
    cpu.set_current_cycle(cycles);
}

#endif  // GBA_ENABLE_HLE_BIOS

}  // namespace

Arm7tdmi::Arm7tdmi(Bus& bus, IrqController& irq, TraceLogger* logger)
    : bus_(bus), irq_(irq), logger_(logger) {}

void Arm7tdmi::reset(bool skip_bios) {
    state_ = {};
    state_.cpsr = static_cast<u32>(CpuMode::Supervisor) | kFlagI | kFlagF;
    state_.next_fetch_access = AccessType::CodeFetch;
    current_cycle_ = 0;
    last_fetch_cycle_ = 0;
    last_fetch_gamepak_ = false;

    hle_swi_enabled_ = true;
    if (!skip_bios && bus_.has_bios()) {
        const auto swi_vector = bus_.read(0x00000008u, BusWidth::Word, AccessType::CodeFetch, 0).value;
        std::fprintf(stderr, "BIOS SWI vector at 0x08: 0x%08X (stub=0xEAFFFFFE, match=%d)\n",
                     static_cast<unsigned>(swi_vector), swi_vector == 0xEAFFFFFEu);
        hle_swi_enabled_ = swi_vector == 0xEAFFFFFEu;
        return;
    }

    state_.cpsr = static_cast<u32>(CpuMode::System) | kFlagF;
    state_.regs[13] = 0x03007F00u;
    state_.regs[15] = 0x08000000u;
    state_.banked_r13_r14[r13_r14_bank_index(CpuMode::Supervisor)][0] = 0x03007FE0u;
    state_.banked_r13_r14[r13_r14_bank_index(CpuMode::Irq)][0] = 0x03007FA0u;
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
    last_fetch_cycle_ = cycle;
    last_fetch_gamepak_ = false;
}

u64 IRAM_ATTR Arm7tdmi::cpu_run_until(u64 target_cycle) {
    static constexpr int kHardwareServiceBatchInstructions = 16;
    int service_countdown = 0;
    while (current_cycle_ < target_cycle) {
        if (service_countdown <= 0) {
            service_countdown = kHardwareServiceBatchInstructions;
            bus_.service_timers(current_cycle_);
            if (bus_.dma_next_event_cycle() <= current_cycle_) {
                current_cycle_ += bus_.service_dma(current_cycle_);
            }
            irq_.advance(current_cycle_);
        }
        --service_countdown;

        if (last_fetch_cycle_ > current_cycle_) {
            last_fetch_cycle_ = current_cycle_;
        }
        const auto since_fetch = static_cast<int>(current_cycle_ - last_fetch_cycle_);
        if (since_fetch > 0) {
            if (last_fetch_gamepak_) {
                bus_.prefetch_advance(since_fetch);
            }
            last_fetch_cycle_ = current_cycle_;
        }

        if (state_.halted || bus_.halted()) {
            state_.halted = true;
            const auto halt_wake_pending = static_cast<u16>(irq_.ie() & irq_.iflags()) != 0u;
            if (halt_wake_pending) {
                state_.halted = false;
                bus_.clear_halt();
                if (pc_trace_enabled_) {
                    pc_trace_[pc_trace_pos_ % kPcTraceSize] = 0xFFFF0001u;
                    pc_trace_[++pc_trace_pos_ % kPcTraceSize] = state_.regs[15];
                    ++pc_trace_pos_;
                }
            } else {
                current_cycle_ = target_cycle;
                return current_cycle_;
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
            last_fetch_cycle_ = current_cycle_;
            if (bus_.dma_next_event_cycle() <= current_cycle_) {
                current_cycle_ += bus_.service_dma(current_cycle_);
            }
            execute_thumb(instruction);
        } else {
            const auto instruction = fetch_arm();
            last_fetch_cycle_ = current_cycle_;
            if (bus_.dma_next_event_cycle() <= current_cycle_) {
                current_cycle_ += bus_.service_dma(current_cycle_);
            }
            execute_arm(instruction);
        }
    }
    return current_cycle_;
}

u32 Arm7tdmi::step() {
    const auto start_cycle = current_cycle_;
    bus_.service_timers(current_cycle_);
    if (bus_.dma_next_event_cycle() <= current_cycle_) {
        current_cycle_ += bus_.service_dma(current_cycle_);
    }
    irq_.advance(current_cycle_);

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

    if (last_fetch_cycle_ > current_cycle_) {
        last_fetch_cycle_ = current_cycle_;
    }
    const auto since_fetch = static_cast<int>(current_cycle_ - last_fetch_cycle_);
    if (since_fetch > 0) {
        if (last_fetch_gamepak_) {
            bus_.prefetch_advance(since_fetch);
        }
        last_fetch_cycle_ = current_cycle_;
    }

    if (thumb_state()) {
        const auto instruction = fetch_thumb();
        last_fetch_cycle_ = current_cycle_;
        if (bus_.dma_next_event_cycle() <= current_cycle_) {
            current_cycle_ += bus_.service_dma(current_cycle_);
        }
        execute_thumb(instruction);
    } else {
        const auto instruction = fetch_arm();
        last_fetch_cycle_ = current_cycle_;
        if (bus_.dma_next_event_cycle() <= current_cycle_) {
            current_cycle_ += bus_.service_dma(current_cycle_);
        }
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
    return kCondLut[(condition << 4) | ((state_.cpsr >> 28u) & 0xFu)];
}

u32 Arm7tdmi::fetch_arm() {
    const auto address = state_.regs[15];
    const auto result = bus_.read(address, BusWidth::Word, state_.next_fetch_access, current_cycle_);
    current_cycle_ += result.cycles;
    state_.regs[15] += 4u;
    state_.next_fetch_access = AccessType::CodeFetch | AccessType::Sequential;
    last_fetch_gamepak_ = address >= 0x08000000u && address < 0x0E000000u;
    return result.value;
}

u16 Arm7tdmi::fetch_thumb() {
    const auto address = state_.regs[15];
    const auto result = bus_.read(address, BusWidth::Half, state_.next_fetch_access, current_cycle_);
    current_cycle_ += result.cycles;
    state_.regs[15] += 2u;
    state_.next_fetch_access = AccessType::CodeFetch | AccessType::Sequential;
    last_fetch_gamepak_ = address >= 0x08000000u && address < 0x0E000000u;
    return static_cast<u16>(result.value & 0xFFFFu);
}

u32 Arm7tdmi::pc_visible() const {
    return state_.regs[15] + (thumb_state() ? 2u : 4u);
}

u32 Arm7tdmi::thumb_pc_visible() const {
    return pc_visible() & ~0x2u;
}

u32 IRAM_ATTR Arm7tdmi::read_visible_reg(u32 index) const {
    return index == 15u ? pc_visible() : state_.regs[index];
}

void IRAM_ATTR Arm7tdmi::arm_write_pc(u32 value) {
    branch_to(value & ~0x3u, false);
}

AccessType IRAM_ATTR Arm7tdmi::data_access(AccessType access) const {
    const auto current_instruction = state_.regs[15] - 4u;
    if (current_instruction >= kBiosSize) {
        access |= AccessType::CpuOutsideBios;
    }
    return access;
}

void IRAM_ATTR Arm7tdmi::break_fetch_burst(const BusAccessResult& result) {
    if (result.breaks_fetch_burst || !test_bit(bus_.waitcnt(), 14u)) {
        state_.next_fetch_access = AccessType::CodeFetch;
    }
}

void IRAM_ATTR Arm7tdmi::break_fetch_burst_for_internal() {
    if (!test_bit(bus_.waitcnt(), 14u)) {
        state_.next_fetch_access = AccessType::CodeFetch;
    }
}

u32 IRAM_ATTR Arm7tdmi::arm_open_bus_word() const {
    return bus_.peek_word(pc_visible());
}

u32 IRAM_ATTR Arm7tdmi::thumb_open_bus_word() const {
    const auto pc = pc_visible();
    if (pc >= 0x08000000u && pc < 0x0E000000u) {
        const auto half = bus_.peek_word(pc) & 0xFFFFu;
        return half | (half << 16u);
    }
    return bus_.peek_word(pc);
}

u32 IRAM_ATTR Arm7tdmi::resolve_arm_open_bus_word(const BusAccessResult& result) const {
    return result.dma_open_bus ? result.value : arm_open_bus_word();
}

u32 IRAM_ATTR Arm7tdmi::resolve_thumb_open_bus_word(const BusAccessResult& result) const {
    return result.dma_open_bus ? result.value : thumb_open_bus_word();
}

void IRAM_ATTR Arm7tdmi::trace_arm_gpio(const char* op, u32 address, BusWidth width, u32 value) const {
    trace_gpio_cpu("ARM", op, state_.regs[15] - 4u, pc_visible(), address, width, value);
}

void IRAM_ATTR Arm7tdmi::trace_thumb_gpio(const char* op, u32 address, BusWidth width, u32 value) const {
    trace_gpio_cpu("THUMB", op, thumb_pc_visible() - 4u, pc_visible(), address, width, value);
}

u32 IRAM_ATTR Arm7tdmi::arm_read8(u32 address) {
    const auto result = bus_.read(address, BusWidth::Byte, data_access(), current_cycle_);
    current_cycle_ += result.cycles;
    break_fetch_burst(result);
    u32 value = result.value & 0xFFu;
    if (result.open_bus) {
        value = (resolve_arm_open_bus_word(result) >> ((address & 3u) * 8u)) & 0xFFu;
    }
    trace_arm_gpio("R", address, BusWidth::Byte, value);
    return value;
}

u32 IRAM_ATTR Arm7tdmi::arm_read16(u32 address) {
    const auto result = bus_.read(address, BusWidth::Half, data_access(), current_cycle_);
    current_cycle_ += result.cycles;
    break_fetch_burst(result);
    auto value = result.open_bus ? ((resolve_arm_open_bus_word(result) >> ((address & 2u) * 8u)) & 0xFFFFu)
                                 : (result.value & 0xFFFFu);
    if ((address & 1u) != 0) {
        value = rotate_right(value, 8u);
    }
    trace_arm_gpio("R", address, BusWidth::Half, value);
    return value;
}

u32 IRAM_ATTR Arm7tdmi::arm_read32(u32 address) {
    const auto result = bus_.read(address, BusWidth::Word, data_access(), current_cycle_);
    current_cycle_ += result.cycles;
    break_fetch_burst(result);
    const auto value = result.open_bus ? resolve_arm_open_bus_word(result) : result.value;
    const auto rotated = rotate_right(value, (address & 3u) * 8u);
    trace_arm_gpio("R", address, BusWidth::Word, rotated);
    return rotated;
}

void IRAM_ATTR Arm7tdmi::arm_write8(u32 address, u32 value) {
    const auto result = bus_.write(address, value, BusWidth::Byte, AccessType::NonSequential, current_cycle_);
    current_cycle_ += result.cycles;
    break_fetch_burst(result);
    if (bus_.dma_next_event_cycle() <= current_cycle_) {
        current_cycle_ += bus_.service_dma(current_cycle_);
    }
    trace_arm_gpio("W", address, BusWidth::Byte, value);
}

void IRAM_ATTR Arm7tdmi::arm_write16(u32 address, u32 value) {
    const auto result = bus_.write(address, value, BusWidth::Half, AccessType::NonSequential, current_cycle_);
    current_cycle_ += result.cycles;
    break_fetch_burst(result);
    if (bus_.dma_next_event_cycle() <= current_cycle_) {
        current_cycle_ += bus_.service_dma(current_cycle_);
    }
    trace_arm_gpio("W", address, BusWidth::Half, value);
}

void IRAM_ATTR Arm7tdmi::arm_write32(u32 address, u32 value) {
    const auto result = bus_.write(address, value, BusWidth::Word, AccessType::NonSequential, current_cycle_);
    current_cycle_ += result.cycles;
    break_fetch_burst(result);
    if (bus_.dma_next_event_cycle() <= current_cycle_) {
        current_cycle_ += bus_.service_dma(current_cycle_);
    }
    trace_arm_gpio("W", address, BusWidth::Word, value);
}

u32 IRAM_ATTR Arm7tdmi::thumb_read8(u32 address) {
    const auto result = bus_.read(address, BusWidth::Byte, data_access(), current_cycle_);
    current_cycle_ += result.cycles;
    break_fetch_burst(result);
    u32 value = result.value & 0xFFu;
    if (result.open_bus) {
        value = (resolve_thumb_open_bus_word(result) >> ((address & 3u) * 8u)) & 0xFFu;
    }
    trace_thumb_gpio("R", address, BusWidth::Byte, value);
    return value;
}

u32 IRAM_ATTR Arm7tdmi::thumb_read16(u32 address) {
    const auto result = bus_.read(address, BusWidth::Half, data_access(), current_cycle_);
    current_cycle_ += result.cycles;
    break_fetch_burst(result);
    auto value = result.open_bus ? ((resolve_thumb_open_bus_word(result) >> ((address & 2u) * 8u)) & 0xFFFFu)
                                 : (result.value & 0xFFFFu);
    if ((address & 1u) != 0) {
        value = rotate_right(value, 8u);
    }
    trace_thumb_gpio("R", address, BusWidth::Half, value);
    return value;
}

u32 IRAM_ATTR Arm7tdmi::thumb_read32(u32 address) {
    const auto result = bus_.read(address, BusWidth::Word, data_access(), current_cycle_);
    current_cycle_ += result.cycles;
    break_fetch_burst(result);
    const auto value = result.open_bus ? resolve_thumb_open_bus_word(result) : result.value;
    const auto rotated = rotate_right(value, (address & 3u) * 8u);
    trace_thumb_gpio("R", address, BusWidth::Word, rotated);
    return rotated;
}

void IRAM_ATTR Arm7tdmi::thumb_write8(u32 address, u32 value) {
    const auto result = bus_.write(address, value, BusWidth::Byte, AccessType::NonSequential, current_cycle_);
    current_cycle_ += result.cycles;
    break_fetch_burst(result);
    if (bus_.dma_next_event_cycle() <= current_cycle_) {
        current_cycle_ += bus_.service_dma(current_cycle_);
    }
    trace_thumb_gpio("W", address, BusWidth::Byte, value);
}

void IRAM_ATTR Arm7tdmi::thumb_write16(u32 address, u32 value) {
    const auto result = bus_.write(address, value, BusWidth::Half, AccessType::NonSequential, current_cycle_);
    current_cycle_ += result.cycles;
    break_fetch_burst(result);
    if (bus_.dma_next_event_cycle() <= current_cycle_) {
        current_cycle_ += bus_.service_dma(current_cycle_);
    }
    trace_thumb_gpio("W", address, BusWidth::Half, value);
}

void IRAM_ATTR Arm7tdmi::thumb_write32(u32 address, u32 value) {
    const auto result = bus_.write(address, value, BusWidth::Word, AccessType::NonSequential, current_cycle_);
    current_cycle_ += result.cycles;
    break_fetch_burst(result);
    if (bus_.dma_next_event_cycle() <= current_cycle_) {
        current_cycle_ += bus_.service_dma(current_cycle_);
    }
    trace_thumb_gpio("W", address, BusWidth::Word, value);
}

bool Arm7tdmi::handle_hle_swi(u32 comment) {
    if (!hle_swi_enabled_) {
        return false;
    }

    switch (comment) {
    case 0x00u: {
        // SoftReset: read reset flag from IWRAM[0x7FFA], clear 0x7E00-0x7FFF,
        // jump to entry point (0x08000000 or 0x02000000)
        const auto iwram = bus_.iwram();
        const auto reset_flag = iwram.size() > 0x7FFAu ? iwram[0x7FFAu] : static_cast<u8>(0);
        if (iwram.size() >= 0x8000u) {
            std::memset(iwram.data() + 0x7E00u, 0, 0x200u);
        }
        state_.cpsr = static_cast<u32>(CpuMode::System) | kFlagI | kFlagF;
        state_.halted = false;
        bus_.clear_halt();
        branch_to(reset_flag ? 0x02000000u : 0x08000000u, false);
        return true;
    }
    case 0x01u: {
        // RegisterRamReset: clear specified subsystems based on r0 flags
        const auto flags = state_.regs[0];
        if (test_bit(flags, 0u)) {
            auto ewram = bus_.ewram();
            std::memset(ewram.data(), 0, std::min(ewram.size(), static_cast<std::size_t>(kEwramSize)));
        }
        if (test_bit(flags, 1u)) {
            auto iwram = bus_.iwram();
            const auto clear_end = std::min(iwram.size(), static_cast<std::size_t>(0x7E00u));
            std::memset(iwram.data(), 0, clear_end);
        }
        if (test_bit(flags, 2u)) {
            std::memset(const_cast<u8*>(bus_.palette().data()), 0, bus_.palette().size());
        }
        if (test_bit(flags, 3u)) {
            std::memset(bus_.vram_write().data(), 0, bus_.vram_write().size());
        }
        if (test_bit(flags, 4u)) {
            std::memset(const_cast<u8*>(bus_.oam().data()), 0, bus_.oam().size());
        }
        current_cycle_ += 50;
        return true;
    }
     case 0x03u: {
         // Stop - same as halt basically for our purposes
         auto halt_result = bus_.write(kHaltCnt, 0, BusWidth::Byte, AccessType::Io, current_cycle_);
         current_cycle_ += halt_result.cycles;
         return true;
     }
     case 0x02u: {
         const auto halt_result = bus_.write(kHaltCnt, 0, BusWidth::Byte, AccessType::Io, current_cycle_);
         current_cycle_ += halt_result.cycles;
         return true;
     }
     case 0x04u: {
        // IntrWait - halt and wait for matching interrupt
        const auto mask = static_cast<u16>(state_.regs[1] & 0x3FFFu);
        const auto discard = (state_.regs[0] & 1u) != 0u;
        if (discard) {
            irq_.acknowledge(mask);
        }
        if ((irq_.ie() & irq_.iflags() & mask) != 0u) {
            state_.regs[0] = 1;
            state_.regs[1] = 1;  
            irq_.acknowledge(mask);
            return true;
        }
        auto halt_result = bus_.write(kHaltCnt, 0, BusWidth::Byte, AccessType::Io, current_cycle_);
        state_.regs[0] = 1;
        state_.regs[1] = 1;
        current_cycle_ += halt_result.cycles;
        return true;
    }
    case 0x05u: {
        // VBlankIntrWait: enable VBlank in IE, then call IntrWait
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
        // Forward to IntrWait logic with VBlank mask
        const auto discard = (state_.regs[0] & 1u) != 0u;
        const auto mask = static_cast<u16>(IrqVBlank);
        if (discard) {
            irq_.acknowledge(mask);
        }
        if ((irq_.ie() & irq_.iflags() & mask) != 0u) {
            state_.regs[0] = 1;
            state_.regs[1] = 1;
            irq_.acknowledge(mask);
            return true;
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
    case 0x06u: {
        // BIOS Div: r0 / r1 -> r0 (quotient), r1 (remainder), r3 (abs quotient)
        const auto num = static_cast<s32>(state_.regs[0]);
        const auto den = static_cast<s32>(state_.regs[1]);
        if (den == 0) {
            state_.regs[0] = (num < 0) ? 0x80000000u : 0x7FFFFFFFu;
            state_.regs[1] = static_cast<u32>(num);
            state_.regs[3] = state_.regs[0];
        } else {
            state_.regs[0] = static_cast<u32>(num / den);
            state_.regs[1] = static_cast<u32>(num % den);
            state_.regs[3] = static_cast<u32>((num < 0) ? -num : num) / static_cast<u32>((den < 0) ? -den : den);
        }
        current_cycle_ += 23;
        return true;
    }
    case 0x07u: {
        // BIOS DivArm: r0 / r1 with r0 being the denominator (swapped)
        const auto den = static_cast<s32>(state_.regs[0]);
        const auto num = static_cast<s32>(state_.regs[1]);
        if (den == 0) {
            state_.regs[0] = (num < 0) ? 0x80000000u : 0x7FFFFFFFu;
            state_.regs[1] = static_cast<u32>(num);
            state_.regs[3] = state_.regs[0];
        } else {
            state_.regs[0] = static_cast<u32>(num / den);
            state_.regs[1] = static_cast<u32>(num % den);
            state_.regs[3] = static_cast<u32>(state_.regs[0]);
        }
        current_cycle_ += 23;
        return true;
    }
    case 0x08u: {
        // BIOS Sqrt: sqrt(r0) -> r0
        state_.regs[0] = integer_sqrt(state_.regs[0]);
        current_cycle_ += 13;
        return true;
    }
    case 0x09u: {
        // BIOS ArcTan: atan(r0) -> r0 (result in 0.015873.. units)
        const auto x = static_cast<s16>(state_.regs[0]);
        state_.regs[0] = bios_atan2_units(x, 16384);
        current_cycle_ += 30;
        return true;
    }
    case 0x0Au: {
        // BIOS ArcTan2: atan2(r1, r0) -> r0
        const auto y = static_cast<s16>(state_.regs[1]);
        const auto x = static_cast<s16>(state_.regs[0]);
        state_.regs[0] = bios_atan2_units(y, x);
        current_cycle_ += 38;
        return true;
    }
    case 0x0Du: {
        // GetBiosChecksum - returns fixed checksum
        state_.regs[0] = 0xBAAE187Fu;
        current_cycle_ += 10;
        return true;
    }
    case 0x0Eu: {
#ifdef GBA_ENABLE_HLE_BIOS
        hle_bg_affine_set(*this, bus_);
#else
        current_cycle_ += 50;
#endif
        return true;
    }
    case 0x0Fu: {
#ifdef GBA_ENABLE_HLE_BIOS
        hle_obj_affine_set(*this, bus_);
#else
        current_cycle_ += 50;
#endif
        return true;
    }
    case 0x10u: {
#ifdef GBA_ENABLE_HLE_BIOS
        hle_bit_unpack(*this, bus_);
#else
        current_cycle_ += 100;
#endif
        return true;
    }
    case 0x11u: {
#ifdef GBA_ENABLE_HLE_BIOS
        hle_lz77_uncomp(*this, bus_, false);
#else
        current_cycle_ += 200;
#endif
        return true;
    }
    case 0x12u: {
#ifdef GBA_ENABLE_HLE_BIOS
        hle_lz77_uncomp(*this, bus_, true);
#else
        current_cycle_ += 200;
#endif
        return true;
    }
    case 0x13u: {
#ifdef GBA_ENABLE_HLE_BIOS
        hle_huff_uncomp(*this, bus_);
#else
        current_cycle_ += 200;
#endif
        return true;
    }
    case 0x14u: {
#ifdef GBA_ENABLE_HLE_BIOS
        hle_rl_uncomp(*this, bus_, false);
#else
        current_cycle_ += 200;
#endif
        return true;
    }
    case 0x15u: {
#ifdef GBA_ENABLE_HLE_BIOS
        hle_rl_uncomp(*this, bus_, true);
#else
        current_cycle_ += 200;
#endif
        return true;
    }
    case 0x16u: {
#ifdef GBA_ENABLE_HLE_BIOS
        hle_diff_8bit_unfilter(*this, bus_, false);
#else
        current_cycle_ += 100;
#endif
        return true;
    }
    case 0x17u: {
#ifdef GBA_ENABLE_HLE_BIOS
        hle_diff_8bit_unfilter(*this, bus_, true);
#else
        current_cycle_ += 100;
#endif
        return true;
    }
    case 0x18u: {
#ifdef GBA_ENABLE_HLE_BIOS
        hle_diff_16bit_unfilter(*this, bus_);
#else
        current_cycle_ += 100;
#endif
        return true;
    }
    case 0x19u: {
#ifdef GBA_ENABLE_HLE_BIOS
        hle_sound_bias(*this, bus_);
#else
        current_cycle_ += 10;
#endif
        return true;
    }
    case 0x1Fu: {
#ifdef GBA_ENABLE_HLE_BIOS
        hle_midi_key2freq(*this, bus_);
#else
        current_cycle_ += 10;
#endif
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

    // Jump-table dispatch on bits 27:25 of ARM instruction
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
    // Jump-table dispatch on bits 27:25 of ARM instruction
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
    switch ((instruction >> 25u) & 0x7u) {

    // Case 0: Data Processing (register) / BX / MUL / MULL / Halfword-Signed / MRS / MSR(reg)
    case 0: {
        // BX
        if ((instruction & 0x0FFFFFF0u) == 0x012FFF10u) {
            const auto current_instruction = state_.regs[15] - 4u;
            const auto target = read_visible_reg(instruction & 0xFu);
            branch_to(target, test_bit(target, 0));
            current_cycle_ += 2u * sequential_code_cycles(bus_.waitcnt(), current_instruction, BusWidth::Word);
            return 0;
        }

        // MUL
        if ((instruction & 0x0FC000F0u) == 0x00000090u) {
            const auto accumulate = test_bit(instruction, 21);
            const auto set_flags = test_bit(instruction, 20);
            const auto rd = (instruction >> 16u) & 0xFu;
            const auto rn = (instruction >> 12u) & 0xFu;
            const auto rs = (instruction >> 8u) & 0xFu;
            const auto rm = instruction & 0xFu;
            const auto multiplier = read_visible_reg(rs);
            const auto product = read_visible_reg(rm) * multiplier;
            const auto result = accumulate ? product + read_visible_reg(rn) : product;
            if (rd == 15u) {
                arm_write_pc(result);
            } else {
                state_.regs[rd] = result;
            }
            if (set_flags) {
                update_nz(result);
            }
            current_cycle_ += signed_multiply_cycles(multiplier) + (accumulate ? 1u : 0u);
            break_fetch_burst_for_internal();
            return 0;
        }

        // MULL
        if ((instruction & 0x0F8000F0u) == 0x00800090u) {
            const auto accumulate = test_bit(instruction, 21);
            const auto set_flags = test_bit(instruction, 20);
            const auto sign_extend_mul = test_bit(instruction, 22);
            const auto rd_hi = (instruction >> 16u) & 0xFu;
            const auto rd_lo = (instruction >> 12u) & 0xFu;
            const auto rs = (instruction >> 8u) & 0xFu;
            const auto rm = instruction & 0xFu;

            const auto lhs = read_visible_reg(rm);
            const auto rhs = read_visible_reg(rs);

            u64 product;
            if (sign_extend_mul) {
                const auto signed_product =
                    static_cast<s64>(sign_extend<32>(lhs)) * static_cast<s64>(sign_extend<32>(rhs));
                product = static_cast<u64>(signed_product);
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
                const auto carry = sign_extend_mul
                    ? multiply_carry_hi<true>(lhs, rhs, accum_hi)
                    : multiply_carry_hi<false>(lhs, rhs, accum_hi);
                assign_bit(state_.cpsr, 29, carry);
            }

            const auto multiply_cycles = sign_extend_mul ? signed_multiply_cycles(rhs) : unsigned_multiply_cycles(rhs);
            current_cycle_ += multiply_cycles + (accumulate ? 2u : 1u);
            break_fetch_burst_for_internal();
            return 0;
        }

        // Halfword/signed transfer / SWP
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
                                          : read_visible_reg(instruction & 0xFu);

            const auto base = read_visible_reg(rn);
            auto address = pre ? (up ? base + offset : base - offset) : base;

            if (load) {
                u32 value = 0;
                if (sh == 1u) {
                    value = arm_read16(address);
                } else if (sh == 2u) {
                    value = static_cast<u32>(sign_extend<8>(arm_read8(address)));
                } else {
                    value = (address & 1u) != 0 ? static_cast<u32>(sign_extend<8>(arm_read8(address)))
                                                : static_cast<u32>(sign_extend<16>(arm_read16(address)));
                }

                if (rd == 15u) {
                    arm_write_pc(value);
                    ++current_cycle_;
                } else {
                    state_.regs[rd] = value;
                }
                ++current_cycle_;
            } else if (sh == 1u) {
#if GBA_TRACE_TIMERS
                if ((address & ~0x3u) == 0x030000B0u) {
                    std::fprintf(stderr,
                                 "CPU STRH instr=%08X rd=%u rn=%u base=%08X addr=%08X val=%08X pre=%d up=%d imm=%d wb=%d\n",
                                 instruction, rd, rn, base, address, read_visible_reg(rd), pre ? 1 : 0, up ? 1 : 0,
                                 immediate ? 1 : 0, write_back ? 1 : 0);
                }
#endif
                arm_write16(address, read_visible_reg(rd));
            }

            if (!pre) {
                address = up ? base + offset : base - offset;
            }
            if (write_back || !pre) {
                state_.regs[rn] = address;
            }
            return 0;
        }

        // MRS
        if ((instruction & 0x0FBF0FFFu) == 0x010F0000u) {
            const auto rd = (instruction >> 12u) & 0xFu;
            state_.regs[rd] = test_bit(instruction, 22) ? spsr_for_mode(state_, mode()) : state_.cpsr;
            return 0;
        }

        // MSR (register operand)
        if ((instruction & 0x0DB0F000u) == 0x0120F000u) {
            const auto field_mask = (instruction >> 16u) & 0xFu;
            const auto value = read_visible_reg(instruction & 0xFu);

            u32 mask = 0;
            if (test_bit(field_mask, 0)) mask |= 0x000000FFu;
            if (test_bit(field_mask, 1)) mask |= 0x0000FF00u;
            if (test_bit(field_mask, 2)) mask |= 0x00FF0000u;
            if (test_bit(field_mask, 3)) mask |= 0xFF000000u;

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

        // Data Processing (register operand, bit 25=0)
        goto arm_data_processing;
    }

    // Case 1: Data Processing (immediate) / MSR(imm)
    case 1: {
        // MSR (immediate operand)
        if ((instruction & 0x0DB0F000u) == 0x0120F000u) {
            const auto field_mask = (instruction >> 16u) & 0xFu;
            const auto imm = instruction & 0xFFu;
            const auto rotate = ((instruction >> 8u) & 0xFu) * 2u;
            const auto value = rotate_right(imm, rotate);

            u32 mask = 0;
            if (test_bit(field_mask, 0)) mask |= 0x000000FFu;
            if (test_bit(field_mask, 1)) mask |= 0x0000FF00u;
            if (test_bit(field_mask, 2)) mask |= 0x00FF0000u;
            if (test_bit(field_mask, 3)) mask |= 0xFF000000u;

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

        // Data Processing (immediate operand, bit 25=1)
        goto arm_data_processing;
    }

    // Case 2-3: LDR/STR
    case 2: case 3: {
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
            offset = apply_shift(read_visible_reg(rm), shift_type, shift_imm, carry_in, true).value;
        } else {
            offset = instruction & 0x0FFFu;
        }

        const auto base = read_visible_reg(rn);
        auto address = pre ? (up ? base + offset : base - offset) : base;

        if (load) {
            const auto value = byte ? arm_read8(address) : arm_read32(address);
            if (rd == 15u) {
                arm_write_pc(value);
                ++current_cycle_;
            } else {
                state_.regs[rd] = value;
            }
            ++current_cycle_;
        } else {
            const auto value = read_visible_reg(rd);
            if (byte) {
                arm_write8(address, value);
            } else {
                arm_write32(address, value);
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

    // Case 4: LDM/STM
    case 4: {
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

        const auto base = read_visible_reg(rn);
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
                break_fetch_burst(result);
                const auto value = result.open_bus ? resolve_arm_open_bus_word(result) : result.value;
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
                break_fetch_burst(result);
            }
            access = AccessType::Sequential;
            address += 4u;
        }

        if (write_back && !(load && test_bit(register_list, rn))) {
            const auto delta = 4u * count;
            state_.regs[rn] = up ? base + delta : base - delta;
        }
        if (load) {
            auto extra_cycles = test_bit(register_list, 15u) ? 3u : 1u;
            if (!test_bit(register_list, 15u) && rn == 13u && mode() == CpuMode::Irq) {
                extra_cycles += 2u;
            }
            current_cycle_ += extra_cycles;
        }
        return 0;
    }

    // Case 5: Branch / Branch with link
    case 5: {
        const auto offset = sign_extend<26>((instruction & 0x00FFFFFFu) << 2u);
        if (test_bit(instruction, 24)) {
            state_.regs[14] = state_.regs[15];
        }
        const auto current_instruction = state_.regs[15] - 4u;
        branch_to(pc_visible() + static_cast<u32>(offset), false);
        current_cycle_ += 2u * sequential_code_cycles(bus_.waitcnt(), current_instruction, BusWidth::Word);
        return 0;
    }

    // Case 6: Coprocessor (undefined on GBA)
    case 6: {
        break;  // fall to undefined
    }

    // Case 7: SWI / Coprocessor
    case 7: {
        if ((instruction & 0x0F000000u) == 0x0F000000u) {
            const auto current_instruction = state_.regs[15] - 4u;
            if (handle_hle_swi(instruction & 0x00FFFFFFu)) {
                current_cycle_ += arm_exception_refill_cycles(bus_.waitcnt(), current_instruction);
                last_fetch_cycle_ = current_cycle_;
                return 0;
            }
            raise_exception(ExceptionType::SoftwareInterrupt);
            current_cycle_ += arm_exception_refill_cycles(bus_.waitcnt(), current_instruction);
            last_fetch_cycle_ = current_cycle_;
            return 0;
        }
        break;  // coprocessor → undefined
    }

    default:
        break;
    }
#pragma GCC diagnostic pop

    // Fall-through: undefined ARM instruction
    goto arm_undefined;

    // Data processing shared between case 0 and case 1
    arm_data_processing: {
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
                    arm_write_pc(result);
                    if (!set_flags) {
                        ++current_cycle_;
                    }
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

    arm_undefined:
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
    const auto reg = [&](u32 index) -> u32& { return state_.regs[index]; };

    // Jump-table dispatch on top byte of Thumb instruction
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
    switch (instruction >> 8) {

    // Format 1: Move shifted register / Format 2: ADD/SUB
    case 0x00 ... 0x1F: {
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
        const auto lhs = reg(rs);
        if (subtract) {
            const auto result = lhs - operand;
            reg(rd) = result;
            update_nzc_sub(lhs, operand, static_cast<u64>(result));
        } else {
            const auto wide = static_cast<u64>(lhs) + operand;
            reg(rd) = static_cast<u32>(wide);
            update_nzc_add(lhs, operand, wide);
        }
        return 0;
    }

    // Format 3: MOV/CMP/ADD/SUB immediate
    case 0x20 ... 0x3F: {
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
            const auto lhs = reg(rd);
            const auto wide = static_cast<u64>(lhs) + imm;
            reg(rd) = static_cast<u32>(wide);
            update_nzc_add(lhs, imm, wide);
            break;
        }
        case 3: {
            const auto lhs = reg(rd);
            const auto result = lhs - imm;
            reg(rd) = result;
            update_nzc_sub(lhs, imm, static_cast<u64>(result));
            break;
        }
        }
        return 0;
    }

    // Format 4: ALU operations
    case 0x40 ... 0x43: {
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
            const auto rhs = reg(rs);
            const auto result = 0 - rhs;
            reg(rd) = result;
            update_nzc_sub(0, rhs, static_cast<u64>(result));
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
            const auto multiplier = reg(rd);
            const auto product = reg(rd) * reg(rs);
            reg(rd) = product;
            update_nz(product);
            internal_cycles = signed_multiply_cycles(multiplier);
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
        if (internal_cycles != 0u) {
            break_fetch_burst_for_internal();
        }
        return 0;
    }

    // Format 5: Hi-register operations / BX
    case 0x44 ... 0x47: {
        const auto op = (instruction >> 8u) & 0x3u;
        const auto h1 = test_bit(instruction, 7);
        const auto h2 = test_bit(instruction, 6);
        const auto rs = ((h2 ? 1u : 0u) << 3u) | ((instruction >> 3u) & 0x7u);
        const auto rd = ((h1 ? 1u : 0u) << 3u) | (instruction & 0x7u);
        const auto read_hi_reg = [&](u32 index) -> u32 {
            return index == 15u ? pc_visible() : reg(index);
        };
        bool refill_pipeline = false;
        const auto current_instruction = state_.regs[15] - 2u;
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
            current_cycle_ += thumb_branch_refill_cycles(bus_.waitcnt(), current_instruction);
            last_fetch_cycle_ = current_cycle_;
        }
        return 0;
    }

    // Format 6: PC-relative load
    case 0x48 ... 0x4F: {
        const auto rd = (instruction >> 8u) & 0x7u;
        const auto word = instruction & 0xFFu;
        reg(rd) = thumb_read32((thumb_pc_visible() & ~0x3u) + (word * 4u));
        ++current_cycle_;
        return 0;
    }

    // Format 7/8: LDR/STR register offset
    case 0x50 ... 0x5F: {
        const auto op = (instruction >> 9u) & 0x7u;
        const auto rm = (instruction >> 6u) & 0x7u;
        const auto rn = (instruction >> 3u) & 0x7u;
        const auto rd = instruction & 0x7u;
        const auto address = reg(rn) + reg(rm);
        switch (op) {
        case 0:
            thumb_write32(address, reg(rd));
            break;
        case 1:
            thumb_write16(address, reg(rd));
            break;
        case 2:
            thumb_write8(address, reg(rd));
            break;
        case 3:
            reg(rd) = static_cast<u32>(sign_extend<8>(thumb_read8(address)));
            break;
        case 4:
            reg(rd) = thumb_read32(address);
            break;
        case 5:
            reg(rd) = thumb_read16(address);
            break;
        case 6:
            reg(rd) = thumb_read8(address);
            break;
        case 7:
            reg(rd) = (address & 1u) != 0 ? static_cast<u32>(sign_extend<8>(thumb_read8(address)))
                                          : static_cast<u32>(sign_extend<16>(thumb_read16(address)));
            break;
        default:
            break;
       }
       if (op >= 3) {
           ++current_cycle_;
       }
       return 0;
    }

    // Format 9: LDR/STR word/byte immediate offset
    case 0x60 ... 0x7F: {
        const auto byte = test_bit(instruction, 12);
        const auto load = test_bit(instruction, 11);
        const auto offset = (instruction >> 6u) & 0x1Fu;
        const auto rn = (instruction >> 3u) & 0x7u;
        const auto rd = instruction & 0x7u;
        const auto address = reg(rn) + (byte ? offset : offset * 4u);
        if (load) {
            reg(rd) = byte ? thumb_read8(address) : thumb_read32(address);
            ++current_cycle_;
        } else if (byte) {
            thumb_write8(address, reg(rd));
        } else {
            thumb_write32(address, reg(rd));
        }
        return 0;
    }

    // Format 10: LDRH/STRH immediate offset
    case 0x80 ... 0x8F: {
        const auto load = test_bit(instruction, 11);
        const auto offset = ((instruction >> 6u) & 0x1Fu) * 2u;
        const auto rn = (instruction >> 3u) & 0x7u;
        const auto rd = instruction & 0x7u;
        const auto address = reg(rn) + offset;
        if (load) {
            reg(rd) = thumb_read16(address);
            ++current_cycle_;
        } else {
            thumb_write16(address, reg(rd));
        }
        return 0;
    }

    // Format 11: SP-relative LDR/STR
    case 0x90 ... 0x9F: {
        const auto load = test_bit(instruction, 11);
        const auto rd = (instruction >> 8u) & 0x7u;
        const auto offset = (instruction & 0xFFu) * 4u;
        const auto address = reg(13) + offset;
        if (load) {
            reg(rd) = thumb_read32(address);
            ++current_cycle_;
        } else {
            thumb_write32(address, reg(rd));
        }
        return 0;
    }

    // Format 12: Load address (PC/SP + immediate)
    case 0xA0 ... 0xAF: {
        const auto sp = test_bit(instruction, 11);
        const auto rd = (instruction >> 8u) & 0x7u;
        const auto offset = (instruction & 0xFFu) * 4u;
        reg(rd) = (sp ? reg(13) : thumb_pc_visible()) + offset;
        return 0;
    }

    // Format 13: Adjust stack pointer
    case 0xB0: {
        const auto subtract = test_bit(instruction, 7);
        const auto offset = (instruction & 0x7Fu) * 4u;
        reg(13) = subtract ? reg(13) - offset : reg(13) + offset;
        return 0;
    }

    // Format 14: PUSH/POP
    case 0xB4: case 0xB5:
    case 0xBC: case 0xBD: {
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
                    break_fetch_burst(result);
                    access = AccessType::Sequential;
                    write_address += 4u;
                }
            }
            if (include_pc_lr) {
                const auto result = bus_.write(write_address, reg(14), BusWidth::Word, access, current_cycle_);
                current_cycle_ += result.cycles;
                break_fetch_burst(result);
            }
            reg(13) = address;
        } else {
            auto read_address = reg(13);
            for (u32 r = 0; r < 8u; ++r) {
                if (test_bit(instruction, r)) {
                    const auto result = bus_.read(read_address, BusWidth::Word, access, current_cycle_);
                    current_cycle_ += result.cycles;
                    break_fetch_burst(result);
                    reg(r) = result.open_bus ? resolve_thumb_open_bus_word(result) : result.value;
                    access = AccessType::Sequential;
                    read_address += 4u;
                }
            }
            if (include_pc_lr) {
                const auto result = bus_.read(read_address, BusWidth::Word, access, current_cycle_);
                current_cycle_ += result.cycles;
                break_fetch_burst(result);
                const auto value = result.open_bus ? resolve_thumb_open_bus_word(result) : result.value;
                branch_to(value, true);
                read_address += 4u;
            }
            reg(13) = read_address;
        }
        if (load) {
            ++current_cycle_;
        }
        return 0;
    }

    // Format 15: STMIA/LDMIA
    case 0xC0 ... 0xCF: {
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
                break_fetch_burst(result);
                reg(r) = result.open_bus ? resolve_thumb_open_bus_word(result) : result.value;
            } else {
                const auto result = bus_.write(address, reg(r), BusWidth::Word, access, current_cycle_);
                current_cycle_ += result.cycles;
                break_fetch_burst(result);
            }
            address += 4u;
            access = AccessType::Sequential;
        }
        reg(rn) = address;
        if (load) {
            ++current_cycle_;
        }
        return 0;
    }

    // Format 16: Conditional branch (B{cond})
    case 0xD0 ... 0xDE: {
        const auto condition = (instruction >> 8u) & 0xFu;
        if (condition_passed(condition)) {
            const auto offset = sign_extend<9>((instruction & 0xFFu) << 1u);
            const auto current_instruction = state_.regs[15] - 2u;
            branch_to(state_.regs[15] + 2u + static_cast<u32>(offset), true);
            current_cycle_ += thumb_branch_refill_cycles(bus_.waitcnt(), current_instruction);
            last_fetch_cycle_ = current_cycle_;
        }
        return 0;
    }

    // Format 17: SWI
    case 0xDF: {
        const auto current_instruction = state_.regs[15] - 2u;
        if (handle_hle_swi(instruction & 0x00FFu)) {
            current_cycle_ += thumb_exception_refill_cycles(bus_.waitcnt(), current_instruction);
            last_fetch_cycle_ = current_cycle_;
            return 0;
        }
        raise_exception(ExceptionType::SoftwareInterrupt);
        current_cycle_ += thumb_exception_refill_cycles(bus_.waitcnt(), current_instruction);
        last_fetch_cycle_ = current_cycle_;
        return 0;
    }

    // Format 18: Unconditional branch
    case 0xE0 ... 0xE7: {
        const auto offset = sign_extend<12>((instruction & 0x7FFu) << 1u);
        const auto current_instruction = state_.regs[15] - 2u;
        branch_to(state_.regs[15] + 2u + static_cast<u32>(offset), true);
        current_cycle_ += thumb_branch_refill_cycles(bus_.waitcnt(), current_instruction);
        last_fetch_cycle_ = current_cycle_;
        return 0;
    }

    // Format 19: Long branch with link (first instruction)
    case 0xF0 ... 0xF7: {
        const auto offset = sign_extend<23>((instruction & 0x7FFu) << 12u);
        reg(14) = pc_visible() + static_cast<u32>(offset);
        return 0;
    }

    // Format 19: Long branch with link (second instruction)
    case 0xF8 ... 0xFF: {
        const auto target = reg(14) + ((instruction & 0x7FFu) << 1u);
        reg(14) = (state_.regs[15]) | 1u;
        branch_to(target, true);
        current_cycle_ += 2;
        return 0;
    }

    default:
        break;
    }
#pragma GCC diagnostic pop

    // Undefined Thumb instruction
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
        return state_.banked_r13_r14[r13_r14_bank_index(CpuMode::User)][0];
    }
    if (reg == 14u) {
        return state_.banked_r13_r14[r13_r14_bank_index(CpuMode::User)][1];
    }
    return state_.regs[reg];
}

void Arm7tdmi::write_user_reg(u32 reg, u32 value) {
    if (reg >= 8u && reg <= 12u) {
        state_.banked_usr_r8_r12[reg - 8u] = value;
    } else if (reg == 13u) {
        state_.banked_r13_r14[r13_r14_bank_index(CpuMode::User)][0] = value;
    } else if (reg == 14u) {
        state_.banked_r13_r14[r13_r14_bank_index(CpuMode::User)][1] = value;
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
        state_.banked_fiq_r8_r14[0] = state_.regs[8];
        state_.banked_fiq_r8_r14[1] = state_.regs[9];
        state_.banked_fiq_r8_r14[2] = state_.regs[10];
        state_.banked_fiq_r8_r14[3] = state_.regs[11];
        state_.banked_fiq_r8_r14[4] = state_.regs[12];
        state_.banked_fiq_r8_r14[5] = state_.regs[13];
        state_.banked_fiq_r8_r14[6] = state_.regs[14];
    } else {
        state_.banked_usr_r8_r12[0] = state_.regs[8];
        state_.banked_usr_r8_r12[1] = state_.regs[9];
        state_.banked_usr_r8_r12[2] = state_.regs[10];
        state_.banked_usr_r8_r12[3] = state_.regs[11];
        state_.banked_usr_r8_r12[4] = state_.regs[12];
    }

    if (old_mode != CpuMode::Fiq) {
        auto& old_bank = state_.banked_r13_r14[r13_r14_bank_index(old_mode)];
        old_bank[0] = state_.regs[13];
        old_bank[1] = state_.regs[14];
    }

    state_.cpsr = (state_.cpsr & ~0x1Fu) | static_cast<u32>(new_mode);

    if (new_mode == CpuMode::Fiq) {
        state_.regs[8] = state_.banked_fiq_r8_r14[0];
        state_.regs[9] = state_.banked_fiq_r8_r14[1];
        state_.regs[10] = state_.banked_fiq_r8_r14[2];
        state_.regs[11] = state_.banked_fiq_r8_r14[3];
        state_.regs[12] = state_.banked_fiq_r8_r14[4];
        state_.regs[13] = state_.banked_fiq_r8_r14[5];
        state_.regs[14] = state_.banked_fiq_r8_r14[6];
    } else {
        state_.regs[8] = state_.banked_usr_r8_r12[0];
        state_.regs[9] = state_.banked_usr_r8_r12[1];
        state_.regs[10] = state_.banked_usr_r8_r12[2];
        state_.regs[11] = state_.banked_usr_r8_r12[3];
        state_.regs[12] = state_.banked_usr_r8_r12[4];
    }

    if (new_mode == CpuMode::Fiq) {
        state_.regs[13] = state_.banked_fiq_r8_r14[5];
        state_.regs[14] = state_.banked_fiq_r8_r14[6];
    } else {
        const auto& new_bank = state_.banked_r13_r14[r13_r14_bank_index(new_mode)];
        state_.regs[13] = new_bank[0];
        state_.regs[14] = new_bank[1];
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
