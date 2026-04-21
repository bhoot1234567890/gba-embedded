#include "gba/core/scheduler.hpp"

namespace gba {

void Scheduler::reset(u64 cycle) {
    current_cycle_ = cycle;
    events_.fill(std::numeric_limits<u64>::max());
}

u64 Scheduler::current_cycle() const {
    return current_cycle_;
}

void Scheduler::set_current_cycle(u64 cycle) {
    current_cycle_ = cycle;
}

void Scheduler::set_next_event(SchedulerSlot slot, u64 cycle) {
    events_[static_cast<std::size_t>(slot)] = cycle;
}

u64 Scheduler::next_event() const {
    u64 earliest = std::numeric_limits<u64>::max();
    for (const auto cycle : events_) {
        if (cycle < earliest) {
            earliest = cycle;
        }
    }
    return earliest;
}

}  // namespace gba
