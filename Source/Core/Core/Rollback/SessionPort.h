// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <functional>
#include <optional>
#include <utility>

#include "Common/CommonTypes.h"
#include "Core/CoreTiming.h"
#include "Core/Orca/Session/Session.h"
#include "Core/Rollback/Rollback.h"
#include "InputCommon/GCPadStatus.h"

namespace Core
{
class System;
}

namespace Rollback
{
namespace OnlineMatch
{
class StallBudget;
}
class RingPort final : public Orca::Net::Game
{
public:
  RingPort(Core::System& system, int max_rollback, bool unthrottled = false);
  ~RingPort() override;
  RingPort(const RingPort&) = delete;
  RingPort& operator=(const RingPort&) = delete;

  bool Save(int frame) override;
  bool Load(int frame) override;
  void SetPads(int frame, const Orca::Net::Pads& pads) override;
  void SetResimulating(bool resimulating) override;
  std::optional<u64> SnapshotChecksum(int frame) override;
  void ResetPacing() override;
  double SaveMs() const override { return m_save_ms; }

  GCPadStatus Pad(int port) const { return m_pads[port]; }

  void SetCatchingUp(bool catching_up);
  bool CatchingUp() const { return m_catching_up; }
  bool Resimulating() const { return m_resimulating; }
  bool LoadImage(MachineImage image, int frame);

  static RingPort* Active();

private:
  Core::System& m_system;
  SnapshotRing m_ring;
  double m_save_ms = 0;
  std::array<GCPadStatus, Orca::Net::MAX_SEATS> m_pads;
  bool m_unthrottled;
  bool m_resimulating = false;
  bool m_catching_up = false;
  int m_saved_since_run = -1;
  std::optional<std::pair<s64, TimePoint>> m_reference_before_load;
};

Orca::Net::Step StepSession(Orca::Net::Session& session, Orca::Net::Game& game, int completed_frame,
                            const std::function<Orca::Net::Pad()>& sample_local,
                            const std::function<bool()>& stopping,
                            std::chrono::microseconds stall_wait,
                            std::chrono::milliseconds give_up_after = {}, bool* gave_up = nullptr,
                            std::chrono::nanoseconds* waited = nullptr,
                            const std::function<bool(std::chrono::steady_clock::time_point)>&
                                hold_silence = {},
                            OnlineMatch::StallBudget* stall_budget = nullptr,
                            const std::function<bool(std::chrono::steady_clock::time_point)>&
                                room_recovering = {});
Orca::Net::Step StepSession(Orca::Net::Session& session, Orca::Net::Game& game, int completed_frame,
                            const std::function<Orca::Net::Pad(int)>& sample_local,
                            const std::function<bool()>& stopping,
                            std::chrono::microseconds stall_wait,
                            std::chrono::milliseconds give_up_after = {}, bool* gave_up = nullptr,
                            std::chrono::nanoseconds* waited = nullptr,
                            const std::function<bool(std::chrono::steady_clock::time_point)>&
                                hold_silence = {},
                            OnlineMatch::StallBudget* stall_budget = nullptr,
                            const std::function<bool(std::chrono::steady_clock::time_point)>&
                                room_recovering = {});
}
