#include "gba/core/irq.hpp"

#include <cstdio>

#include "gba/core/constants.hpp"

namespace gba {

namespace {

#ifndef GBA_TRACE_TIMERS
#define GBA_TRACE_TIMERS 0
#endif

}  // namespace

void IrqController::reset() {
    ie_ = 0;
    if_ = 0;
    ime_ = 0;
    pending_if_ = 0;
    line_asserted_ = false;
    write_apply_cycle_ = std::numeric_limits<u64>::max();
    delayed_ = {};
}

u16 IrqController::ie() const {
    return ie_;
}

u16 IrqController::iflags() const {
    return if_;
}

u16 IrqController::ime() const {
    return ime_;
}

bool IrqController::line_asserted() const {
    return line_asserted_;
}

u32 IrqController::read_register(u32 address, BusWidth width) const {
    auto read_half = [&](u32 half_address) -> u16 {
        switch (half_address) {
        case kIe:
            return ie_;
        case kIf:
            return if_;
        case kIme:
            return ime_;
        default:
            return 0;
        }
    };

    if (width == BusWidth::Byte) {
        const auto aligned = align_down(address, 2u);
        const auto shift = (address & 1u) * 8u;
        return static_cast<u32>((read_half(aligned) >> shift) & 0xFFu);
    }
    if (width == BusWidth::Half) {
        return read_half(address);
    }
    return static_cast<u32>(read_half(address)) | (static_cast<u32>(read_half(address + 2u)) << 16u);
}

void IrqController::write_register(u32 address, u32 value, BusWidth width, u64 cycle_now) {
    auto write_half = [&](u32 half_address, u16 half_value) {
        switch (half_address) {
        case kIe:
            ie_ = static_cast<u16>(half_value & 0x3FFFu);
            break;
        case kIf:
            pending_if_ = static_cast<u16>(pending_if_ & ~half_value);
            write_apply_cycle_ = cycle_now + 1u;
            break;
        case kIme:
            ime_ = static_cast<u16>(half_value & 1u);
            break;
        default:
            break;
        }
    };

    if (width == BusWidth::Byte) {
        const auto aligned = align_down(address, 2u);
        const auto shift = (address & 1u) * 8u;
        const auto existing = static_cast<u16>(read_register(aligned, BusWidth::Half));
        const auto merged = static_cast<u16>((existing & ~(0xFFu << shift)) | ((value & 0xFFu) << shift));
        write_half(aligned, merged);
    } else if (width == BusWidth::Half) {
        write_half(address, static_cast<u16>(value));
    } else {
        write_half(address, static_cast<u16>(value & 0xFFFFu));
        write_half(address + 2u, static_cast<u16>((value >> 16u) & 0xFFFFu));
    }

    recompute_line();
}

void IrqController::request(u16 mask) {
#if GBA_TRACE_TIMERS
    if ((mask & static_cast<u16>(IrqTimer0 | IrqTimer1 | IrqTimer2 | IrqTimer3)) != 0) {
        std::fprintf(stderr, "IRQ request mask=%04X if=%04X ie=%04X ime=%04X\n", mask, if_, ie_, ime_);
    }
#endif
    if_ = static_cast<u16>(if_ | mask);
    pending_if_ = if_;
    recompute_line();
}

void IrqController::raise_delayed(u16 mask, u64 cycle_when) {
#if GBA_TRACE_TIMERS
    if ((mask & static_cast<u16>(IrqTimer0 | IrqTimer1 | IrqTimer2 | IrqTimer3)) != 0) {
        std::fprintf(stderr, "IRQ delay mask=%04X at=%llu fire=%llu\n", mask,
                     static_cast<unsigned long long>(cycle_when),
                     static_cast<unsigned long long>(cycle_when + 4));
    }
#endif
    const auto fire_cycle = cycle_when + 4;
    const auto previous_mask = delayed_.mask;
    delayed_.mask |= mask;
    delayed_.fire_cycle = previous_mask == 0 ? fire_cycle : std::min(delayed_.fire_cycle, fire_cycle);
}

void IrqController::advance(u64 cycle_now) {
    if (delayed_.mask != 0 && cycle_now >= delayed_.fire_cycle) {
#if GBA_TRACE_TIMERS
        if ((delayed_.mask & static_cast<u16>(IrqTimer0 | IrqTimer1 | IrqTimer2 | IrqTimer3)) != 0) {
            std::fprintf(stderr, "IRQ fire mask=%04X cyc=%llu ie=%04X ime=%04X\n", delayed_.mask,
                         static_cast<unsigned long long>(cycle_now), ie_, ime_);
        }
#endif
        if_ = static_cast<u16>(if_ | delayed_.mask);
        pending_if_ = if_;
        delayed_ = {};
        recompute_line();
    }

    if (write_apply_cycle_ != std::numeric_limits<u64>::max() && cycle_now >= write_apply_cycle_) {
        if_ = pending_if_;
        write_apply_cycle_ = std::numeric_limits<u64>::max();
        recompute_line();
    }
}

void IrqController::acknowledge(u16 mask) {
    if_ = static_cast<u16>(if_ & ~mask);
    pending_if_ = if_;
    recompute_line();
}

void IrqController::recompute_line() {
    line_asserted_ = (ime_ & 1u) != 0 && (ie_ & if_) != 0;
}

}  // namespace gba
