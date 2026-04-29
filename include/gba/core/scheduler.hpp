#pragma once

#include <array>
#include <limits>

#include "gba/core/types.hpp"

namespace gba {

enum class SchedulerSlot : std::size_t {
    Ppu = 0,
    Timers = 1,
    Dma = 2,
    Apu = 3,
    Serial = 4,
    Irq = 5,
    Count = 6,
};

class Scheduler {
public:
    void reset(u64 cycle = 0);
    [[nodiscard]] u64 current_cycle() const;
    void set_current_cycle(u64 cycle);
    void set_next_event(SchedulerSlot slot, u64 cycle);
    [[nodiscard]] u64 next_event() const;

private:
    void recompute_next_event();

    u64 current_cycle_ = 0;
    u64 next_event_ = std::numeric_limits<u64>::max();
    std::array<u64, static_cast<std::size_t>(SchedulerSlot::Count)> events_{
        std::numeric_limits<u64>::max(),
        std::numeric_limits<u64>::max(),
        std::numeric_limits<u64>::max(),
        std::numeric_limits<u64>::max(),
        std::numeric_limits<u64>::max(),
        std::numeric_limits<u64>::max(),
    };
};

}  // namespace gba
