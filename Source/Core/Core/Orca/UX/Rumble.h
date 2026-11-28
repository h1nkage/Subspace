// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
}

namespace Orca::UX::Rumble
{
enum class Device : u8
{
  None,
  Adapter,
  Pad,
};

struct Target
{
  Device device = Device::None;
  int index = 0;
  bool operator==(const Target&) const = default;
};

struct State
{
  bool on = false;
  int port = 0;
  Target target;
  bool operator==(const State&) const = default;
};

constexpr int OFF_FRAMES = 3;
constexpr int MIN_GAP_FRAMES = 6;
constexpr int RESEND_FRAMES = 30;
constexpr int STALL_HALT_MS = 100;

bool MotorOn(u32 channel_out);

class Tracker
{
public:
  std::optional<State> Boundary(bool motor, bool live, int port, Target target);
  std::optional<State> Halt();
  const State& Sent() const { return m_sent; }

private:
  State m_sent;
  int m_off_run = 0;
  int m_since_on = RESEND_FRAMES;
};

std::string FormatLine(const State& state);

void Frame(const Core::CPUThreadGuard& guard, int local_port);
void Stall(int stalled_ms);
void Halt();
}
