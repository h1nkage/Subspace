// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <picojson.h>

#include "Common/CommonTypes.h"

namespace Orca::Bridge
{
inline constexpr std::string_view PREFIX = "br ";
inline constexpr size_t MAX_INBOUND = 64 * 1024;
inline constexpr size_t MAX_LINE = 128 * 1024;
inline constexpr size_t MAX_OUTBOUND = 32 * 1024;

bool Enabled();
bool Ready();
bool AllowedRequest(std::string_view type);

class LineSplitter
{
public:
  explicit LineSplitter(size_t max) : m_max(max) {}
  std::vector<std::string> Feed(std::string_view bytes);
  u64 Dropped() const { return m_dropped; }
  size_t Pending() const { return m_pending.size(); }

private:
  size_t m_max;
  std::string m_pending;
  bool m_skipping = false;
  u64 m_dropped = 0;
};

struct Inbound
{
  enum class Kind
  {
    Ignored,
    Reply,
    Controllers,
    Push,
  };
  Kind kind = Kind::Ignored;
  u64 re = 0;
  picojson::value body;
};
Inbound Parse(std::string_view line);

std::vector<std::string> Route(std::vector<std::string> lines);
void Close();

void SetWriter(void (*writer)(const std::string& line));

std::optional<picojson::object> Request(picojson::object request, std::chrono::milliseconds timeout,
                                        const std::function<bool()>& cancelled = {});

u64 SetPushHandler(std::function<void(const picojson::object&)> handler);
void ClearPushHandler(u64 token);
u64 SetControllersHandler(std::function<void(const picojson::value&)> handler);
void ClearControllersHandler(u64 token);

struct Stats
{
  u64 replies = 0;
  u64 unmatched = 0;
  u64 controllers = 0;
  u64 coalesced = 0;
  u64 pushes = 0;
  u64 ignored = 0;
  u64 requests = 0;
  u64 unanswered = 0;
};
Stats GetStats();
void ResetForTests();

inline constexpr std::string_view HEARTBEAT_LINE = "orca alive";
inline constexpr std::chrono::milliseconds HEARTBEAT_PERIOD{1000};
class Heartbeat
{
public:
  bool Due(std::chrono::steady_clock::time_point now);

private:
  std::optional<std::chrono::steady_clock::time_point> m_next;
};
}
