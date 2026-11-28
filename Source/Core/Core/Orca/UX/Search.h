// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <string>
#include <utility>

namespace Orca::UX
{
namespace Search
{
enum class State
{
  None,
  Searching,
  Found,
};
void Begin(bool ranked, std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
void SetWaiting(bool waiting,
                std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
std::string WaitingLine(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
void Matched(bool hosting);
void End();
State Current();
bool Ranked();
void JoinStarted();
bool Joining();
void JoinOver();
std::pair<std::string, std::string> Lines();
std::pair<std::string, std::string> JoiningLines(const std::string& host, bool ranked);
}
}
