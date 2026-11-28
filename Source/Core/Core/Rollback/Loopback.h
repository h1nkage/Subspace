// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Session.h"

namespace Rollback::Loopback
{
struct RecordedFrame
{
  Orca::Net::Pads pads{};
  u64 checksum = 0;
};

std::string FormatRecordedFrame(int frame, const Orca::Net::Pads& pads, u64 checksum);
std::vector<RecordedFrame> LoadRecording(const std::string& path);

class VirtualRemote final : public Orca::Net::Transport
{
public:
  VirtualRemote(const std::vector<RecordedFrame>& recording, std::vector<int> seats, int local_seat,
                int lag, int jitter, int input_delay, int checksum_every,
                std::vector<int> local_co_seats = {});

  void Tick();
  void TickWhileWaiting();
  void SilenceFrom(int frame) { m_silent_from = frame; }
  void StartSpikes(std::chrono::milliseconds period, std::chrono::milliseconds length,
                   int max_lead);
  int Spikes() const { return m_spikes; }
  bool Exhausted() const;

  void Send(const Orca::Net::Packet& packet) override;
  std::vector<Orca::Net::Packet> Receive() override;
  bool Connected() const override { return true; }

private:
  struct Seat
  {
    int seat = 0;
    int delivered = -1;
    int last_checksum = 0;
  };
  int Lag(int seat) const;

  const std::vector<RecordedFrame>& m_recording;
  std::vector<Seat> m_seats;
  int m_local_seat;
  std::vector<int> m_local_co_seats;
  int m_lag;
  int m_jitter;
  int m_input_delay;
  int m_checksum_every;
  int m_ticks = 0;
  int m_silent_from = -1;
  int m_local_newest = -1;
  std::chrono::milliseconds m_spike_period{0};
  std::chrono::milliseconds m_spike_length{0};
  std::chrono::steady_clock::time_point m_spike_start;
  std::chrono::steady_clock::time_point m_last_tick;
  int m_max_lead = 0;
  int m_lead = 0;
  bool m_spiking = false;
  int m_spikes = 0;
};
}
