#pragma once

#include "gba/core/types.hpp"

namespace gba {

constexpr u32 kSystemClockHz = 16u * 1024u * 1024u;
constexpr u32 kCyclesPerDot = 4;
constexpr u32 kVisibleDots = 240;
constexpr u32 kHblankDots = 68;
constexpr u32 kDotsPerScanline = 308;
constexpr u32 kCyclesPerScanline = 1232;
constexpr u32 kVisibleScanlines = 160;
constexpr u32 kVblankScanlines = 68;
constexpr u32 kScanlinesPerFrame = 228;
constexpr u32 kCyclesPerFrame = 280896;

constexpr u32 kScreenWidth = 240;
constexpr u32 kScreenHeight = 160;
constexpr u32 kFramebufferPixels = kScreenWidth * kScreenHeight;

constexpr u32 kBiosSize = 0x4000;
constexpr u32 kEwramSize = 0x40000;
constexpr u32 kIwramSize = 0x8000;
constexpr u32 kPaletteSize = 0x400;
constexpr u32 kVramSize = 0x18000;
constexpr u32 kOamSize = 0x400;
constexpr u32 kMaxRomSize = 32u * 1024u * 1024u;

constexpr u32 kRegBase = 0x04000000;
constexpr u32 kDispcnt = 0x04000000;
constexpr u32 kDispstat = 0x04000004;
constexpr u32 kVcount = 0x04000006;
constexpr u32 kBg0Cnt = 0x04000008;
constexpr u32 kBg2Pa = 0x04000020;
constexpr u32 kBg2X = 0x04000028;
constexpr u32 kBg3Pa = 0x04000030;
constexpr u32 kWin0H = 0x04000040;
constexpr u32 kMosaic = 0x0400004C;
constexpr u32 kBldCnt = 0x04000050;
constexpr u32 kSoundCntL = 0x04000080;
constexpr u32 kSoundCntH = 0x04000082;
constexpr u32 kSoundCntX = 0x04000084;
constexpr u32 kSoundBias = 0x04000088;
constexpr u32 kFifoA = 0x040000A0;
constexpr u32 kFifoB = 0x040000A4;
constexpr u32 kTm0CntL = 0x04000100;
constexpr u32 kTm0CntH = 0x04000102;
constexpr u32 kDma0Sad = 0x040000B0;
constexpr u32 kDma0Dad = 0x040000B4;
constexpr u32 kDma0CntL = 0x040000B8;
constexpr u32 kDma0CntH = 0x040000BA;
constexpr u32 kIe = 0x04000200;
constexpr u32 kIf = 0x04000202;
constexpr u32 kWaitCnt = 0x04000204;
constexpr u32 kIme = 0x04000208;
constexpr u32 kPostFlg = 0x04000300;
constexpr u32 kHaltCnt = 0x04000301;
constexpr u32 kKeyInput = 0x04000130;
constexpr u32 kKeyCnt = 0x04000132;

constexpr u16 kKeyA = 1u << 0;
constexpr u16 kKeyB = 1u << 1;
constexpr u16 kKeySelect = 1u << 2;
constexpr u16 kKeyStart = 1u << 3;
constexpr u16 kKeyRight = 1u << 4;
constexpr u16 kKeyLeft = 1u << 5;
constexpr u16 kKeyUp = 1u << 6;
constexpr u16 kKeyDown = 1u << 7;
constexpr u16 kKeyR = 1u << 8;
constexpr u16 kKeyL = 1u << 9;

}  // namespace gba
