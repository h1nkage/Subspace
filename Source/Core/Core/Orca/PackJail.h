// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Orca::Sandbox
{
struct PackJailDirectory
{
  unsigned long long device;
  long fs_type;
};

struct PackJailHomeEntries
{
  bool symlink = false;
  std::vector<std::string> names;
};

struct PackJailProbes
{
  std::function<std::optional<long>()> root_type;
  std::function<std::optional<std::string>()> passwd_home;
  std::function<std::optional<PackJailHomeEntries>(const std::string&)> home_entries;
  std::function<std::optional<unsigned long long>()> root_device;
  std::function<std::optional<PackJailDirectory>(const std::string&)> directory;
  std::function<std::optional<bool>(const std::string&)> exists;
  std::function<std::optional<bool>(int, bool)> connect_x;
  std::optional<std::string> home;
  std::optional<std::string> display;
};

struct PackJailResult
{
  bool jailed = false;
  bool passwd_present = false;
  std::string failed;
  std::string pid1;
  std::vector<std::string> interfaces;
  std::string Status() const;
};

PackJailResult CheckLinuxPackJail(const std::string& root, const PackJailProbes& probes, bool online);
#ifdef __linux__
PackJailProbes LinuxPackJailProbes();
#endif
}
