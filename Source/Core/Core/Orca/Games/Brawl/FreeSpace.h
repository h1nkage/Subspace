// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>

#include "Common/CommonTypes.h"

namespace Orca::UX::FreeSpace
{
struct Range
{
  u32 begin = 0;
  u32 end = 0;

  constexpr u32 Size() const { return end - begin; }
  constexpr bool Contains(u32 address) const { return address >= begin && address < end; }
  constexpr bool Overlaps(const Range& other) const
  {
    return begin < other.end && other.begin < end;
  }
};

constexpr u32 kSoraSceneBase = 0x806BB480;
constexpr Range kSoraSceneText{0x806BB554, 0x806F8ADC};

constexpr Range kMatchBlock{0x806F2380, 0x806F2618};
constexpr Range kMatchBlockHead{0x806F2380, 0x806F2580};
constexpr u32 kMatchBlockLimit = kMatchBlock.end;
constexpr u32 kMatchBlockOriginalWord = 0x3A200000;

constexpr std::array<Range, 12> kCodeCaves{{
    {0x806F2674, 0x806F2D24},
    {0x806F2D74, 0x806F30E0},
    {0x806F31F0, 0x806F34CC},
    {0x806F352C, 0x806F3DA4},
    {0x806F3DA8, 0x806F3FA0},
    {0x806F3FF0, 0x806F4468},
    {0x806F45B0, 0x806F4C70},
    {0x806F4CC0, 0x806F5180},
    {0x806F52E0, 0x806F58B4},
    {0x806F6C74, 0x806F7220},
    {0x806F7328, 0x806F76F8},
    {0x806F7748, 0x806F82A4},
}};

constexpr std::array<Range, 7> kSequenceCreates{{
    {0x806F229C, 0x806F22EC},
    {0x806F2D24, 0x806F2D74},
    {0x806F30E0, 0x806F3130},
    {0x806F3FA0, 0x806F3FF0},
    {0x806F4C70, 0x806F4CC0},
    {0x806F6C24, 0x806F6C74},
    {0x806F76F8, 0x806F7748},
}};

constexpr std::array<u32, 25> kSoraMeleeRelocations{
    0x806F2368, 0x806F2618, 0x806F2634, 0x806F2654, 0x806F2670, 0x806F31EC, 0x806F34CC,
    0x806F34E8, 0x806F350C, 0x806F3528, 0x806F4468, 0x806F4550, 0x806F456C, 0x806F4590,
    0x806F45AC, 0x806F5180, 0x806F5280, 0x806F529C, 0x806F52C0, 0x806F52DC, 0x806F7220,
    0x806F72CC, 0x806F72E8, 0x806F7308, 0x806F7324,
};

constexpr u32 kPPlusHandicapNop = 0x806F3DA4;

constexpr Range kOnlineMenuBlock{0x806DC8C8, 0x806DC920};

namespace Detail
{
constexpr bool Free(const Range& range)
{
  if (range.begin % 4 != 0 || range.end % 4 != 0 || range.end <= range.begin ||
      !kSoraSceneText.Contains(range.begin) || !kSoraSceneText.Contains(range.end - 1) ||
      range.Overlaps(kOnlineMenuBlock) || range.Contains(kPPlusHandicapNop))
  {
    return false;
  }
  for (const Range& create : kSequenceCreates)
  {
    if (range.Overlaps(create))
      return false;
  }
  for (const u32 word : kSoraMeleeRelocations)
  {
    if (range.Contains(word))
      return false;
  }
  return true;
}

constexpr bool CavesValid()
{
  for (std::size_t i = 0; i < kCodeCaves.size(); ++i)
  {
    if (!Free(kCodeCaves[i]) || kCodeCaves[i].Overlaps(kMatchBlock))
      return false;
    if (i > 0 && kCodeCaves[i - 1].end > kCodeCaves[i].begin)
      return false;
  }
  return true;
}
}

static_assert(kMatchBlockHead.Size() >= 96 && kMatchBlock.begin % 32 == 0 &&
              kMatchBlockHead.begin == kMatchBlock.begin && kMatchBlockHead.end % 32 == 0 &&
              kMatchBlockHead.end <= kMatchBlock.end);
static_assert(Detail::Free(kMatchBlock));
static_assert([] {
  for (const Range& cave : kCodeCaves)
  {
    if (cave.begin < ((kMatchBlock.end + 31) & ~31u) && cave.end > (kMatchBlock.begin & ~31u))
      return false;
  }
  return true;
}());
static_assert(Detail::CavesValid());
}
