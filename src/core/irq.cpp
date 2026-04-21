#include "gba/core/irq.hpp"

#include "gba/core/constants.hpp"

namespace gba {

void IrqController::reset() {
    ie_ = 0;
    if_ = 0;
    ime_ = 0;
    line_asserted_ = false;
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

void IrqController::write_register(u32 address, u32 value, BusWidth width) {
    auto write_half = [&](u32 half_address, u16 half_value) {
        switch (half_address) {
        case kIe:
            ie_ = half_value;
            break;
        case kIf:
            if_ = static_cast<u16>(if_ & ~half_value);
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
    if_ = static_cast<u16>(if_ | mask);
    recompute_line();
}

void IrqController::acknowledge(u16 mask) {
    if_ = static_cast<u16>(if_ & ~mask);
    recompute_line();
}

void IrqController::recompute_line() {
    line_asserted_ = (ime_ & 1u) != 0 && (ie_ & if_) != 0;
}

}  // namespace gba
