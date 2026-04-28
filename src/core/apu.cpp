#include "gba/core/apu.hpp"

#include <algorithm>

#include "gba/core/constants.hpp"

namespace gba {

void Apu::reset() {
    sound1cnt_l_ = 0; sound1cnt_h_ = 0; sound1cnt_x_ = 0;
    sound2cnt_l_ = 0; sound2cnt_h_ = 0;
    sound3cnt_l_ = 0; sound3cnt_h_ = 0; sound3cnt_x_ = 0;
    sound4cnt_l_ = 0; sound4cnt_h_ = 0;
    soundcnt_l_ = 0; soundcnt_h_ = 0; soundcnt_x_ = 0;
    soundbias_ = 0x0200;
    wave_ram_.fill(0xFFFFu);
    fifo_latch_.fill(0);
    fifo_[0].clear(); fifo_[1].clear();
    mix_buffer_count_ = 0;
    fifo_request_a_ = false; fifo_request_b_ = false;
    
    ch1_ = SquareChannel{}; ch2_ = SquareChannel{};
    ch3_ = WaveChannel{}; ch4_ = NoiseChannel{};
    
    next_sample_cycle_ = 512;
    frame_seq_cycle_ = 32768;
    frame_seq_step_ = 0;
}

u32 Apu::read_register(u32 address, BusWidth width) const {
    auto read_half = [&](u32 half_address) -> u16 {
        switch (half_address) {
        case 0x04000060u: return static_cast<u16>(sound1cnt_l_ & 0x007Fu);
        case 0x04000062u: return static_cast<u16>(sound1cnt_h_ & 0xFFC0u);
        case 0x04000064u: return static_cast<u16>(sound1cnt_x_ & 0x4000u);
        case 0x04000068u: return static_cast<u16>(sound2cnt_l_ & 0xFFC0u);
        case 0x0400006Cu: return static_cast<u16>(sound2cnt_h_ & 0x4000u);
        case 0x04000070u: return static_cast<u16>(sound3cnt_l_ & 0x00E0u);
        case 0x04000072u: return static_cast<u16>(sound3cnt_h_ & 0xE000u);
        case 0x04000074u: return static_cast<u16>(sound3cnt_x_ & 0x4000u);
        case 0x04000078u: return static_cast<u16>(sound4cnt_l_ & 0xFF00u);
        case 0x0400007Cu: return static_cast<u16>(sound4cnt_h_ & 0x40FFu);
        case kSoundCntL: return static_cast<u16>(soundcnt_l_ & 0xFF77u);
        case kSoundCntH: return static_cast<u16>(soundcnt_h_ & 0x770Fu);
        case kSoundCntX: {
            u16 x = soundcnt_x_ & 0x0080u;
            if (ch1_.active) x |= 1;
            if (ch2_.active) x |= 2;
            if (ch3_.active) x |= 4;
            if (ch4_.active) x |= 8;
            return x;
        }
        case kSoundBias: return soundbias_;
        case 0x04000090u: case 0x04000092u: case 0x04000094u: case 0x04000096u:
        case 0x04000098u: case 0x0400009Au: case 0x0400009Cu: case 0x0400009Eu:
            return wave_ram_[(half_address - 0x04000090u) / 2u];
        case kFifoA: return static_cast<u16>(fifo_latch_[0] & 0xFFFFu);
        case kFifoA + 2u: return static_cast<u16>((fifo_latch_[0] >> 16u) & 0xFFFFu);
        case kFifoB: return static_cast<u16>(fifo_latch_[1] & 0xFFFFu);
        case kFifoB + 2u: return static_cast<u16>((fifo_latch_[1] >> 16u) & 0xFFFFu);
        default: return 0;
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
    advance_to(cycle_now);

    auto write_half = [&](u32 half_address, u16 half_value) {
        switch (half_address) {
        case 0x04000060u: sound1cnt_l_ = half_value; break;
        case 0x04000062u:
            sound1cnt_h_ = half_value;
            ch1_.length = 64 - (half_value & 0x3F);
            ch1_.duty = (half_value >> 6) & 3;
            break;
        case 0x04000064u:
            sound1cnt_x_ = half_value;
            ch1_.period = half_value & 0x7FF;
            if (test_bit(half_value, 15)) {
                ch1_.active = true;
                ch1_.vol = (sound1cnt_h_ >> 12) & 0xF;
                ch1_.env_step = (sound1cnt_h_ >> 8) & 7;
                ch1_.env_timer = ch1_.env_step;
                ch1_.env_dir = test_bit(sound1cnt_h_, 11);
                if (ch1_.length == 0) ch1_.length = 64;
            }
            break;
        case 0x04000068u:
            sound2cnt_l_ = half_value;
            ch2_.length = 64 - (half_value & 0x3F);
            ch2_.duty = (half_value >> 6) & 3;
            break;
        case 0x0400006Cu:
            sound2cnt_h_ = half_value;
            ch2_.period = half_value & 0x7FF;
            if (test_bit(half_value, 15)) {
                ch2_.active = true;
                ch2_.vol = (sound2cnt_l_ >> 12) & 0xF;
                ch2_.env_step = (sound2cnt_l_ >> 8) & 7;
                ch2_.env_timer = ch2_.env_step;
                ch2_.env_dir = test_bit(sound2cnt_l_, 11);
                if (ch2_.length == 0) ch2_.length = 64;
            }
            break;
        case 0x04000070u: sound3cnt_l_ = half_value; break;
        case 0x04000072u: 
            sound3cnt_h_ = half_value; 
            ch3_.length = 256 - (half_value & 0xFF);
            break;
        case 0x04000074u:
            sound3cnt_x_ = half_value;
            ch3_.period = half_value & 0x7FF;
            if (test_bit(half_value, 15)) {
                ch3_.active = true;
                ch3_.phase = 0;
                if (ch3_.length == 0) ch3_.length = 256;
            }
            break;
        case 0x04000078u:
            sound4cnt_l_ = half_value;
            ch4_.length = 64 - (half_value & 0x3F);
            break;
        case 0x0400007Cu:
            sound4cnt_h_ = half_value;
            if (test_bit(half_value, 15)) {
                ch4_.active = true;
                ch4_.vol = (sound4cnt_l_ >> 12) & 0xF;
                ch4_.env_step = (sound4cnt_l_ >> 8) & 7;
                ch4_.env_timer = ch4_.env_step;
                ch4_.env_dir = test_bit(sound4cnt_l_, 11);
                ch4_.lfsr = 0x7FFF;
                if (ch4_.length == 0) ch4_.length = 64;
            }
            break;
        case kSoundCntL: soundcnt_l_ = half_value; break;
        case kSoundCntH:
            soundcnt_h_ = half_value;
            if (test_bit(soundcnt_h_, 11)) fifo_[0].clear();
            if (test_bit(soundcnt_h_, 15)) fifo_[1].clear();
            break;
        case kSoundCntX:
            soundcnt_x_ = half_value;
            if (!test_bit(soundcnt_x_, 7)) {
                ch1_.active = false; ch2_.active = false;
                ch3_.active = false; ch4_.active = false;
            }
            break;
        case kSoundBias: soundbias_ = half_value; break;
        case 0x04000090u: case 0x04000092u: case 0x04000094u: case 0x04000096u:
        case 0x04000098u: case 0x0400009Au: case 0x0400009Cu: case 0x0400009Eu:
            wave_ram_[(half_address - 0x04000090u) / 2u] = half_value;
            break;
        case kFifoA: case kFifoA + 2u: push_fifo(0, half_value | (half_value << 16)); break;
        case kFifoB: case kFifoB + 2u: push_fifo(1, half_value | (half_value << 16)); break;
        default: break;
        }
    };

    if (width == BusWidth::Byte) {
        const auto aligned = align_down(address, 2u);
        const auto shift = (address & 1u) * 8u;
        const auto current = read_register(aligned, BusWidth::Half);
        const auto mask = 0xFFu << shift;
        write_half(aligned, static_cast<u16>((current & ~mask) | ((value & 0xFFu) << shift)));
    } else if (width == BusWidth::Half) {
        write_half(address, static_cast<u16>(value & 0xFFFFu));
    } else {
        write_half(address, static_cast<u16>(value & 0xFFFFu));
        write_half(address + 2u, static_cast<u16>((value >> 16u) & 0xFFFFu));
    }
}

void Apu::push_fifo(int fifo_index, u32 word) {
    auto& fifo = fifo_[static_cast<std::size_t>(fifo_index)];
    for (int byte = 0; byte < 4; ++byte) {
        if (fifo.size() < 32u) fifo.push(static_cast<s8>((word >> (byte * 8)) & 0xFFu));
    }
}

void Apu::on_timer_overflow(int timer_index) {
    if (!test_bit(soundcnt_x_, 7)) return;

    const auto timer_for_a = test_bit(soundcnt_h_, 10) ? 1 : 0;
    const auto timer_for_b = test_bit(soundcnt_h_, 14) ? 1 : 0;

    if (timer_index == timer_for_a) {
        auto& fifo = fifo_[0];
        if (!fifo.empty()) fifo_latch_[0] = fifo.pop();
        if (fifo.size() <= 16u) fifo_request_a_ = true;
    }
    if (timer_index == timer_for_b) {
        auto& fifo = fifo_[1];
        if (!fifo.empty()) fifo_latch_[1] = fifo.pop();
        if (fifo.size() <= 16u) fifo_request_b_ = true;
    }
}

void Apu::advance_to(u64 cycle_now) {
    if (!test_bit(soundcnt_x_, 7)) {
        next_sample_cycle_ = cycle_now + 512;
        frame_seq_cycle_ = cycle_now + 32768;
        return;
    }

    while (cycle_now >= next_event_cycle()) {
        u64 next_event = next_event_cycle();
        
        if (next_event == frame_seq_cycle_) {
            // Clock Frame Sequencer (512 Hz)
            if (frame_seq_step_ % 2 == 0) {
                ch1_.clock_length(test_bit(sound1cnt_x_, 14));
                ch2_.clock_length(test_bit(sound2cnt_h_, 14));
                ch3_.clock_length(test_bit(sound3cnt_x_, 14));
                ch4_.clock_length(test_bit(sound4cnt_h_, 14));
            }
            if (frame_seq_step_ == 7) {
                ch1_.clock_env();
                ch2_.clock_env();
                ch4_.clock_env();
            }
            frame_seq_step_ = (frame_seq_step_ + 1) % 8;
            frame_seq_cycle_ += 32768;
        }

        if (next_event == next_sample_cycle_) {
            // Generate audio sample (32768 Hz)
            const int cycles_per_sample = 512;
            
            // Channel 1 & 2 (Square)
            auto clock_square = [cycles_per_sample](SquareChannel& ch) -> int {
                if (!ch.active) return 0;
                static const u8 duty_table[4][8] = {
                    {0,0,0,0,0,0,0,1}, {1,0,0,0,0,0,0,1}, {1,0,0,0,0,1,1,1}, {0,1,1,1,1,1,1,0}
                };
                ch.timer -= cycles_per_sample;
                if (ch.timer <= 0) {
                    ch.timer += (2048 - ch.period) * 128;
                    ch.phase = (ch.phase + 1) % 8;
                }
                return duty_table[ch.duty][ch.phase] ? ch.vol : -ch.vol;
            };

            // Channel 3 (Wave)
            auto clock_wave = [this, cycles_per_sample]() -> int {
                if (!ch3_.active || !test_bit(sound3cnt_l_, 7)) return 0;
                ch3_.timer -= cycles_per_sample;
                if (ch3_.timer <= 0) {
                    ch3_.timer += (2048 - ch3_.period) * 64;
                    ch3_.phase = (ch3_.phase + 1) % 32;
                }
                const u8 bank_offset = test_bit(sound3cnt_l_, 6) ? 16 : 0; // Dual bank bit
                const u8 sample_byte = wave_ram_[(ch3_.phase / 2) / 2] >> ((ch3_.phase % 4) * 4); // Simplified wave RAM access
                u8 sample_val = (sample_byte) & 0xF;
                
                int vol_shift = (sound3cnt_h_ >> 13) & 3;
                if (vol_shift == 0) sample_val = 0;
                else if (vol_shift == 1) sample_val = sample_val;
                else if (vol_shift == 2) sample_val >>= 1;
                else if (vol_shift == 3) sample_val >>= 2;
                
                return sample_val - 8;
            };

            // Channel 4 (Noise)
            auto clock_noise = [cycles_per_sample](NoiseChannel& ch) -> int {
                if (!ch.active) return 0;
                ch.timer -= cycles_per_sample;
                if (ch.timer <= 0) {
                    // Simple noise clocking
                    ch.timer += 512; 
                    bool bit = (ch.lfsr ^ (ch.lfsr >> 1)) & 1;
                    ch.lfsr = (ch.lfsr >> 1) | (bit << 14);
                }
                return (ch.lfsr & 1) ? ch.vol : -ch.vol;
            };

            int v1 = clock_square(ch1_);
            int v2 = clock_square(ch2_);
            int v3 = clock_wave();
            int v4 = clock_noise(ch4_);

            int left = 0, right = 0;
            
            // Mix Channels 1-4 based on SOUNDCNT_L
            const int vol_l = (soundcnt_l_ >> 4) & 7;
            const int vol_r = soundcnt_l_ & 7;
            
            auto mix_legacy = [&](int v, int bit_r, int bit_l) {
                if (test_bit(soundcnt_l_, bit_r)) right += v * vol_r;
                if (test_bit(soundcnt_l_, bit_l)) left += v * vol_l;
            };
            mix_legacy(v1, 8, 12);
            mix_legacy(v2, 9, 13);
            mix_legacy(v3, 10, 14);
            mix_legacy(v4, 11, 15);

            // Mix FIFOs based on SOUNDCNT_H
            int fifo_a = static_cast<s8>(fifo_latch_[0]);
            int fifo_b = static_cast<s8>(fifo_latch_[1]);
            
            const auto gain_a = test_bit(soundcnt_h_, 2) ? 2 : 1;
            const auto gain_b = test_bit(soundcnt_h_, 3) ? 2 : 1;
            
            if (test_bit(soundcnt_h_, 8)) right += fifo_a * gain_a * 4;
            if (test_bit(soundcnt_h_, 9)) left += fifo_a * gain_a * 4;
            if (test_bit(soundcnt_h_, 12)) right += fifo_b * gain_b * 4;
            if (test_bit(soundcnt_h_, 13)) left += fifo_b * gain_b * 4;

            append_sample_pair(clamp_audio_sample(left * 32), clamp_audio_sample(right * 32));
            next_sample_cycle_ += 512;
        }
    }
}

u64 Apu::next_event_cycle() const {
    if (!test_bit(soundcnt_x_, 7)) return std::numeric_limits<u64>::max();
    return std::min(next_sample_cycle_, frame_seq_cycle_);
}

bool Apu::audio_chunk_ready() const { return mix_buffer_count_ >= 1024u; }

std::span<const s16> Apu::consume_audio_chunk() {
    std::span<const s16> chunk{mix_buffer_.data(), mix_buffer_count_};
    mix_buffer_count_ = 0;
    return chunk;
}

bool Apu::take_fifo_request_a() { const auto pending = fifo_request_a_; fifo_request_a_ = false; return pending; }
bool Apu::take_fifo_request_b() { const auto pending = fifo_request_b_; fifo_request_b_ = false; return pending; }

void Apu::append_sample_pair(s16 left, s16 right) {
    if (mix_buffer_count_ + 2 <= mix_buffer_.size()) {
        mix_buffer_[mix_buffer_count_++] = left;
        mix_buffer_[mix_buffer_count_++] = right;
    }
}

s16 Apu::clamp_audio_sample(int sample) {
    return static_cast<s16>(std::clamp(sample, -32768, 32767));
}

}  // namespace gba
