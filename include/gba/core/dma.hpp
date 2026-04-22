#pragma once

#include <array>
#include <limits>

#include "gba/core/types.hpp"

namespace gba {

class Bus;
class IrqController;

enum class DmaStartTiming : u8 {
    Immediate = 0,
    VBlank = 1,
    HBlank = 2,
    Special = 3,
};

struct DmaChannel {
    u32 source = 0;
    u32 destination = 0;
    u16 word_count = 0;
    u16 control = 0;
    bool pending = false;
    u64 activation_cycle = std::numeric_limits<u64>::max();
    u32 current_source = 0;
    u32 current_destination = 0;
    u32 current_count = 0;
    u32 bus_latch = 0;
};

class DmaEngine {
public:
    void reset();

    [[nodiscard]] u32 read_register(u32 address, BusWidth width) const;
    void write_register(u32 address, u32 value, BusWidth width, u64 cycle_now);

    void request_vblank(u64 cycle_now);
    void request_hblank(u64 cycle_now);
    void request_fifo_a(u64 cycle_now);
    void request_fifo_b(u64 cycle_now);

    [[nodiscard]] bool has_pending_transfer() const;
    [[nodiscard]] u64 next_event_cycle() const;
    [[nodiscard]] u32 service_due(u64 cycle_now, Bus& bus, IrqController& irq);

    [[nodiscard]] const std::array<DmaChannel, 4>& channels() const;

private:
    [[nodiscard]] static DmaStartTiming start_timing(const DmaChannel& channel);
    [[nodiscard]] static bool enabled(const DmaChannel& channel);
    [[nodiscard]] static u32 transfer_count(const DmaChannel& channel, std::size_t channel_index);
    [[nodiscard]] static u32 transfer_unit_bytes(const DmaChannel& channel);
    void mark_pending_if_enabled(DmaStartTiming timing, u64 cycle_now);
    void finish_channel(int index);
    void latch_transfer_state(std::size_t index);

    std::array<DmaChannel, 4> channels_{};
    u64 next_event_cycle_ = std::numeric_limits<u64>::max();
};

}  // namespace gba
