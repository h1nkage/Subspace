// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <deque>
#include <optional>

namespace Orca::Net
{
using RoomClock = std::chrono::steady_clock;
inline constexpr auto ROOM_HOLD = std::chrono::seconds(90);
inline constexpr auto ROOM_VERDICT_GRACE = std::chrono::seconds(10);
inline constexpr auto PEER_SILENCE_LIMIT = std::chrono::seconds(10);
inline constexpr auto DIRECT_INPUT_WINDOW = std::chrono::milliseconds(500);

inline bool DirectInputLive(std::optional<RoomClock::time_point> input, RoomClock::time_point now)
{
  return input && now - *input < DIRECT_INPUT_WINDOW;
}

enum class RoomLoss
{
  None,
  InputSilent,
  HoldExpired,
};

struct PeerRoomPresence
{
  std::optional<RoomClock::time_point> away_since;
  bool suspended = false;

  void Away(RoomClock::time_point now)
  {
    if (!away_since)
      away_since = now;
  }

  std::optional<RoomClock::time_point> Deadline() const
  {
    return away_since ? std::optional(*away_since + ROOM_HOLD) : std::nullopt;
  }

  RoomLoss Check(RoomClock::time_point now,
                 std::optional<RoomClock::time_point> last_direct_input) const
  {
    if (!away_since)
      return RoomLoss::None;
    if (now >= *Deadline())
      return RoomLoss::HoldExpired;
    (void)last_direct_input;
    return RoomLoss::None;
  }

  bool Back(RoomClock::time_point now, bool direct_live)
  {
    const bool keep = !suspended && (away_since ? now < *Deadline() : direct_live);
    away_since.reset();
    suspended = false;
    return keep;
  }
};

inline const char* FinalRoomClose(unsigned code)
{
  switch (code)
  {
  case 1008:
    return "message-limit";
  case 4000:
    return "replaced";
  case 4001:
    return "kicked";
  case 4003:
    return "locked";
  case 4004:
    return "match-over";
  default: return nullptr;
  }
}

struct RoomReconnectBudget
{
  std::deque<RoomClock::time_point> connections;
  bool Available(RoomClock::time_point now)
  {
    while (!connections.empty() && now - connections.front() >= std::chrono::minutes(5))
      connections.pop_front();
    return connections.size() < 6;
  }
  bool Take(RoomClock::time_point now)
  {
    if (!Available(now))
      return false;
    connections.push_back(now);
    return true;
  }
};

struct RoomRejoinFlaps
{
  static constexpr auto SHORT_LIFE = std::chrono::seconds(60);
  static constexpr auto WINDOW = std::chrono::minutes(5);
  static constexpr unsigned LIMIT = 4;
  std::optional<RoomClock::time_point> welcome;
  std::deque<RoomClock::time_point> losses;

  void Rejoined(RoomClock::time_point now) { welcome = now; }

  std::size_t Count(RoomClock::time_point now) const
  {
    return std::count_if(losses.begin(), losses.end(),
                        [now](const auto loss) { return now - loss < WINDOW; });
  }

  bool Lost(RoomClock::time_point now)
  {
    while (!losses.empty() && now - losses.front() >= WINDOW)
      losses.pop_front();
    const bool short_lived = welcome && now - *welcome < SHORT_LIFE;
    welcome.reset();
    if (short_lived)
      losses.push_back(now);
    return short_lived && losses.size() >= LIMIT;
  }
};

struct RoomReconnect
{
  std::optional<RoomClock::time_point> since;
  std::optional<RoomClock::time_point> next_attempt;
  unsigned attempts = 0;
  std::chrono::milliseconds backoff{500};

  static std::chrono::milliseconds Jitter(std::chrono::milliseconds delay, unsigned sample)
  {
    return delay * (70 + sample % 61) / 100;
  }

  bool Start(RoomClock::time_point now, unsigned close_code, unsigned sample = 30)
  {
    if (FinalRoomClose(close_code))
      return false;
    if (!since)
    {
      since = now;
      next_attempt = now + Jitter(std::chrono::milliseconds(500), sample);
      attempts = 0;
      backoff = std::chrono::milliseconds(500);
    }
    return true;
  }

  RoomLoss Check(RoomClock::time_point now,
                 std::optional<RoomClock::time_point> deadline = std::nullopt) const
  {
    if (since && now >= deadline.value_or(*since + ROOM_HOLD))
      return RoomLoss::HoldExpired;
    return RoomLoss::None;
  }

  bool Attempt(RoomClock::time_point now,
               std::optional<RoomClock::time_point> deadline = std::nullopt)
  {
    if (!since || !next_attempt || now < *next_attempt || Check(now, deadline) != RoomLoss::None)
      return false;
    next_attempt.reset();
    ++attempts;
    return true;
  }

  std::chrono::milliseconds UpgradeCap(
      RoomClock::time_point now,
      std::optional<RoomClock::time_point> deadline = std::nullopt) const
  {
    const auto cap = std::chrono::milliseconds(attempts <= 1 ? 5000 : attempts == 2 ? 8000 : 10000);
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline.value_or(since.value_or(now) + ROOM_HOLD) - now);
    return std::clamp(remaining, std::chrono::milliseconds::zero(), cap);
  }

  void Retry(RoomClock::time_point now, unsigned sample = 30,
             std::optional<RoomClock::time_point> deadline = std::nullopt)
  {
    if (!since)
      return;
    next_attempt = std::min(now + Jitter(backoff, sample), deadline.value_or(*since + ROOM_HOLD));
    backoff = std::min(backoff * 2, std::chrono::milliseconds(4000));
  }
};

struct HostRoomWait
{
  std::optional<RoomClock::time_point> until;

  bool Start(RoomClock::time_point now, std::optional<RoomClock::time_point> deadline)
  {
    if (!deadline || now >= *deadline)
      return false;
    until = deadline;
    return true;
  }

  bool Expired(RoomClock::time_point now) const { return until && now >= *until; }
  void Back() { until.reset(); }
};
}
