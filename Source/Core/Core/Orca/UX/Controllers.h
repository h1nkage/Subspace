// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "InputCommon/GCPadStatus.h"

namespace picojson
{
class value;
}

namespace Orca::UX
{
struct AdapterPort
{
  int port = 0;
  int seat = 0;
  bool connected = false;
  bool wireless = false;
  u16 buttons = 0;
  std::array<u8, 4> axes{128, 128, 128, 128};
  std::array<u8, 2> triggers{0, 0};
  std::array<u8, 6> origin{128, 128, 128, 128, 0, 0};
};

struct StandardPad
{
  int index = 0;
  std::optional<int> seat;
  std::array<float, 4> axes{};
  std::array<float, 17> buttons{};
  std::array<bool, 17> pressed{};
};

struct Snapshot
{
  std::string session;
  u64 sequence = 0;
  double sent_at = 0;
  double received_at = 0;
  bool owned = false;
  bool suspended = false;
  std::vector<AdapterPort> ports;
  std::vector<StandardPad> pads;
};

std::optional<Snapshot> ParseSnapshotEvent(std::string_view json);
std::optional<Snapshot> ParseSnapshotEvent(const picojson::value& event);
bool SafeJson(std::string_view text);

constexpr double STALE_MS = 250;

GCPadStatus NeutralPad();

GCPadStatus MapAdapterPort(const AdapterPort& port);

GCPadStatus MapStandardPad(const StandardPad& pad);

enum class PadSource
{
  Adapter,
  Gamepad,
};

struct LocalChoice
{
  PadSource source;
  int index;
  GCPadStatus pad;
  bool neutral;
};

std::optional<LocalChoice> ChooseLocalPad(const Snapshot& snapshot, double age_ms);
constexpr int MAX_LOCAL = 2;
std::array<std::optional<LocalChoice>, MAX_LOCAL> ChooseLocalPads(const Snapshot& snapshot,
                                                               double age_ms, int n);
}
