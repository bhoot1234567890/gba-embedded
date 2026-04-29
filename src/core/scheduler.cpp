#include "gba/core/scheduler.hpp"

#ifdef GBA_PLATFORM_ESP32
#include "esp_attr.h"
#else
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
#endif

namespace gba {

void IRAM_ATTR Scheduler::reset(u64 cycle) {
    current_cycle_ = cycle;
    events_.fill(std::numeric_limits<u64>::max());
    next_event_ = std::numeric_limits<u64>::max();
}

u64 IRAM_ATTR Scheduler::current_cycle() const {
    return current_cycle_;
}

void IRAM_ATTR Scheduler::set_current_cycle(u64 cycle) {
    current_cycle_ = cycle;
}

void IRAM_ATTR Scheduler::set_next_event(SchedulerSlot slot, u64 cycle) {
    const auto index = static_cast<std::size_t>(slot);
    const auto old_cycle = events_[index];
    events_[index] = cycle;
    if (cycle < next_event_) {
        next_event_ = cycle;
    } else if (old_cycle == next_event_ && cycle != old_cycle) {
        recompute_next_event();
    }
}

u64 IRAM_ATTR Scheduler::next_event() const {
    return next_event_;
}

void IRAM_ATTR Scheduler::recompute_next_event() {
    next_event_ = std::numeric_limits<u64>::max();
    for (const auto cycle : events_) {
        if (cycle < next_event_) {
            next_event_ = cycle;
        }
    }
}

}  // namespace gba
