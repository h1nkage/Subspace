// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "InputCommon/GCPadStatus.h"

namespace Core
{
class CPUThreadGuard;
}

namespace Orca::Events
{
struct PortInfo
{
  int port = 0;
  std::string name;
  bool remote = false;
  std::vector<u8> controls;
  std::vector<u8> queue;
  bool operator==(const PortInfo&) const = default;
};

void SetOwnControls(std::vector<u8> controls);
std::vector<u8> OwnControls();
void SetOwnQueue(std::vector<u8> queue);
std::vector<u8> OwnQueue();

using PlugInCallback = std::function<void(int frame, const std::vector<PortInfo>& ports)>;
void SetPlugInCallback(PlugInCallback callback);

struct LinkStats
{
  bool online = false;
  int delay = 0;
  double rollbacks_per_second = 0;
  int deepest_rollback = 0;
  int stalls = 0;
  int hitches = 0;
  int peer_hitches = 0;
  int round_trip_ms = -1;
  int transport = 0;
  int link_rtt_ms = -1;
  int lamp_ms = -1;
  std::array<int, 4> seat_lamp_ms{-1, -1, -1, -1};
  int confirmed_frame = -1;
  int stalled_ms = 0;
  int waiting_seat = -1;
};
LinkStats GetLinkStats();

using LocalPadSource = std::function<std::optional<GCPadStatus>(int ordinal, bool for_frame)>;
void SetLocalPadSource(LocalPadSource source);
std::optional<GCPadStatus> LocalPad(bool for_frame = false);
std::optional<GCPadStatus> LocalPad(int ordinal, bool for_frame);

struct InputAge
{
  u64 frames = 0;
  u64 reports = 0;
  u64 changes = 0;
  double age_ms = 0;
};
using InputAgeSource = std::function<InputAge()>;
void SetInputAgeSource(InputAgeSource source);
InputAge GetInputAge();

using FrameCallback =
    std::function<void(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
                       const std::vector<PortInfo>& ports, bool alone)>;
void SetFrameCallback(FrameCallback callback);

void NoteResync();
u64 Resyncs();

using StallCallback = std::function<void(int stalled_ms)>;
void SetStallCallback(StallCallback callback);

using BoundaryCallback = std::function<void(bool shown, double game_held_ms, int game_held_frames)>;
void SetBoundaryCallback(BoundaryCallback callback);

double GameHeldMs(double emulated_ms, double wall_ms, double frame_ms);
int GameHeldFrames(double game_held_ms, double frame_ms, double& carry);

void SetUXCompatVersion(int version);
int UXCompatVersion();

void PublishLinkStats(const LinkStats& stats);
void NotifyPlugIn(int frame, const std::vector<PortInfo>& ports);
void NotifyFrame(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
                 const std::vector<PortInfo>& ports, bool alone);
void NotifyStall(int stalled_ms);
void NotifyBoundary(bool shown, double game_held_ms, int game_held_frames);
}
