#include "gba/core/apu.hpp"

#include <algorithm>

#include "gba/core/constants.hpp"

namespace gba {

void Apu::reset() {
    soundcnt_l_ = 0;
    soundcnt_h_ = 0;
    soundcnt_x_ = 0;
    soundbias_ = 0x0200;
    fifo_[0].clear();
    fifo_[1].clear();
    mix_buffer_.clear();
    fifo_request_a_ = false;
    fifo_request_b_ = false;
}

u32 Apu::read_register(u32 address, BusWidth width) const {
    auto read_half = [&](u32 half_address) -> u16 {
        switch (half_address) {
        case kSoundCntL:
            return soundcnt_l_;
        case kSoundCntH:
            return soundcnt_h_;
        case kSoundCntX:
            return soundcnt_x_;
        case kSoundBias:
            return soundbias_;
        default:
            return 0;
        }
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

void Apu::write_register(u32 address, u32 value, BusWidth width, u64 cycle_now) {
    (void)cycle_now;

    auto write_half = [&](u32 half_address, u16 half_value) {
        switch (half_address) {
        case kSoundCntL:
            soundcnt_l_ = half_value;
            break;
        case kSoundCntH:
            soundcnt_h_ = half_value;
            if (test_bit(soundcnt_h_, 11)) {
                fifo_[0].clear();
            }
            if (test_bit(soundcnt_h_, 15)) {
                fifo_[1].clear();
            }
            break;
        case kSoundCntX:
            soundcnt_x_ = static_cast<u16>(half_value & 0x0080u);
            break;
        case kSoundBias:
            soundbias_ = half_value;
            break;
        default:
            break;
        }
    };

    if (address == kFifoA || address == kFifoA + 2u || address == kFifoB || address == kFifoB + 2u) {
        if (width == BusWidth::Word) {
            push_fifo(address == kFifoB || address == kFifoB + 2u ? 1 : 0, value);
        } else if (width == BusWidth::Half) {
            const auto word = static_cast<u32>(value & 0xFFFFu) | (static_cast<u32>(value & 0xFFFFu) << 16u);
            push_fifo(address == kFifoB || address == kFifoB + 2u ? 1 : 0, word);
        }
        return;
    }

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
}

void Apu::push_fifo(int fifo_index, u32 word) {
    auto& fifo = fifo_[static_cast<std::size_t>(fifo_index)];
    for (int byte = 0; byte < 4; ++byte) {
        fifo.push_back(static_cast<s8>((word >> (byte * 8)) & 0xFFu));
    }
    while (fifo.size() > 32u) {
        fifo.pop_front();
    }
}

void Apu::on_timer_overflow(int timer_index) {
    if (!test_bit(soundcnt_x_, 7)) {
        return;
    }

    const auto timer_for_a = test_bit(soundcnt_h_, 10) ? 1 : 0;
    const auto timer_for_b = test_bit(soundcnt_h_, 14) ? 1 : 0;

    int left = 0;
    int right = 0;

    const auto pop_sample = [&](int fifo_index) -> int {
        auto& fifo = fifo_[static_cast<std::size_t>(fifo_index)];
        int sample = 0;
        if (!fifo.empty()) {
            sample = fifo.front();
            fifo.pop_front();
        }
        if (fifo.size() <= 16u) {
            fifo_request_a_ = fifo_request_a_ || fifo_index == 0;
            fifo_request_b_ = fifo_request_b_ || fifo_index == 1;
        }
        return sample;
    };

    if (timer_index == timer_for_a) {
        const auto sample = pop_sample(0);
        const auto gain = test_bit(soundcnt_h_, 2) ? 2 : 1;
        if (test_bit(soundcnt_h_, 8)) {
            right += sample * gain;
        }
        if (test_bit(soundcnt_h_, 9)) {
            left += sample * gain;
        }
    }

    if (timer_index == timer_for_b) {
        const auto sample = pop_sample(1);
        const auto gain = test_bit(soundcnt_h_, 3) ? 2 : 1;
        if (test_bit(soundcnt_h_, 12)) {
            right += sample * gain;
        }
        if (test_bit(soundcnt_h_, 13)) {
            left += sample * gain;
        }
    }

    append_sample_pair(clamp_audio_sample(left * 256), clamp_audio_sample(right * 256));
}

void Apu::advance_to(u64 cycle_now) {
    (void)cycle_now;
}

u64 Apu::next_event_cycle() const {
    return std::numeric_limits<u64>::max();
}

bool Apu::audio_chunk_ready() const {
    return mix_buffer_.size() >= 1024u;
}

std::vector<s16> Apu::consume_audio_chunk() {
    std::vector<s16> chunk;
    chunk.swap(mix_buffer_);
    return chunk;
}

bool Apu::take_fifo_request_a() {
    const auto pending = fifo_request_a_;
    fifo_request_a_ = false;
    return pending;
}

bool Apu::take_fifo_request_b() {
    const auto pending = fifo_request_b_;
    fifo_request_b_ = false;
    return pending;
}

void Apu::append_sample_pair(s16 left, s16 right) {
    mix_buffer_.push_back(left);
    mix_buffer_.push_back(right);
}

s16 Apu::clamp_audio_sample(int sample) {
    return static_cast<s16>(std::clamp(sample, -32768, 32767));
}

}  // namespace gba
