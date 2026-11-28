// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "DiscIO/VolumeVerifier.h"

namespace Orca::DiscVerify
{
using Problem = DiscIO::VolumeVerifier::Problem;

inline constexpr int EXIT_OK = 0;
inline constexpr int EXIT_UNREADABLE = 1;
inline constexpr int EXIT_DAMAGED = 2;

std::optional<std::string> TakeArgument(std::vector<char*>* argv, std::string* error);

bool Counts(const Problem& problem);
std::size_t CountDamage(std::span<const Problem> problems);
int ExitCode(std::size_t damage);

std::string ProgressLine(u64 done, u64 total);
std::string ProblemLine(const Problem& problem);
std::string DamageLine(const Problem& problem);
std::string VerdictLine(std::size_t damage);
std::string UnreadableLine(std::string_view path);
std::vector<std::string> ResultLines(std::span<const Problem> problems);

class ProgressPacer
{
public:
  bool Due(std::chrono::steady_clock::time_point now);

private:
  std::optional<std::chrono::steady_clock::time_point> m_last;
};

int Run(const std::string& path, std::FILE* out);

inline constexpr std::string_view NAND_PREFIX = "orca-verify-nand-";
std::string MakeNand();
std::size_t ClearStaleNands(const std::string& dir, const std::function<bool(u64 pid)>& alive);
bool ProcessAlive(u64 pid);
}
