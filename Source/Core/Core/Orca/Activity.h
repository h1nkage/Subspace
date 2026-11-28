// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "InputCommon/GCPadStatus.h"

namespace Orca::UX
{
struct Snapshot;
}

namespace Orca::Activity
{
constexpr s64 INTERVAL_MS = 60'000;
constexpr s64 ACTIVE_AFTER_INPUT_MS = 30'000;

constexpr int STICK_OUT = 48;
constexpr int STICK_IN = 28;
constexpr int TRIGGER_PRESS = 128;
constexpr int TRIGGER_RELEASE = 64;
constexpr u16 BUTTONS = PAD_BUTTON_A | PAD_BUTTON_B | PAD_BUTTON_X | PAD_BUTTON_Y |
                        PAD_BUTTON_START | PAD_TRIGGER_Z | PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT |
                        PAD_BUTTON_DOWN | PAD_BUTTON_UP;

class Edges
{
public:
  int Feed(const GCPadStatus& pad);

private:
  bool m_primed = false;
  u16 m_buttons = 0;
  std::array<bool, 2> m_trigger_down{};
  std::array<bool, 2> m_stick_out{};
};

enum class Mode : u8
{
  Solo,
  Training,
  Friends,
  Casual,
  Ranked,
};
constexpr std::size_t MODES = 5;
std::string_view ModeName(Mode mode);
Mode DecideMode(std::string_view room_queue, bool friend_plugged, bool training);

struct Line
{
  s64 interval_ms = 0;
  s64 active_ms = 0;
  u32 inputs = 0;
  Mode mode = Mode::Solo;
  u32 pads = 0;
};
std::string FormatLine(const Line& line);
bool IsEmpty(const Line& line);

class Clock
{
public:
  Clock(s64 start_ms, Mode mode);
  void SetMode(s64 now_ms, Mode mode);
  void Input(s64 now_ms, int edges, u32 pad);
  s64 DueAt() const { return m_start + INTERVAL_MS; }
  Line Take(s64 now_ms);

private:
  void Advance(s64 now_ms);

  s64 m_start;
  s64 m_last;
  s64 m_active_until;
  Mode m_mode;
  std::array<s64, MODES> m_wall_ms{};
  std::array<s64, MODES> m_active_ms{};
  u32 m_inputs = 0;
  std::vector<u32> m_pads;
};

constexpr u32 PAD_ADAPTER = 0x100;
constexpr u32 PAD_GAMEPAD = 0x200;
constexpr u32 PAD_DOLPHIN = 0x300;
constexpr u32 PAD_SCRIPT = 0x400;

struct Pad
{
  u32 id = 0;
  GCPadStatus status;
};

class Reader
{
public:
  void Note(u32 id, const GCPadStatus& pad);
  void Boundary(bool first_run, bool gate, const std::vector<Pad>& direct,
                std::vector<std::pair<u32, int>>* edges);

private:
  void Feed(const Pad& pad, std::vector<std::pair<u32, int>>* edges);

  std::vector<Pad> m_noted;
  std::vector<std::pair<u32, Edges>> m_edges;
};
void SnapshotPads(const UX::Snapshot& snapshot, double age_ms, std::vector<Pad>* out);

void HoldUntilReady();
void Ready();
void NoteLocalPad(u32 id, const GCPadStatus& pad);
void OnBoundary(bool first_run);
void Finish();
}
