#include "gba/core/dma.hpp"

#include <algorithm>
#include <cstdio>

#include "gba/core/apu.hpp"
#include "gba/core/bus.hpp"
#include "gba/core/constants.hpp"
#include "gba/core/irq.hpp"

namespace gba {

namespace {

#ifndef GBA_TRACE_DMA
#define GBA_TRACE_DMA 0
#endif

[[nodiscard]] u16 dma_irq_mask(int index) {
    return static_cast<u16>(IrqDma0 << index);
}

[[nodiscard]] constexpr u32 src_mask(std::size_t channel) {
    return channel == 0 ? 0x07FFFFFFu : 0x0FFFFFFFu;
}

[[nodiscard]] constexpr u32 dst_mask(std::size_t channel) {
    return channel <= 2 ? 0x07FFFFFFu : 0x0FFFFFFFu;
}

[[nodiscard]] constexpr u32 len_mask(std::size_t channel) {
    return channel == 3 ? 0xFFFFu : 0x3FFFu;
}

[[nodiscard]] constexpr u16 control_read_mask(std::size_t channel) {
    return channel == 3 ? 0xFFE0u : 0xF7E0u;
}

[[nodiscard]] u32 apply_src_mode(u32 address, u32 transfer_bytes, u32 mode) {
    switch (mode) {
    case 0:
        return address + transfer_bytes;
    case 1:
        return address - transfer_bytes;
    case 2:
    case 3:
    default:
        return address;
    }
}

[[nodiscard]] u32 apply_dst_mode(u32 address, u32 transfer_bytes, u32 mode) {
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
            return 0;
        case 10:
            return static_cast<u16>(dma.control & control_read_mask(channel));
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
            dma.destination =
                (dma.destination & 0x0000FFFFu) | (static_cast<u32>(half_value) << 16u);
            break;
        case 8:
            dma.word_count = static_cast<u16>(half_value & len_mask(channel));
            break;
        case 10: {
            dma.control = half_value;
            const auto now_enabled = enabled(dma);
            if (now_enabled) {
                latch_transfer_state(channel);
                if (start_timing(dma) == DmaStartTiming::Immediate) {
                    dma.pending = true;
                    dma.activation_cycle = cycle_now + 3u;
                    next_event_cycle_ = std::min(next_event_cycle_, dma.activation_cycle);
                }
            } else {
                dma.pending = false;
                dma.activation_cycle = std::numeric_limits<u64>::max();
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
    auto& channel = channels_[1];
    if (enabled(channel) && start_timing(channel) == DmaStartTiming::Special) {
        if (channel.current_count == 0) {
            latch_transfer_state(1);
        }
        channel.pending = true;
        channel.activation_cycle = cycle_now + 2u;
        next_event_cycle_ = std::min(next_event_cycle_, channel.activation_cycle);
    }
}

void DmaEngine::request_fifo_b(u64 cycle_now) {
    auto& channel = channels_[2];
    if (enabled(channel) && start_timing(channel) == DmaStartTiming::Special) {
        if (channel.current_count == 0) {
            latch_transfer_state(2);
        }
        channel.pending = true;
        channel.activation_cycle = cycle_now + 2u;
        next_event_cycle_ = std::min(next_event_cycle_, channel.activation_cycle);
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
    bool serviced_any = false;

    for (std::size_t index = 0; index < channels_.size(); ++index) {
        auto& channel = channels_[index];
        if (!channel.pending || !enabled(channel) || channel.activation_cycle > cycle_now) {
            continue;
        }

        if (!serviced_any) {
            cycles_consumed += 1;
            serviced_any = true;
        }

        const auto fifo_mode = (index == 1u || index == 2u) &&
                               start_timing(channel) == DmaStartTiming::Special;
        const auto units = fifo_mode ? 4u : channel.current_count;
        const auto unit_bytes = fifo_mode ? 4u : transfer_unit_bytes(channel);

        const auto dest_mode = static_cast<u32>((channel.control >> 5u) & 0x3u);
        const auto src_mode = static_cast<u32>((channel.control >> 7u) & 0x3u);

        const auto addr_align_mask = unit_bytes == 4u ? ~0x3u : ~0x1u;
        auto source = channel.current_source & addr_align_mask;
        auto destination = channel.current_destination & addr_align_mask;
#if GBA_TRACE_DMA
        std::fprintf(stderr,
                     "DMA ch=%zu src=%08X dst=%08X cnt=%u bytes=%u ctl=%04X fifo=%d\n",
                     index, source, destination, units, unit_bytes, channel.control, fifo_mode ? 1 : 0);
#endif

        const bool rom_to_vram = source >= 0x08000000u && source < 0x0E000000u &&
                                 destination >= 0x06000000u && destination < 0x06018000u &&
                                 !fifo_mode && unit_bytes == 2u;
        if (rom_to_vram) {
            bus.mark_video_dirty();
        }

        bool did_access_rom = false;

        for (u32 unit = 0; unit < units; ++unit) {
            const auto transfer_cycle = cycle_now + cycles_consumed;
            const auto width = unit_bytes == 4u ? BusWidth::Word : BusWidth::Half;

            u32 read_value;
            u32 read_cycles = 0;

            if (rom_to_vram) {
                const auto rom = bus.rom();
                const auto rom_offset = source & 0x01FFFFFFu;
                const auto seq = did_access_rom;
                did_access_rom = true;
                read_cycles = bus.dma_rom_cycles(source, false, seq);
                if (rom_offset + 2u <= rom.size()) {
                    read_value = static_cast<u32>(rom[rom_offset]) |
                                 (static_cast<u32>(rom[rom_offset + 1u]) << 8u);
                } else {
                    read_value = 0;
                }
                channel.bus_latch = (read_value << 16u) | (read_value & 0xFFFFu);

                auto vram = bus.vram_write();
                auto vram_offset = (destination - 0x06000000u) & 0x1FFFFu;
                vram[vram_offset] = static_cast<u8>(read_value);
                vram[vram_offset + 1u] = static_cast<u8>(read_value >> 8u);
                cycles_consumed += read_cycles + 1u;  // +1 for VRAM write
            } else {
                auto src_access = AccessType::Dma;
                if (source >= 0x08000000u) {
                    src_access = did_access_rom ? (AccessType::Dma | AccessType::Sequential) : AccessType::Dma;
                    did_access_rom = true;
                }

                if (source >= 0x02000000u) {
                    const auto read_result = bus.read(source, width, src_access, transfer_cycle);
                    read_cycles = read_result.cycles;
                    channel.bus_latch = unit_bytes == 2u
                        ? (read_result.value << 16u) | (read_result.value & 0xFFFFu)
                        : read_result.value;
                    read_value = read_result.value;
#if GBA_TRACE_DMA
                    std::fprintf(stderr, "DMA read ch=%zu src=%08X value=%08X cycles=%u\n",
                                 index, source, read_value, read_cycles);
#endif
                } else {
                    if (unit_bytes == 2u) {
                        read_value = (destination & 2u) ? (channel.bus_latch >> 16) : channel.bus_latch;
                    } else {
                        read_value = channel.bus_latch;
                    }
                    read_cycles = 1;
                }
                cycles_consumed += read_cycles;

                auto dst_access = AccessType::Dma;
                if (destination >= 0x08000000u) {
                    if (did_access_rom) {
                        dst_access |= AccessType::Sequential;
                    }
                    did_access_rom = true;
                }

                const auto write_result = bus.write(destination, read_value, width, dst_access, transfer_cycle + read_cycles);
                cycles_consumed += write_result.cycles;
            }

            source = apply_src_mode(source, unit_bytes, src_mode);
            destination = fifo_mode ? destination : apply_dst_mode(destination, unit_bytes, dest_mode);
        }

        channel.current_source = source;
        channel.current_destination = destination;
        channel.current_count = units;
        channel.pending = false;
        channel.activation_cycle = std::numeric_limits<u64>::max();

        const auto repeat = test_bit(channel.control, 9);
        if (repeat && start_timing(channel) != DmaStartTiming::Immediate) {
            if (fifo_mode) {
                channel.current_count = 4;
            } else {
                auto new_count = static_cast<u16>(channel.word_count & len_mask(index));
                channel.current_count = new_count == 0 ? (len_mask(index) + 1u) : new_count;
            }
            if (dest_mode == 3u && !fifo_mode) {
                auto mask = unit_bytes == 4u ? ~3u : ~1u;
                channel.current_destination = channel.destination & mask;
            }
        } else {
            finish_channel(static_cast<int>(index));
        }

        if (test_bit(channel.control, 14)) {
            irq.request(dma_irq_mask(static_cast<int>(index)));
        }
    }

    if (serviced_any) {
        cycles_consumed += 1;
    }

    for (const auto& channel : channels_) {
        if (channel.pending && enabled(channel)) {
            next_event_cycle_ = std::min(next_event_cycle_, channel.activation_cycle);
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

u32 DmaEngine::transfer_count(const DmaChannel& channel, std::size_t channel_index) {
    if (channel.word_count != 0) {
        return channel.word_count;
    }
    return channel_index == 3u ? 0x10000u : 0x4000u;
}

u32 DmaEngine::transfer_unit_bytes(const DmaChannel& channel) {
    return test_bit(channel.control, 10) ? 4u : 2u;
}

void DmaEngine::mark_pending_if_enabled(DmaStartTiming timing, u64 cycle_now) {
    for (std::size_t index = 0; index < channels_.size(); ++index) {
        auto& channel = channels_[index];
        if (enabled(channel) && start_timing(channel) == timing) {
            if (channel.current_count == 0) {
                latch_transfer_state(index);
            }
            channel.pending = true;
            channel.activation_cycle = cycle_now + 2u;
            next_event_cycle_ = std::min(next_event_cycle_, channel.activation_cycle);
        }
    }
}

void DmaEngine::finish_channel(int index) {
    auto& channel = channels_[static_cast<std::size_t>(index)];
    channel.control = static_cast<u16>(channel.control & ~0x8000u);
    channel.current_count = 0;
    channel.activation_cycle = std::numeric_limits<u64>::max();
}

void DmaEngine::latch_transfer_state(std::size_t index) {
    auto& channel = channels_[index];
    channel.current_source = channel.source & src_mask(index);
    channel.current_destination = channel.destination & dst_mask(index);
    channel.current_count = transfer_count(channel, index);
}

}  // namespace gba
