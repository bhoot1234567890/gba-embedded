#include "gba/core/timers.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>

#include "gba/core/apu.hpp"
#include "gba/core/constants.hpp"
#include "gba/core/irq.hpp"

namespace gba {

namespace {

#ifndef GBA_TRACE_TIMERS
#define GBA_TRACE_TIMERS 0
#endif

constexpr std::array<u8, 4> kTimerShift{0, 6, 8, 10};
constexpr std::array<u16, 4> kTimerMask{0, 0x003Fu, 0x00FFu, 0x03FFu};

#if GBA_TRACE_TIMERS
std::array<u64, 4> g_last_disable_cycle{
    std::numeric_limits<u64>::max(),
    std::numeric_limits<u64>::max(),
    std::numeric_limits<u64>::max(),
    std::numeric_limits<u64>::max(),
};
std::array<int, 4> g_post_disable_word_reads{};
#endif

enum class PendingTimerEventType : u8 {
    None = 0,
    Overflow = 1,
    ReloadWrite = 2,
    ControlWrite = 3,
};

struct PendingTimerEvent {
    PendingTimerEventType type = PendingTimerEventType::None;
    int index = -1;
    u64 cycle = std::numeric_limits<u64>::max();
    u8 priority = 0xFFu;
};

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

[[nodiscard]] u32 counter_delta_since_last_update(const TimerChannel& channel, u64 cycle_now) {
    if (!channel.running || timer_count_up(channel)) {
        return 0;
    }

    const auto delta = static_cast<s64>(cycle_now) - channel.timestamp_started;
    if (delta <= 0) {
        return 0;
    }

    return static_cast<u32>(static_cast<u64>(delta) >> channel.shift);
}

void start_channel(TimerChannel& channel, u64 cycle_now, int cycle_offset) {
    const auto cycles =
        static_cast<s64>((0x10000u - static_cast<u32>(channel.counter)) << channel.shift) - cycle_offset;

    channel.running = true;
    channel.timestamp_started = static_cast<s64>(cycle_now) - cycle_offset;
    channel.next_event_cycle =
        cycle_now + static_cast<u64>(std::max<s64>(cycles, 1));
}

bool stop_channel(TimerChannel& channel, u64 cycle_now) {
    const auto current = static_cast<u32>(channel.counter) + counter_delta_since_last_update(channel, cycle_now);
    channel.counter = static_cast<u16>(current);
    channel.running = false;
    channel.timestamp_started = static_cast<s64>(cycle_now);
    channel.next_event_cycle = std::numeric_limits<u64>::max();
    return current >= 0x10000u;
}

PendingTimerEvent next_pending_event(const std::array<TimerChannel, 4>& channels, u64 cycle_limit) {
    PendingTimerEvent best{};

    const auto consider = [&](u64 cycle, PendingTimerEventType type, int index, u8 priority) {
        if (cycle > cycle_limit) {
            return;
        }
        if (cycle < best.cycle || (cycle == best.cycle && priority < best.priority)) {
            best = PendingTimerEvent{type, index, cycle, priority};
        }
    };

    for (std::size_t index = 0; index < channels.size(); ++index) {
        const auto& channel = channels[index];
        if (channel.running && !timer_count_up(channel) &&
            channel.next_event_cycle != std::numeric_limits<u64>::max()) {
            consider(channel.next_event_cycle, PendingTimerEventType::Overflow, static_cast<int>(index), 0);
        }
        if (channel.pending_reload_valid) {
            consider(channel.reload_apply_cycle, PendingTimerEventType::ReloadWrite, static_cast<int>(index), 1);
        }
        if (channel.pending_control_valid) {
            consider(channel.control_apply_cycle, PendingTimerEventType::ControlWrite, static_cast<int>(index), 2);
        }
    }

    return best;
}

void apply_reload_write(TimerChannel& channel) {
    channel.reload = channel.pending_reload;
    channel.pending_reload_valid = false;
    channel.reload_apply_cycle = std::numeric_limits<u64>::max();
}

}  // namespace

void Timers::reset() {
    for (auto& channel : channels_) {
        channel = {};
        channel.next_event_cycle = std::numeric_limits<u64>::max();
        channel.reload_apply_cycle = std::numeric_limits<u64>::max();
        channel.control_apply_cycle = std::numeric_limits<u64>::max();
    }
#if GBA_TRACE_TIMERS
    trace_context_ = {};
    g_last_disable_cycle.fill(std::numeric_limits<u64>::max());
    g_post_disable_word_reads.fill(0);
#endif
}

u32 Timers::read_register(u32 address, BusWidth width, u64 cycle_now) {
    const auto timer = (address - kTm0CntL) / 4u;
    if (timer >= channels_.size()) {
        return 0;
    }

    const auto& channel = channels_[timer];
    const auto read_half = [&](u32 half_address) -> u16 {
        return ((half_address - kTm0CntL) & 0x2u) == 0 ? read_counter(channel, cycle_now) : channel.control;
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

u32 Timers::read_word_register_split(u32 address, u64 lo_cycle, u64 hi_cycle) {
    const auto timer = (address - kTm0CntL) / 4u;
    if (timer >= channels_.size()) {
        return 0;
    }

    const auto& channel = channels_[timer];
    (void)hi_cycle;
    // Timer word reads behave like one latched MMIO snapshot rather than a
    // low-half sample followed by a separately advancing control-half sample.
    const auto sample_cycle = lo_cycle;
    const auto lo = static_cast<u16>(channel.counter + counter_delta_since_last_update(channel, sample_cycle));
    const auto hi = channel.pending_control_valid && sample_cycle >= channel.control_apply_cycle
        ? channel.pending_control
        : channel.control;
#if GBA_TRACE_TIMERS
    if (timer == 0 && g_last_disable_cycle[timer] != std::numeric_limits<u64>::max() &&
        g_post_disable_word_reads[timer] < 6) {
        std::fprintf(stderr,
                     "TMR read32 after disable suite=%d test=%d sub=%d info=%08X t=%llu delta=%llu lo=%04X hi=%04X ctrl=%04X pending=%04X pending_valid=%d apply=%llu running=%d\n",
                     trace_context_.suite_id, trace_context_.test_id, trace_context_.subtest_id,
                     trace_context_.info_address,
                     static_cast<unsigned long long>(sample_cycle),
                     static_cast<unsigned long long>(sample_cycle - g_last_disable_cycle[timer]), lo, hi,
                     channel.control, channel.pending_control, channel.pending_control_valid ? 1 : 0,
                     static_cast<unsigned long long>(channel.control_apply_cycle), channel.running ? 1 : 0);
        ++g_post_disable_word_reads[timer];
    }
#endif
    return static_cast<u32>(lo) | (static_cast<u32>(hi) << 16u);
}

void Timers::write_register(u32 address, u32 value, BusWidth width, u64 cycle_now,
                            IrqController& irq, Apu& apu) {
    const auto timer = (address - kTm0CntL) / 4u;
    if (timer >= channels_.size()) {
        return;
    }

    auto& channel = channels_[timer];

    const auto current_half = [&](u32 half_address) -> u16 {
        if (((half_address - kTm0CntL) & 0x2u) == 0) {
            return channel.pending_reload_valid ? channel.pending_reload : channel.reload;
        }
        return channel.pending_control_valid ? channel.pending_control : channel.control;
    };

    const auto write_half = [&](u32 half_address, u16 half_value) {
        if (((half_address - kTm0CntL) & 0x2u) == 0) {
#if GBA_TRACE_TIMERS
            if (timer == 0) {
                std::fprintf(stderr, "TMR write reload t=%llu val=%04X apply=%llu\n",
                             static_cast<unsigned long long>(cycle_now), half_value,
                             static_cast<unsigned long long>(cycle_now + 1u));
            }
#endif
            channel.pending_reload = half_value;
            channel.pending_reload_valid = true;
            channel.reload_apply_cycle = cycle_now + 1u;
            return;
        }

        channel.pending_control = static_cast<u16>(half_value & 0x00C7u);
        if (timer == 0) {
            channel.pending_control = static_cast<u16>(channel.pending_control & ~0x0004u);
        }
        const auto disable_running = !test_bit(channel.pending_control, 7) && timer_enabled(channel) && channel.running;
        if (disable_running) {
            if (stop_channel(channel, cycle_now)) {
                overflow(static_cast<int>(timer), irq, apu, cycle_now);
            }
#if GBA_TRACE_TIMERS
            g_last_disable_cycle[timer] = cycle_now;
            g_post_disable_word_reads[timer] = 0;
            if (timer == 0) {
                std::fprintf(stderr,
                             "TMR disable write suite=%d test=%d sub=%d info=%08X t=%llu counter=%04X ctrl=%04X pending=%04X apply=%llu\n",
                             trace_context_.suite_id, trace_context_.test_id, trace_context_.subtest_id,
                             trace_context_.info_address,
                             static_cast<unsigned long long>(cycle_now), channel.counter, channel.control,
                             channel.pending_control, static_cast<unsigned long long>(cycle_now + 1u));
            }
#endif
        }
        const auto apply_cycle = cycle_now + (disable_running ? 2u : 1u);
#if GBA_TRACE_TIMERS
        if (timer == 0) {
            std::fprintf(stderr, "TMR write control t=%llu val=%04X apply=%llu width=%u addr=%08X\n",
                         static_cast<unsigned long long>(cycle_now), channel.pending_control,
                         static_cast<unsigned long long>(apply_cycle), static_cast<unsigned>(width),
                         half_address);
        }
#endif
        channel.pending_control_valid = true;
        channel.control_apply_cycle = apply_cycle;
    };

    if (width == BusWidth::Byte) {
        const auto aligned = align_down(address, 2u);
        const auto shift = (address & 1u) * 8u;
        const auto merged =
            static_cast<u16>((current_half(aligned) & ~(0xFFu << shift)) | ((value & 0xFFu) << shift));
        write_half(aligned, merged);
    } else if (width == BusWidth::Half) {
        write_half(address, static_cast<u16>(value));
    } else {
        write_half(address, static_cast<u16>(value & 0xFFFFu));
        write_half(address + 2u, static_cast<u16>((value >> 16u) & 0xFFFFu));
    }
}

void Timers::advance_to(u64 cycle_now, IrqController& irq, Apu& apu) {
    while (true) {
        const auto pending = next_pending_event(channels_, cycle_now);
        if (pending.type == PendingTimerEventType::None) {
            break;
        }

        auto& channel = channels_[static_cast<std::size_t>(pending.index)];
        switch (pending.type) {
        case PendingTimerEventType::Overflow:
            overflow(pending.index, irq, apu, pending.cycle);
            break;
        case PendingTimerEventType::ReloadWrite:
#if GBA_TRACE_TIMERS
            if (pending.index == 0) {
                std::fprintf(stderr, "TMR apply reload t=%llu val=%04X\n",
                             static_cast<unsigned long long>(pending.cycle), channel.pending_reload);
            }
#endif
            apply_reload_write(channel);
            break;
        case PendingTimerEventType::ControlWrite: {
#if GBA_TRACE_TIMERS
            if (pending.index == 0) {
                std::fprintf(stderr,
                             "TMR apply control t=%llu old=%04X new=%04X running=%d counter=%04X next=%llu\n",
                             static_cast<unsigned long long>(pending.cycle), channel.control,
                             channel.pending_control, channel.running ? 1 : 0, read_counter(channel, pending.cycle),
                             static_cast<unsigned long long>(channel.next_event_cycle));
            }
#endif
            channel.pending_control_valid = false;
            channel.control_apply_cycle = std::numeric_limits<u64>::max();

            const auto enable_previous = timer_enabled(channel);
            if (channel.running) {
                if (stop_channel(channel, pending.cycle)) {
                    overflow(pending.index, irq, apu, pending.cycle);
                }
            }

            channel.control = channel.pending_control;
            channel.shift = kTimerShift[channel.control & 0x0003u];
            channel.mask = kTimerMask[channel.control & 0x0003u];

            if (!timer_enabled(channel)) {
                channel.next_event_cycle = std::numeric_limits<u64>::max();
#if GBA_TRACE_TIMERS
                if (pending.index == 0) {
                    std::fprintf(stderr, "TMR disabled t=%llu counter=%04X\n",
                                 static_cast<unsigned long long>(pending.cycle), channel.counter);
                }
#endif
                break;
            }

            if (timer_count_up(channel)) {
                if (!enable_previous) {
                    channel.counter = channel.reload;
                }
                channel.running = false;
                channel.timestamp_started = static_cast<s64>(pending.cycle);
                channel.next_event_cycle = std::numeric_limits<u64>::max();
#if GBA_TRACE_TIMERS
                if (pending.index == 0) {
                    std::fprintf(stderr, "TMR cascade enable t=%llu counter=%04X reload=%04X\n",
                                 static_cast<unsigned long long>(pending.cycle), channel.counter, channel.reload);
                }
#endif
                break;
            }

            const auto prescaler_offset = static_cast<int>(pending.cycle & channel.mask);
            if (enable_previous) {
                start_channel(channel, pending.cycle, prescaler_offset);
#if GBA_TRACE_TIMERS
                if (pending.index == 0) {
                    std::fprintf(stderr,
                                 "TMR restart t=%llu offset=%d counter=%04X next=%llu\n",
                                 static_cast<unsigned long long>(pending.cycle), prescaler_offset, channel.counter,
                                 static_cast<unsigned long long>(channel.next_event_cycle));
                }
#endif
                break;
            }

            if (channel.counter == 0xFFFFu && prescaler_offset == 0) {
                start_channel(channel, pending.cycle, 0);
            } else {
                channel.counter = channel.reload;
                start_channel(channel, pending.cycle, prescaler_offset - 1);
            }
#if GBA_TRACE_TIMERS
            if (pending.index == 0) {
                std::fprintf(stderr,
                             "TMR enabled t=%llu counter=%04X reload=%04X offset=%d next=%llu\n",
                             static_cast<unsigned long long>(pending.cycle), channel.counter, channel.reload,
                             prescaler_offset, static_cast<unsigned long long>(channel.next_event_cycle));
            }
#endif
            break;
        }
        case PendingTimerEventType::None:
            break;
        }
    }
}

u64 Timers::next_event_cycle() const {
    u64 earliest = std::numeric_limits<u64>::max();
    for (const auto& channel : channels_) {
        earliest = std::min(earliest, channel.next_event_cycle);
        earliest = std::min(earliest, channel.reload_apply_cycle);
        earliest = std::min(earliest, channel.control_apply_cycle);
    }
    return earliest;
}

const std::array<TimerChannel, 4>& Timers::channels() const {
    return channels_;
}

#if GBA_TRACE_TIMERS
void Timers::set_trace_context(u32 info_address, int suite_id, int test_id, int subtest_id) {
    trace_context_.valid = info_address != 0;
    trace_context_.info_address = info_address;
    trace_context_.suite_id = suite_id;
    trace_context_.test_id = test_id;
    trace_context_.subtest_id = subtest_id;
}
#endif

u32 Timers::timer_period_cycles(const TimerChannel& channel) {
    return 1u << channel.shift;
}

u16 Timers::read_counter(const TimerChannel& channel, u64 cycle_now) {
    return static_cast<u16>(channel.counter + counter_delta_since_last_update(channel, cycle_now));
}

void Timers::tick_cascade(int index, IrqController& irq, Apu& apu, u64 cycle_now) {
    auto& channel = channels_[static_cast<std::size_t>(index)];
    if (!timer_enabled(channel) || !timer_count_up(channel)) {
        return;
    }

    channel.counter = static_cast<u16>(channel.counter + 1u);
    if (channel.counter == 0) {
        overflow(index, irq, apu, cycle_now);
    }
}

void Timers::overflow(int index, IrqController& irq, Apu& apu, u64 cycle_now) {
    auto& channel = channels_[static_cast<std::size_t>(index)];
#if GBA_TRACE_TIMERS
    std::fprintf(stderr, "TMR overflow t=%d cyc=%llu cnt=%04X reload=%04X ctl=%04X\n", index,
                 static_cast<unsigned long long>(cycle_now), read_counter(channel, cycle_now),
                 channel.reload, channel.control);
#endif

    channel.counter = channel.reload;
    channel.timestamp_started = static_cast<s64>(cycle_now);

    if (timer_irq_enabled(channel)) {
        irq.raise_delayed(timer_irq_mask(index), cycle_now);
    }

    apu.on_timer_overflow(index);

    if (index + 1 < static_cast<int>(channels_.size())) {
        tick_cascade(index + 1, irq, apu, cycle_now);
    }

    if (channel.running && !timer_count_up(channel)) {
        start_channel(channel, cycle_now, 0);
    } else {
        channel.next_event_cycle = std::numeric_limits<u64>::max();
    }
}

}  // namespace gba
