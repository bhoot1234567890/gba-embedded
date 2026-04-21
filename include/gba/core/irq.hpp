#pragma once

#include "gba/core/types.hpp"

namespace gba {

enum IrqBits : u16 {
    IrqVBlank = 1u << 0,
    IrqHBlank = 1u << 1,
    IrqVCount = 1u << 2,
    IrqTimer0 = 1u << 3,
    IrqTimer1 = 1u << 4,
    IrqTimer2 = 1u << 5,
    IrqTimer3 = 1u << 6,
    IrqSerial = 1u << 7,
    IrqDma0 = 1u << 8,
    IrqDma1 = 1u << 9,
    IrqDma2 = 1u << 10,
    IrqDma3 = 1u << 11,
    IrqKeypad = 1u << 12,
    IrqGamePak = 1u << 13,
};

class IrqController {
public:
    void reset();

    [[nodiscard]] u16 ie() const;
    [[nodiscard]] u16 iflags() const;
    [[nodiscard]] u16 ime() const;
    [[nodiscard]] bool line_asserted() const;

    [[nodiscard]] u32 read_register(u32 address, BusWidth width) const;
    void write_register(u32 address, u32 value, BusWidth width);

    void request(u16 mask);
    void acknowledge(u16 mask);

private:
    void recompute_line();

    u16 ie_ = 0;
    u16 if_ = 0;
    u16 ime_ = 0;
    bool line_asserted_ = false;
};

}  // namespace gba
