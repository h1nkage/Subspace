// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/Games/Brawl/FreeSpace.h"
#include "Core/Orca/Policy/CharOrder.h"
#include "Core/Rollback/InputGate.h"

namespace Core
{
class CPUThreadGuard;
}

namespace Orca::UX
{
class GuestMemory;
}

namespace Orca::UX::CharOrder
{

constexpr u32 kArea = FreeSpace::kMatchBlockHead.end;
constexpr u32 kAreaSize = 0x10;
constexpr u32 kMagic = 0x5353434F;
static_assert(kArea % 16 == 0 && kArea + kAreaSize <= FreeSpace::kMatchBlockLimit &&
              FreeSpace::Detail::Free({kArea, kArea + kAreaSize}));
constexpr u32 kAnchorArea = FreeSpace::kMatchBlock.begin + 0x1F4;
constexpr u32 kAnchorAreaSize = 0x0C;
static_assert(kAnchorArea + kAnchorAreaSize <= FreeSpace::kMatchBlockHead.end &&
              FreeSpace::Detail::Free({kAnchorArea, kAnchorArea + kAnchorAreaSize}));

namespace Block
{
constexpr u32 MAGIC = 0x00;
constexpr u32 MAGIC_VALUE = 0x53534D31;
constexpr u32 VERSION = 0x04;
constexpr u32 MODE = 0x05;
constexpr u32 RULESET = 0x06;
constexpr u32 SET_WINS = 0x211;
constexpr u32 SET_DONE = 0x213;
constexpr u32 SET_LAST_WINNER = 0x215;
constexpr u8 MODE_RANKED = 2;
}

std::string Line(const SetView& set, const CssView& css, const State& state,
                 const std::vector<Events::PortInfo>& ports);

int Picker(const CssView& css, const State& state);
int CurrentPicker();
std::string PickerRibbon(int picker, const std::vector<Events::PortInfo>& ports);
std::string CurrentPickerRibbon();

SetView ReadSet(const GuestMemory& memory);
CssView ReadCss(const GuestMemory& memory);
State ReadState(const GuestMemory& memory);
int WriteState(GuestMemory& memory, const State& state);

void Frame(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
           const std::vector<Events::PortInfo>& ports);
Rollback::InputGate::Masks GateFrame(const Core::CPUThreadGuard& guard);

bool ParseTestSpec(const std::string& spec, SetView* out);
bool TestActive();
}
