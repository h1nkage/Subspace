// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstddef>
#include <vector>

namespace VideoCommon
{
class PresentPacer
{
public:
  using Duration = std::chrono::nanoseconds;

  struct Tuning
  {
    std::size_t window = 180;
    int quantile_permille = 980;
    Duration rise_per_frame = std::chrono::microseconds(500);
    Duration fall_per_frame = std::chrono::microseconds(200);
    Duration earliest = std::chrono::milliseconds(-34);
    Duration latest = Duration::zero();
  };

  PresentPacer();
  explicit PresentPacer(const Tuning& tuning);

  Duration Next(Duration arrival);

  Duration Offset() const { return m_offset; }
  const Tuning& GetTuning() const { return m_tuning; }

private:
  Tuning m_tuning;
  std::vector<Duration> m_arrivals;
  std::size_t m_next = 0;
  std::vector<Duration> m_scratch;
  Duration m_offset{};
};
}
