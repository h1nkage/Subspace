// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "InputCommon/GCPadStatus.h"

namespace Core
{
class CPUThreadGuard;
}

namespace Rollback::InputGate
{
constexpr int PORTS = 4;

struct Mask
{
  u16 buttons = 0;
  bool main_stick = false;
  bool c_stick = false;
  u16 press = 0;
  u16 a_as = 0;
  bool steer = false;
  u8 steer_x = GCPadStatus::MAIN_STICK_CENTER_X;
  u8 steer_y = GCPadStatus::MAIN_STICK_CENTER_Y;
  bool a_centres_stick = false;
  u16 drop_with_lr = 0;

  bool Empty() const
  {
    return buttons == 0 && !main_stick && !c_stick && press == 0 && a_as == 0 && !steer &&
           !a_centres_stick && drop_with_lr == 0;
  }
  bool operator==(const Mask&) const = default;
};
constexpr u16 ALL_BUTTONS = 0x1F7F;
constexpr Mask ALL{ALL_BUTTONS, true, true};

using Masks = std::array<Mask, PORTS>;

constexpr u8 TRIGGER_HELD = 0x40;
constexpr bool TriggerHeld(bool button, u8 analog)
{
  return button || analog >= TRIGGER_HELD;
}

GCPadStatus Apply(const GCPadStatus& pad, const Mask& mask);

using Source = std::function<Masks(const Core::CPUThreadGuard& guard)>;
void SetSource(Source source);

using Latch = std::function<void(const Core::CPUThreadGuard& guard,
                                 const std::array<std::optional<GCPadStatus>, PORTS>& raw)>;
void SetLatch(Latch latch);

void OnBoundary(const Core::CPUThreadGuard& guard);
void Clear();
const Masks& Current();
GCPadStatus GatePad(int port, const GCPadStatus& pad);

struct TestRule
{
  int port = 0;
  Mask mask;
  int every = 0;
};
std::optional<std::vector<TestRule>> ParseTestSpec(std::string_view spec, std::string* error);
Masks TestMasks(const std::vector<TestRule>& rules, u64 memory_hash);
constexpr u32 TEST_HASH_START = 0x80500000;
constexpr u32 TEST_HASH_SIZE = 0x100000;
}
