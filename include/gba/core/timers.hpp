#pragma once

#include <array>

#include "gba/core/types.hpp"

namespace gba {

class Apu;
class IrqController;

struct TimerChannel {
    u16 reload = 0;
    u16 control = 0;
    u16 counter = 0;
    bool running = false;
    u64 last_update_cycle = 0;
    u64 next_event_cycle = std::numeric_limits<u64>::max();
};

class Timers {
public:
    void reset();

    [[nodiscard]] u32 read_register(u32 address, BusWidth width, u64 cycle_now);
    void write_register(u32 address, u32 value, BusWidth width, u64 cycle_now);

    void advance_to(u64 cycle_now, IrqController& irq, Apu& apu);
    [[nodiscard]] u64 next_event_cycle() const;

    [[nodiscard]] const std::array<TimerChannel, 4>& channels() const;

private:
    [[nodiscard]] static u32 timer_period_cycles(const TimerChannel& channel);
    void refresh_next_event(int index, u64 cycle_now);
    void tick_cascade(int index, IrqController& irq, Apu& apu, u64 cycle_now);
    void overflow(int index, IrqController& irq, Apu& apu, u64 cycle_now);

    std::array<TimerChannel, 4> channels_{};
};

}  // namespace gba
