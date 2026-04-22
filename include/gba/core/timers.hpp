#pragma once

#include <array>

#include "gba/core/types.hpp"

namespace gba {

class Apu;
class IrqController;

struct TimerChannel {
    u16 reload = 0;
    u16 pending_reload = 0;
    u16 control = 0;
    u16 pending_control = 0;
    u16 counter = 0;
    bool running = false;
    bool stop_read_bias = false;
    bool pending_reload_valid = false;
    bool pending_control_valid = false;
    s64 timestamp_started = 0;
    u64 next_event_cycle = std::numeric_limits<u64>::max();
    u64 reload_apply_cycle = std::numeric_limits<u64>::max();
    u64 control_apply_cycle = std::numeric_limits<u64>::max();
    u8 shift = 0;
    u16 mask = 0;
};

class Timers {
public:
    void reset();

    [[nodiscard]] u32 read_register(u32 address, BusWidth width, u64 cycle_now);
    [[nodiscard]] u32 read_word_register_split(u32 address, u64 lo_cycle, u64 hi_cycle);
    void write_register(u32 address, u32 value, BusWidth width, u64 cycle_now);

    void advance_to(u64 cycle_now, IrqController& irq, Apu& apu);
    [[nodiscard]] u64 next_event_cycle() const;

    [[nodiscard]] const std::array<TimerChannel, 4>& channels() const;

private:
    [[nodiscard]] static u32 timer_period_cycles(const TimerChannel& channel);
    [[nodiscard]] static u16 read_counter(const TimerChannel& channel, u64 cycle_now);
    void tick_cascade(int index, IrqController& irq, Apu& apu, u64 cycle_now);
    void overflow(int index, IrqController& irq, Apu& apu, u64 cycle_now);

    std::array<TimerChannel, 4> channels_{};
};

}  // namespace gba
