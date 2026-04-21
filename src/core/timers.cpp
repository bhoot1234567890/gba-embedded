#include "gba/core/timers.hpp"

#include <algorithm>

#include "gba/core/apu.hpp"
#include "gba/core/constants.hpp"
#include "gba/core/irq.hpp"

namespace gba {

namespace {

[[nodiscard]] bool timer_enabled(const TimerChannel& channel) {
    return test_bit(channel.control, 7);
}

[[nodiscard]] bool timer_irq_enabled(const TimerChannel& channel) {
    return test_bit(channel.control, 6);
}

[[nodiscard]] bool timer_count_up(const TimerChannel& channel) {
    return test_bit(channel.control, 2);
}

[[nodiscard]] u16 timer_irq_mask(int index) {
    return static_cast<u16>(IrqTimer0 << index);
}

void update_running_timer(TimerChannel& channel, u64 cycle_now) {
    if (!channel.running || timer_count_up(channel)) {
        return;
    }

    const auto periods = std::array<u32, 4>{1u, 64u, 256u, 1024u};
    const auto period = periods[channel.control & 0x0003u];
    if (period == 0 || cycle_now <= channel.last_update_cycle) {
        return;
    }

    const auto elapsed = cycle_now - channel.last_update_cycle;
    const auto ticks = static_cast<u32>(elapsed / period);
    if (ticks == 0) {
        return;
    }

    const auto remaining = 0x10000u - channel.counter;
    if (ticks < remaining) {
        channel.counter = static_cast<u16>((channel.counter + ticks) & 0xFFFFu);
    } else {
        channel.counter = static_cast<u16>((channel.reload + ((ticks - remaining) % 0x10000u)) & 0xFFFFu);
    }
    channel.last_update_cycle = cycle_now;
}

}  // namespace

void Timers::reset() {
    for (auto& channel : channels_) {
        channel = {};
        channel.next_event_cycle = std::numeric_limits<u64>::max();
    }
}

u32 Timers::read_register(u32 address, BusWidth width) const {
    const auto timer = (address - kTm0CntL) / 4u;
    if (timer >= channels_.size()) {
        return 0;
    }

    const auto& channel = channels_[timer];
    const auto read_half = [&](u32 half_address) -> u16 {
        return ((half_address - kTm0CntL) & 0x2u) == 0 ? channel.counter : channel.control;
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

void Timers::write_register(u32 address, u32 value, BusWidth width, u64 cycle_now) {
    const auto timer = (address - kTm0CntL) / 4u;
    if (timer >= channels_.size()) {
        return;
    }

    auto& channel = channels_[timer];
    update_running_timer(channel, cycle_now);

    const auto write_half = [&](u32 half_address, u16 half_value) {
        if (((half_address - kTm0CntL) & 0x2u) == 0) {
            channel.reload = half_value;
        } else {
            const auto old_control = channel.control;
            const auto old_running = timer_enabled(channel);
            channel.control = static_cast<u16>(half_value & 0x00C7u);
            if (timer == 0) {
                channel.control = static_cast<u16>(channel.control & ~0x0004u);
            }

            const auto new_running = timer_enabled(channel);
            if (!old_running && new_running) {
                channel.counter = channel.reload;
                channel.running = true;
                channel.last_update_cycle = cycle_now;
            } else if (old_running && !new_running) {
                channel.running = false;
            } else if (old_control != channel.control && new_running) {
                channel.last_update_cycle = cycle_now;
            }
            refresh_next_event(static_cast<int>(timer), cycle_now);
        }
    };

    if (width == BusWidth::Byte) {
        const auto aligned = align_down(address, 2u);
        const auto current = static_cast<u16>(read_register(aligned, BusWidth::Half));
        const auto shift = (address & 1u) * 8u;
        const auto merged = static_cast<u16>((current & ~(0xFFu << shift)) | ((value & 0xFFu) << shift));
        write_half(aligned, merged);
    } else if (width == BusWidth::Half) {
        write_half(address, static_cast<u16>(value));
    } else {
        write_half(address, static_cast<u16>(value & 0xFFFFu));
        write_half(address + 2u, static_cast<u16>((value >> 16u) & 0xFFFFu));
    }

    refresh_next_event(static_cast<int>(timer), cycle_now);
}

void Timers::advance_to(u64 cycle_now, IrqController& irq, Apu& apu) {
    bool progressed = true;
    while (progressed) {
        progressed = false;
        for (int index = 0; index < static_cast<int>(channels_.size()); ++index) {
            auto& channel = channels_[index];
            if (channel.running && !timer_count_up(channel) && channel.next_event_cycle <= cycle_now) {
                overflow(index, irq, apu, channel.next_event_cycle);
                progressed = true;
            }
        }
    }

    for (int index = 0; index < static_cast<int>(channels_.size()); ++index) {
        update_running_timer(channels_[index], cycle_now);
    }
}

u64 Timers::next_event_cycle() const {
    u64 earliest = std::numeric_limits<u64>::max();
    for (const auto& channel : channels_) {
        earliest = std::min(earliest, channel.next_event_cycle);
    }
    return earliest;
}

const std::array<TimerChannel, 4>& Timers::channels() const {
    return channels_;
}

u32 Timers::timer_period_cycles(const TimerChannel& channel) {
    static constexpr std::array<u32, 4> periods{1u, 64u, 256u, 1024u};
    return periods[channel.control & 0x0003u];
}

void Timers::refresh_next_event(int index, u64 cycle_now) {
    auto& channel = channels_[static_cast<std::size_t>(index)];
    if (!channel.running || timer_count_up(channel)) {
        channel.next_event_cycle = std::numeric_limits<u64>::max();
        return;
    }

    const auto period = timer_period_cycles(channel);
    const auto remaining_ticks = 0x10000u - channel.counter;
    channel.next_event_cycle = cycle_now + (static_cast<u64>(remaining_ticks) * period);
}

void Timers::tick_cascade(int index, IrqController& irq, Apu& apu, u64 cycle_now) {
    auto& channel = channels_[static_cast<std::size_t>(index)];
    if (!channel.running || !timer_count_up(channel)) {
        return;
    }

    channel.counter = static_cast<u16>(channel.counter + 1u);
    if (channel.counter == 0) {
        overflow(index, irq, apu, cycle_now);
    }
}

void Timers::overflow(int index, IrqController& irq, Apu& apu, u64 cycle_now) {
    auto& channel = channels_[static_cast<std::size_t>(index)];
    channel.counter = channel.reload;
    channel.last_update_cycle = cycle_now;

    if (timer_irq_enabled(channel)) {
        irq.request(timer_irq_mask(index));
    }

    apu.on_timer_overflow(index);
    if (index + 1 < static_cast<int>(channels_.size())) {
        tick_cascade(index + 1, irq, apu, cycle_now);
    }

    refresh_next_event(index, cycle_now);
}

}  // namespace gba
