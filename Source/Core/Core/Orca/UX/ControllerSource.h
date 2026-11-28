// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/UX/Controllers.h"
#include "InputCommon/GCPadStatus.h"

namespace Orca::UX
{
std::optional<GCPadStatus> LocalPad(bool for_frame = false);
std::optional<GCPadStatus> LocalPad(int ordinal, bool for_frame);
u32 OwnAdapterPorts();
std::optional<GCPadStatus> OwnAdapterPad();

void SetLocalPadCount(int n);

struct LocalDevice
{
  PadSource source;
  int index;
};
std::optional<LocalDevice> LastLocalDevice();
std::optional<LocalDevice> LastLocalDevice(int ordinal);

void StartControllerStreamFromEnvironment();
void StartControllerStreamFromStdio();

void StartControllerStream(const std::string& bridge, const std::string& token);
void StopControllerStream();
std::optional<GCPadStatus> LatestPad();
std::shared_ptr<const Snapshot> LatestSnapshot(double* age_ms);

struct ControllerTiming
{
  u64 snapshots = 0;
  u64 refused = 0;
  u64 samples = 0;
  double p50_ms = 0, p95_ms = 0, p99_ms = 0, max_ms = 0;
  bool attached = false;
  Orca::Events::InputAge input;
  u64 age_samples = 0;
  double age_p50_ms = 0, age_p95_ms = 0, age_p99_ms = 0, age_max_ms = 0;
  double helper_p50_ms = 0, cache_p50_ms = 0;
  u64 hop_samples = 0;
  double hop_p50_ms = 0, hop_p95_ms = 0;
};
ControllerTiming GetControllerTiming();
void ResetControllerTiming();
Orca::Events::InputAge GetInputAge();
}
