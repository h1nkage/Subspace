// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <string_view>

#include "Common/CommonTypes.h"

namespace Orca::Engine
{
enum class RegionKind : u8
{
  Data,
  Latch,
  Text,
  Scratch,
};

struct Region
{
  u32 begin = 0;
  u32 end = 0;
  RegionKind kind = RegionKind::Data;
  std::string_view name;

  constexpr bool Contains(u32 address, u32 bytes) const
  {
    return address >= begin && address < end && bytes <= end - address;
  }
};

struct WriteCounts
{
  u64 writes = 0;
  u64 outside = 0;
};

void SetWriteRegions(std::string_view module, std::span<const Region> regions);
void NoteWrite(u32 address, u32 bytes);
WriteCounts Writes();
}
