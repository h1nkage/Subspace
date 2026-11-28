// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace Orca::UX::QueueApp
{
constexpr int NO_CHARACTER = 0x28;

struct Identity
{
  int rating = -1;
  int character = -1;
  int costume = 0;
  float x = 0;
  float y = 0;
  bool HasPick() const { return character >= 0 && character != NO_CHARACTER; }
  bool operator==(const Identity&) const = default;
};
std::vector<u8> EncodeIdentity(const Identity& identity);
std::optional<Identity> DecodeIdentity(const std::vector<u8>& bytes);

void Begin(bool ranked);
void End();
bool Active();
bool Ranked();
void SetOwnRating(int rating);
int OwnRating();
Identity OwnIdentity();
bool SoloReady();
bool OnSoloCss();
bool OnQueueCss();
void AfterRestore(bool keep_ready, const char* why = "");
void ClearReady();
void ConfirmTimeout(int confirmed, int local_port);
void ResetTimeouts();
bool TakeTimeoutWasMine();

void SetSoloReady(bool ready);
void SetOnSoloCss(bool on);
void SetOnQueueCss(bool on);
void SetPick(const Identity& pick);
void SetTipShown(bool shown);
bool TipShown();
bool TakeClearReady();
std::string ClearReadyWhy();
void NoteTimeout(int frame, u8 value);
}
