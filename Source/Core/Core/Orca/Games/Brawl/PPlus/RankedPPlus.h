// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/UX/RankedSteps.h"
#include "Core/Orca/Games/Brawl/StageCursors.h"
#include "Core/Rollback/InputGate.h"

namespace Core
{
class CPUThreadGuard;
}

namespace Orca::UX
{
class GuestMemory;
}

namespace Orca::UX::RankedPPlus
{
constexpr u32 RSS_EXDATA = 0x8042C4E8;
constexpr u32 RSS_EXDATA_SIZE = 0x320;
constexpr u32 PRESET_BYTES = 0xA8;
constexpr u32 PAGE_INDEX = 0x8042C821;
constexpr u32 STAGE_STRIKE_TABLE = 0x8042C822;
constexpr u32 STRIKE_PAGES = 5;
constexpr u32 CURRENT_PAGE = 0x80496000;
constexpr u32 SSS_HOVERED = 0x244;
constexpr u32 SSS_SELECTED = 0x248;
constexpr u32 SSS_CONTROLLER = 0x278;
constexpr u32 SSS_CURSOR = 0x200;
constexpr u32 SSS_CURSOR_X = 0x3C;
constexpr u32 SSS_CURSOR_Y = 0x40;
constexpr u32 SSS_PAGE = 0x228;
constexpr u32 CUSTOM_PAGE = 2;
constexpr u32 PAGE_ITEM = 0x35;
constexpr u32 RANDOM_ITEM = 0x36;
// Project+'s My Music from the stage select (Source/Community/MusicSelect.asm): a step word, then
// the song, stage, page, mode and hidden alts it carries there and back. Step 1 hides the stage
// select's tiles and turns its pick into My Music; 4 backs it out; 7 picks a stage by itself.
constexpr u32 MUSIC_SELECT = 0x80002810;
constexpr std::array<u32, 10> MUSIC_SELECT_WORDS{0x00, 0x08, 0x0C, 0x10, 0x14,
                                                 0x18, 0x20, 0x24, 0x28, 0x2C};

extern const u8 kSwitch03[RSS_EXDATA_SIZE];

int LegalIndexOfKind(int kind);
std::vector<int> PageZero(const GuestMemory& memory);

int ClearStaleMusicSelect(GuestMemory& memory);

int Apply(GuestMemory& memory, int frame, bool two_players);
Rollback::InputGate::Masks Masks(const GuestMemory& memory);

struct Lines
{
  std::string set;
  std::string turn;
  bool operator==(const Lines&) const = default;
};
Lines Describe(const GuestMemory& memory, int frame, const std::vector<Events::PortInfo>& ports);

std::optional<Ranked::Turn> CurrentTurn(const GuestMemory& memory);

StageCursors::View CursorView(const GuestMemory& memory,
                              const std::vector<Events::PortInfo>& ports, int frame);

void Frame(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
           const std::vector<Events::PortInfo>& ports, bool alone);
Lines CurrentLines();
}
