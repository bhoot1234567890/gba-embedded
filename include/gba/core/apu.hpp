#pragma once

#include <array>
#include <span>
#include <vector>

#include "gba/core/types.hpp"

namespace gba {

template<typename T, size_t N>
class Fifo {
    std::array<T, N> buf_{};
    size_t head_ = 0, tail_ = 0, count_ = 0;
public:
    void push(T val) { buf_[tail_] = val; tail_ = (tail_+1) % N; ++count_; }
    T pop() { T v = buf_[head_]; head_ = (head_+1) % N; --count_; return v; }
    void clear() { head_ = tail_ = count_ = 0; }
    bool empty() const { return count_ == 0; }
    size_t size() const { return count_; }
};

class Apu {
public:
    void reset();

    [[nodiscard]] u32 read_register(u32 address, BusWidth width) const;
    void write_register(u32 address, u32 value, BusWidth width, u64 cycle_now);

    void push_fifo(int fifo_index, u32 word);
    void on_timer_overflow(int timer_index);
    void advance_to(u64 cycle_now);

    [[nodiscard]] u64 next_event_cycle() const;
    [[nodiscard]] bool audio_chunk_ready() const;
    std::span<const s16> consume_audio_chunk();
    bool take_fifo_request_a();
    bool take_fifo_request_b();

private:
    void append_sample_pair(s16 left, s16 right);
    [[nodiscard]] static s16 clamp_audio_sample(int sample);

    u16 sound1cnt_l_ = 0;
    u16 sound1cnt_h_ = 0;
    u16 sound1cnt_x_ = 0;
    u16 sound2cnt_l_ = 0;
    u16 sound2cnt_h_ = 0;
    u16 sound3cnt_l_ = 0;
    u16 sound3cnt_h_ = 0;
    u16 sound3cnt_x_ = 0;
    u16 sound4cnt_l_ = 0;
    u16 sound4cnt_h_ = 0;
    u16 soundcnt_l_ = 0;
    u16 soundcnt_h_ = 0;
    u16 soundcnt_x_ = 0;
    u16 soundbias_ = 0x0200;
    std::array<u16, 8> wave_ram_{};
    std::array<u32, 2> fifo_latch_{};
    std::array<Fifo<s8, 32>, 2> fifo_{};
    std::array<s16, 4096> mix_buffer_{};
    size_t mix_buffer_count_ = 0;
    bool fifo_request_a_ = false;
    bool fifo_request_b_ = false;
};

}  // namespace gba
