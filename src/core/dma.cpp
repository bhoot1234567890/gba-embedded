#include "gba/core/dma.hpp"

#include <algorithm>

#include "gba/core/apu.hpp"
#include "gba/core/bus.hpp"
#include "gba/core/constants.hpp"
#include "gba/core/irq.hpp"

namespace gba {

namespace {

[[nodiscard]] u16 dma_irq_mask(int index) {
    return static_cast<u16>(IrqDma0 << index);
}

[[nodiscard]] u32 apply_address_mode(u32 address, u32 transfer_bytes, u32 mode) {
    switch (mode) {
    case 0:
    case 3:
        return address + transfer_bytes;
    case 1:
        return address - transfer_bytes;
    case 2:
    default:
        return address;
    }
}

}  // namespace

void DmaEngine::reset() {
    channels_.fill({});
    next_event_cycle_ = std::numeric_limits<u64>::max();
}

u32 DmaEngine::read_register(u32 address, BusWidth width) const {
    const auto channel = (address - kDma0Sad) / 12u;
    if (channel >= channels_.size()) {
        return 0;
    }

    const auto& dma = channels_[channel];
    const auto offset = (address - kDma0Sad) % 12u;

    auto read_half = [&]() -> u16 {
        switch (offset & ~1u) {
        case 0:
        case 2:
            return static_cast<u16>((dma.source >> ((offset & 2u) * 8u)) & 0xFFFFu);
        case 4:
        case 6:
            return static_cast<u16>((dma.destination >> (((offset - 4u) & 2u) * 8u)) & 0xFFFFu);
        case 8:
            return dma.word_count;
        case 10:
            return dma.control;
        default:
            return 0;
        }
    };

    if (width == BusWidth::Byte) {
        const auto shift = (address & 1u) * 8u;
        return static_cast<u32>((read_half() >> shift) & 0xFFu);
    }
    if (width == BusWidth::Half) {
        return read_half();
    }

    const auto aligned = align_down(address, 4u);
    const auto lower = read_register(aligned, BusWidth::Half);
    const auto upper = read_register(aligned + 2u, BusWidth::Half);
    return lower | (upper << 16u);
}

void DmaEngine::write_register(u32 address, u32 value, BusWidth width, u64 cycle_now) {
    const auto channel = (address - kDma0Sad) / 12u;
    if (channel >= channels_.size()) {
        return;
    }

    auto& dma = channels_[channel];
    const auto offset = (address - kDma0Sad) % 12u;

    const auto write_half = [&](u32 local_offset, u16 half_value) {
        switch (local_offset) {
        case 0:
            dma.source = (dma.source & 0xFFFF0000u) | half_value;
            break;
        case 2:
            dma.source = (dma.source & 0x0000FFFFu) | (static_cast<u32>(half_value) << 16u);
            break;
        case 4:
            dma.destination = (dma.destination & 0xFFFF0000u) | half_value;
            break;
        case 6:
            dma.destination = (dma.destination & 0x0000FFFFu) | (static_cast<u32>(half_value) << 16u);
            break;
        case 8:
            dma.word_count = half_value;
            break;
        case 10: {
            const auto was_enabled = enabled(dma);
            dma.control = half_value;
            const auto now_enabled = enabled(dma);
            if (!was_enabled && now_enabled && start_timing(dma) == DmaStartTiming::Immediate) {
                dma.pending = true;
                next_event_cycle_ = std::min(next_event_cycle_, cycle_now);
            }
            break;
        }
        default:
            break;
        }
    };

    if (width == BusWidth::Byte) {
        const auto aligned = offset & ~1u;
        const auto current = static_cast<u16>(read_register(address & ~1u, BusWidth::Half));
        const auto shift = (address & 1u) * 8u;
        const auto merged = static_cast<u16>((current & ~(0xFFu << shift)) | ((value & 0xFFu) << shift));
        write_half(aligned, merged);
    } else if (width == BusWidth::Half) {
        write_half(offset, static_cast<u16>(value));
    } else {
        write_half(offset, static_cast<u16>(value & 0xFFFFu));
        write_half(offset + 2u, static_cast<u16>((value >> 16u) & 0xFFFFu));
    }
}

void DmaEngine::request_vblank(u64 cycle_now) {
    mark_pending_if_enabled(DmaStartTiming::VBlank, cycle_now);
}

void DmaEngine::request_hblank(u64 cycle_now) {
    mark_pending_if_enabled(DmaStartTiming::HBlank, cycle_now);
}

void DmaEngine::request_fifo_a(u64 cycle_now) {
    for (std::size_t index = 1; index <= 2 && index < channels_.size(); ++index) {
        auto& channel = channels_[index];
        if (enabled(channel) && start_timing(channel) == DmaStartTiming::Special && channel.destination == kFifoA) {
            channel.pending = true;
            next_event_cycle_ = std::min(next_event_cycle_, cycle_now);
        }
    }
}

void DmaEngine::request_fifo_b(u64 cycle_now) {
    for (std::size_t index = 1; index <= 2 && index < channels_.size(); ++index) {
        auto& channel = channels_[index];
        if (enabled(channel) && start_timing(channel) == DmaStartTiming::Special && channel.destination == kFifoB) {
            channel.pending = true;
            next_event_cycle_ = std::min(next_event_cycle_, cycle_now);
        }
    }
}

bool DmaEngine::has_pending_transfer() const {
    for (const auto& channel : channels_) {
        if (channel.pending && enabled(channel)) {
            return true;
        }
    }
    return false;
}

u64 DmaEngine::next_event_cycle() const {
    return next_event_cycle_;
}

u32 DmaEngine::service_due(u64 cycle_now, Bus& bus, IrqController& irq) {
    if (next_event_cycle_ > cycle_now) {
        return 0;
    }

    u32 cycles_consumed = 0;
    next_event_cycle_ = std::numeric_limits<u64>::max();

    for (std::size_t index = 0; index < channels_.size(); ++index) {
        auto& channel = channels_[index];
        if (!channel.pending || !enabled(channel)) {
            continue;
        }

        const auto fifo_mode = (index == 1u || index == 2u) &&
                               start_timing(channel) == DmaStartTiming::Special &&
                               (channel.destination == kFifoA || channel.destination == kFifoB);
        const auto units = fifo_mode ? 4u : transfer_count(channel);
        const auto unit_bytes = fifo_mode ? 4u : transfer_unit_bytes(channel);

        const auto dest_mode = static_cast<u32>((channel.control >> 5u) & 0x3u);
        const auto src_mode = static_cast<u32>((channel.control >> 7u) & 0x3u);

        const auto original_dest = channel.destination;
        auto source = channel.source;
        auto destination = channel.destination;

        for (u32 unit = 0; unit < units; ++unit) {
            const auto transfer_cycle = cycle_now + cycles_consumed;
            const auto width = unit_bytes == 4u ? BusWidth::Word : BusWidth::Half;
            const auto read_result = bus.read(source, width, AccessType::Dma, transfer_cycle);
            cycles_consumed += read_result.cycles;
            const auto write_result = bus.write(destination, read_result.value, width, AccessType::Dma, transfer_cycle + read_result.cycles);
            cycles_consumed += write_result.cycles;

            source = apply_address_mode(source, unit_bytes, src_mode == 3u ? 2u : src_mode);
            destination = fifo_mode ? destination : apply_address_mode(destination, unit_bytes, dest_mode);
        }

        channel.source = source;
        channel.destination = destination;
        channel.pending = false;

        const auto repeat = test_bit(channel.control, 9);
        if (repeat && start_timing(channel) != DmaStartTiming::Immediate) {
            if (dest_mode == 3u) {
                channel.destination = original_dest;
            }
        } else {
            finish_channel(static_cast<int>(index));
        }

        if (test_bit(channel.control, 14)) {
            irq.request(dma_irq_mask(static_cast<int>(index)));
        }
    }

    return cycles_consumed;
}

const std::array<DmaChannel, 4>& DmaEngine::channels() const {
    return channels_;
}

DmaStartTiming DmaEngine::start_timing(const DmaChannel& channel) {
    return static_cast<DmaStartTiming>((channel.control >> 12u) & 0x3u);
}

bool DmaEngine::enabled(const DmaChannel& channel) {
    return test_bit(channel.control, 15);
}

u32 DmaEngine::transfer_count(const DmaChannel& channel) {
    if (channel.word_count != 0) {
        return channel.word_count;
    }
    return 0x4000u;
}

u32 DmaEngine::transfer_unit_bytes(const DmaChannel& channel) {
    return test_bit(channel.control, 10) ? 4u : 2u;
}

void DmaEngine::mark_pending_if_enabled(DmaStartTiming timing, u64 cycle_now) {
    for (auto& channel : channels_) {
        if (enabled(channel) && start_timing(channel) == timing) {
            channel.pending = true;
            next_event_cycle_ = std::min(next_event_cycle_, cycle_now);
        }
    }
}

void DmaEngine::finish_channel(int index) {
    auto& channel = channels_[static_cast<std::size_t>(index)];
    channel.control = static_cast<u16>(channel.control & ~0x8000u);
}

}  // namespace gba
