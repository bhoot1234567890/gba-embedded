#pragma once

#include <array>
#include <deque>
#include <vector>

#include "gba/core/types.hpp"

namespace gba {

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
    std::vector<s16> consume_audio_chunk();
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
    std::array<std::deque<s8>, 2> fifo_{};
    std::vector<s16> mix_buffer_{};
    bool fifo_request_a_ = false;
    bool fifo_request_b_ = false;
};

}  // namespace gba
