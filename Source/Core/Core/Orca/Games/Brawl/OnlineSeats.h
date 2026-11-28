// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"
#include "Core/Orca/Games/Brawl/OnlineRules.h"
#include "Core/Rollback/InputGate.h"

namespace Core
{
class CPUThreadGuard;
}

namespace Orca::UX
{
class GuestMemory;
}

namespace Orca::UX::OnlineSeats
{
constexpr u32 PICK_FRIENDS_FIRST = 24;
constexpr u32 PICK_FRIENDS_LAST = 27;
constexpr u32 PICK_CASUAL = 30;
constexpr u32 PICK_RANKED = 31;

constexpr u32 GLOBAL_SEL_CHAR = 0x10;
constexpr u32 RECORD_PLAYERS = 0xB8;
constexpr u32 RECORD_PLAYER_SIZE = 0x5C;
constexpr u32 RECORD_STATE = 0x01;
constexpr u8 STATE_HUMAN = 0;
constexpr u8 STATE_CPU = 1;
constexpr u8 STATE_NOBODY = 3;

constexpr u32 AREA_KIND_NONE = 0;
constexpr u32 AREA_KIND_HUMAN = 1;
constexpr u32 AREA_KIND_CPU = 2;
constexpr u32 AREA_MODELS = 0xB0;
constexpr u32 AREA_MODEL_COUNT = 40;
constexpr u32 MUOBJECT_SCENE_MODEL = 0x0C;
constexpr u32 SCENE_MODEL_FLAGS = 0xCC;
constexpr u32 MODEL_FLAGS_GAME = 0xA0000000;
constexpr u32 MODEL_FLAGS_HIDDEN = 0x00000060;

constexpr float TYPE_BUTTONS_A_FLOOR = -16.4f;
constexpr u32 CSS_BUTTON_NAME = Rules::CSS_BUTTON_NAME;

bool Online(const Rules::Header& header, u32 pick, int players);
constexpr u8 RecordStateFor(u8 state, bool plugged)
{
  return state == STATE_CPU || (state == STATE_HUMAN && !plugged) ? STATE_NOBODY : state;
}
constexpr u32 PanelKindFor(u32 kind)
{
  return kind == AREA_KIND_CPU ? AREA_KIND_NONE : kind;
}
bool SeatHidden(bool online, const Rules::Header& header, int port, u32 kind, bool plugged,
                bool searching);
bool HandMayPressA(const Rules::CssHand& hand);
bool PressesJoin(const Rules::CssHand& hand, int port, u32 kind, bool token_in_hand,
                 bool token_flying);

u8 PluggedInMemory(const GuestMemory& memory);
bool ReadOnline(const GuestMemory& memory, u8 plugged);
int ClearRecord(GuestMemory& memory, u8 plugged);
int ClearPanels(GuestMemory& memory, u8 plugged);
int ApplySeats(GuestMemory& memory, u8 plugged, u8* hidden = nullptr);
Rollback::InputGate::Masks GateMasks(const GuestMemory& memory);

void ClearFrame(const Core::CPUThreadGuard& guard, int frame, bool resimulating, u8 plugged);
void SeatsFrame(const Core::CPUThreadGuard& guard, int frame, bool resimulating, u8 plugged);
}
