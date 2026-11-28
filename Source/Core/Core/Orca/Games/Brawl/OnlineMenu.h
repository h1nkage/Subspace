// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Engine/MatchTypes.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/UX/MenuEvents.h"

namespace Core
{
class CPUThreadGuard;
}

namespace Orca::UX
{
class GuestMemory;

struct MenuState
{
  enum class Where
  {
    Other,
    Menu,
    Leaving,
  };
  Where where = Where::Other;
  u32 exit_code = 0;
};

MenuState ReadMenuState(const GuestMemory& memory);

std::optional<OnlinePick> PickForExitCode(u32 exit_code);

std::string ReadSceneName(const GuestMemory& memory);

class OnlineMenuReader
{
public:
  std::optional<OnlinePick> Boundary(const MenuState& state, bool resimulating, bool alone,
                                     u64 resyncs);
  std::optional<OnlinePick> TakeBusyPick();

private:
  bool m_started = false;
  u64 m_resyncs = 0;
  bool m_from_menu = false;
  bool m_seen = false;
  std::optional<OnlinePick> m_busy;
};

std::optional<OnlinePick> TakeLobbyPick();

bool MenusScene(std::string_view scene, std::string_view sequence);
bool OnTheMenus();

std::optional<OnlinePick> CssPick();
bool PickStands(OnlinePick pick);
std::optional<OnlinePick> QueuePickForCssCode(u32 code);

void ReadOnlineMenu(const Core::CPUThreadGuard& guard, bool resimulating, bool alone);

std::string ReadSequenceName(const GuestMemory& memory);

bool SequenceHoldsDropIn(std::string_view sequence);

bool DropInHeld();
bool FightHoldsDropIn(std::string_view scene, std::string_view sequence, bool held);
class FightHoldLatch
{
public:
  void Observe(std::string_view scene, std::string_view sequence)
  {
    m_held.store(FightHoldsDropIn(scene, sequence, Held()), std::memory_order_relaxed);
  }
  bool Held() const { return m_held.load(std::memory_order_relaxed); }
  void Reset() { m_held.store(false, std::memory_order_relaxed); }

private:
  std::atomic<bool> m_held{false};
};
void ResetFightHold();
bool FightInProgress();
bool InTraining();

namespace FriendsMove
{
constexpr u32 STEP_RUNNING = 1;
constexpr u32 STEP_EXIT = 2;
constexpr u32 EXIT_WITH_FRIENDS = 25;
constexpr int FIRST_FRIEND_FRAME = 300;
int FirstFriendFrame();
constexpr u32 MENU_SETTLE_FRAMES = 30;
constexpr u32 MENU_BUILT = 0xAC8;
constexpr u32 MENU_TASK = 0x7B4;
constexpr u32 TASK_PREV = 0x4;
constexpr u32 TASK_NEXT = 0x8;
constexpr std::string_view KEYPAD_TASK = "MuName";
constexpr int TASK_WALK_MAX = 64;
bool KeypadOpen(const GuestMemory& m, u32 menu);

struct Inputs
{
  bool present = false;
  u8 plugged = 0;
  bool queue = false;
  std::string scene;
  std::string sequence;
  u32 step = 0;
  u32 exit_code = 0;
  bool menu_built = false;
  bool keypad = false;
  int frame = 0;
  u8 tag = 0;
  u8 seen = 0;
  u8 pending = 0;
  u32 menu_since = 0;
};

struct Outputs
{
  bool keep = false;
  u8 seen = 0;
  u8 pending = 0;
  u32 menu_since = 0;
  bool move = false;
};

Outputs Decide(const Inputs& in);

bool Frame(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
           const std::vector<Events::PortInfo>& ports);
}

namespace FreshMove
{
using Seen = Orca::Engine::FreshSeen;
Seen Read(const GuestMemory& memory);

bool Apply(GuestMemory& memory, u32 exit);

constexpr u32 RNG_DEFAULT = 0x805A00B8;
constexpr u32 RNG_MENU = 0x805A0420;
constexpr u32 RNG_VTABLE = 0x8042AE50;
constexpr u32 RNG_SEED = 0x04;
u32 RngSeed(u32 seed, u32 rng);
bool ApplySeed(GuestMemory& memory, u32 seed);

void Frame(const Core::CPUThreadGuard& guard, int frame);

Seen LastSeen();

bool Supported();
}
}
