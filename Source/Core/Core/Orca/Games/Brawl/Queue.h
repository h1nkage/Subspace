// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/Policy/Queue.h"
#include "Core/Orca/UX/QueueApp.h"
#include "Core/Orca/Games/Brawl/MatchBlock.h"
#include "Core/Rollback/InputGate.h"
#include "InputCommon/GCPadStatus.h"

namespace Core
{
class CPUThreadGuard;
}

namespace Orca::UX
{
class GuestMemory;
}

namespace Orca::UX::Queue
{
constexpr u32 READY = MatchBlock::QUEUE + 0x00;
constexpr u32 FLAGS = MatchBlock::QUEUE + 0x01;
constexpr u32 TIMEOUT_WHO = MatchBlock::QUEUE + 0x02;
constexpr u32 LOCKED = MatchBlock::QUEUE + 0x03;
constexpr u32 TIMER_START = MatchBlock::QUEUE + 0x04;
constexpr u32 RAW = MatchBlock::QUEUE + 0x08;
constexpr u32 RAW_PREV = MatchBlock::QUEUE + 0x0C;
constexpr u32 TIMER = MatchBlock::QUEUE + 0x10;
constexpr u32 STEER = MatchBlock::QUEUE + 0x12;
constexpr u32 PICK = MatchBlock::QUEUE + 0x14;
constexpr u32 STEER_START = MatchBlock::QUEUE + 0x20;
constexpr u32 COSTUME_START = MatchBlock::QUEUE + 0x24;
constexpr u32 HAND_PREV = MatchBlock::QUEUE + 0x28;
constexpr u32 REGION_END = MatchBlock::QUEUE + 0x30;
static_assert(REGION_END <= MatchBlock::QUEUE + MatchBlock::QUEUE_SIZE);

using QueueApp::DecodeIdentity;
using QueueApp::EncodeIdentity;
using QueueApp::Identity;

constexpr std::string_view TIP_TEXT = "New \u00b7 Hold B to leave";
constexpr std::string_view TIP_KEY = "hold-b-leave";
constexpr int TIP_FRAMES = 6 * 60;
constexpr int TIP_SEEN_FRAMES = 2 * 60;
constexpr int TIP_HOLD_FRAMES = 15;
struct OnceTip
{
  bool over = false;
  int shown = 0;
  bool kept = false;
  struct Step
  {
    bool show = false;
    bool keep = false;
    bool operator==(const Step&) const = default;
  };
  Step Frame(bool on_select, bool ready, int b_frames);
};
std::string Tip();

struct Text
{
  std::string line;
  std::string note;
  std::array<std::string, 4> labels;
  std::array<bool, 4> locked{};
};
Text TextFor(const View& view, const State& state, const std::vector<Events::PortInfo>& ports,
             int local, float skip_progress);

struct OwnSteer
{
  int frames = 0;
  int costume_from = -1;
  bool has_prev = false;
  float prev_x = 0;
  float prev_y = 0;
};
struct OwnStep
{
  bool done = false;
  u8 stick_x = static_cast<u8>(GCPadStatus::MAIN_STICK_CENTER_X);
  u8 stick_y = static_cast<u8>(GCPadStatus::MAIN_STICK_CENTER_Y);
  u16 buttons = 0;
  bool operator==(const OwnStep&) const = default;
};
OwnStep StepOwnPick(const CssPort& port, const Identity& pick, OwnSteer* steer);
void ArmOwnPick(const Identity& pick);
void DisarmOwnPick();
bool OwnPickArmed();
std::optional<GCPadStatus> OwnPickPad();

class PickPlaces
{
public:
  struct Placed
  {
    int character = NO_CHARACTER;
    int costume = 0;
    bool learned = false;
    float last_x = 0, last_y = 0;
    float first_x = 0, first_y = 0;
  };
  std::optional<Placed> Frame(int port, const CssPort& css);
  std::optional<std::pair<float, float>> Find(int character) const;

private:
  struct Hover
  {
    int character = -1;
    float first_x = 0, first_y = 0, last_x = 0, last_y = 0;
  };
  std::array<Hover, 2> m_hover{};
  std::array<bool, 2> m_was_placed{};
  std::map<int, std::pair<float, float>> m_places;
};

Identity Publishable(Identity pick, const std::optional<std::pair<float, float>>& place);

bool CssTaskReadable(const GuestMemory& memory);
View ReadView(const GuestMemory& memory, const std::vector<Events::PortInfo>& ports);
State ReadState(const GuestMemory& memory);
int WriteState(GuestMemory& memory, const State& state);
int Latch(GuestMemory& memory, const std::array<std::optional<GCPadStatus>, 4>& raw);

void Frame(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
           const std::vector<Events::PortInfo>& ports, bool alone);
Rollback::InputGate::Masks GateFrame(const Core::CPUThreadGuard& guard);
bool OwnsStart(const Core::CPUThreadGuard& guard);
void LatchFrame(const Core::CPUThreadGuard& guard,
                const std::array<std::optional<GCPadStatus>, 4>& raw);

}
