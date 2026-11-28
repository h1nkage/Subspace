// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "Common/CommonTypes.h"

namespace Orca::Music
{
inline constexpr std::size_t FRAME_SAMPLES = 96;
inline constexpr std::size_t FRAME_BYTES = FRAME_SAMPLES * 4;
inline constexpr std::size_t DMA_BYTES = 32;

using Frame = std::array<u8, FRAME_BYTES>;

bool Supported();

bool On();
void SetOn(bool on);

constexpr bool IsMusicVoice(u16 is_stream)
{
  return is_stream == 1;
}

class StreamVoices
{
public:
  static constexpr std::size_t KEPT = 8;

  bool IsMusic(u32 pb, u16 is_stream, u32 loop, u32 end, u32 current);
  void Clear();

private:
  struct Ring
  {
    u32 pb = 0;
    u32 low = 0;
    u32 high = 0;
    bool used = false;
  };
  std::array<Ring, KEPT> m_rings{};
  std::size_t m_next = 0;
};

Frame MixFrame(std::span<const int, FRAME_SAMPLES> main_left,
               std::span<const int, FRAME_SAMPLES> main_right,
               std::span<const int, FRAME_SAMPLES> music_left,
               std::span<const int, FRAME_SAMPLES> music_right,
               std::span<const u16, FRAME_SAMPLES> ramp);

class Shadow
{
public:
  static constexpr std::size_t KEPT = 32;

  void Put(u32 address, const Frame& written, const Frame& quiet);
  const u8* Find(u32 address, const u8* ram);
  void Clear();

private:
  struct Entry
  {
    u32 address = 0;
    bool used = false;
    Frame written{};
    Frame quiet{};
  };
  std::array<Entry, KEPT> m_entries{};
  std::size_t m_next = 0;
  u64 m_reads = 0;
  u64 m_quiet_reads = 0;
};

Shadow& GetShadow();
}
