// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string_view>
#include <utility>

#include "Common/CommonTypes.h"
#include "Core/Orca/UX/QueueApp.h"
#include "Core/Rollback/InputGate.h"

namespace Orca::UX::Queue
{
using QueueApp::Identity;

constexpr u8 FLAG_GO = 0x01;
constexpr u8 FLAG_TIMED_OUT = 0x02;
constexpr u8 FLAG_SWALLOW_B = 0x04;
constexpr u8 FLAG_SWALLOW_START = 0x08;
constexpr u8 FLAG_BACK_A = 0x10;

constexpr int SKIP_BAR_FRAMES = 15;
constexpr int SOLO_LEAVE_FRAMES = 30;
constexpr int STEER_LIMIT_FRAMES = 10 * 60;
constexpr int STEER_SETTLE_FRAMES = 20;
constexpr int COSTUME_LIMIT_FRAMES = 3 * 60;

using QueueApp::NO_CHARACTER;
constexpr int RANDOM_CHARACTER = 0x29;

struct CssPort
{
  bool readable = false;
  bool human = false;
  int character = NO_CHARACTER;
  bool placed = false;
  int costume = 0;
  bool name_list = false;
  u32 hand_target = 0;
  u32 hand_button = 0;
  float hand_x = 0;
  float hand_y = 0;
  bool operator==(const CssPort&) const = default;
};

struct View
{
  bool queue2 = false;
  bool solo = false;
  bool ranked = false;
  bool css = false;
  bool game1 = true;
  bool order_done = true;
  bool order_locked_in = false;
  u8 order_locked = 0;
  u8 plugged = 0;
  std::array<CssPort, 2> ports{};
  std::array<u16, 2> raw{};
  std::array<u16, 2> raw_prev{};
  float hand2_prev_x = 0;
  float hand2_prev_y = 0;
  std::optional<Identity> pick2;
  u16 ready_frames = 0;
  u16 skip_frames = 0;
  bool operator==(const View&) const = default;
};

struct State
{
  u8 ready = 0;
  u8 flags = 0;
  u8 timeout_who = 0;
  u8 locked = 0;
  u32 timer_start = 0;
  u16 timer = 0;
  u16 steer = 0;
  u32 steer_start = 0;
  u32 costume_start = 0;
  bool pick_set = false;
  u8 pick_character = 0;
  u8 pick_costume = 0;
  float pick_x = 0;
  float pick_y = 0;
  bool operator==(const State&) const = default;
};

bool ReadyPhase(const View& view);
bool OrderLockedIn(const View& view);

bool Timed(const View& view);

bool MaySkip(const View& view, const State& state);

State Advance(const View& view, const State& state, int frame);

std::string_view UnreadyReason(const View& view, const State& before, const State& after);

Rollback::InputGate::Masks Gate(const View& view, const State& state);

struct SkipHold
{
  int frames = 0;
  bool sent = false;
  bool Step(bool may_skip, bool held, int skip_frames);
  float Progress(int skip_frames) const;
};

constexpr int FIXED_BITS = 16;
constexpr s64 FIXED_ONE = s64{1} << FIXED_BITS;
constexpr s64 FIXED_LIMIT = s64{4096} << FIXED_BITS;
std::optional<s64> ToFixed(float value);
u64 IntSqrt(u64 n);

std::pair<u8, u8> SteerToward(float x, float y, float tx, float ty);
bool HandStill(float x, float y, float prev_x, float prev_y);
}
