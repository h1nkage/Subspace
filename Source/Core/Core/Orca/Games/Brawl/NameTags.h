// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Engine/GuestMemory.h"
#include "Core/Orca/Session/Events.h"

namespace Core
{
class CPUThreadGuard;
}

namespace Orca::UX
{
constexpr int TAG_CHARS = 5;
constexpr int TAG_SLOTS = 120;

std::u16string BrawlTag(std::string_view username, bool view_names = false);

constexpr u32 CONTROLS_LAYOUT_SIZE = 0x2D;
constexpr u32 CONTROLS_PROFILE_SIZE = 1 + CONTROLS_LAYOUT_SIZE;

int ApplyNameTags(GuestMemory& memory, const std::vector<Events::PortInfo>& ports,
                  bool view_names = false);

std::string ControlsHex(const std::vector<u8>& profile);
std::optional<std::vector<u8>> ParseControlsHex(std::string_view hex);
bool ControlsValid(const std::vector<u8>& profile);

std::vector<u8> ReadOwnControls(const GuestMemory& memory, int port, std::string_view own_name,
                                std::u16string* last_worn, std::u16string* read_tag = nullptr,
                                std::u16string* last_edited = nullptr);

class ControlsPageEdits
{
public:
  std::optional<std::u16string> Next(const GuestMemory& memory);

private:
  struct Seen
  {
    std::u16string name;
    std::vector<u8> profile;
  };
  std::vector<Seen> m_seen;
};

class OwnControlsWatch
{
public:
  void Rebase() { m_rebase = true; }
  std::optional<std::vector<u8>> Next(const std::vector<u8>& read, std::u16string_view tag,
                                      const std::vector<u8>& own, bool is_default);

private:
  std::vector<u8> m_last;
  std::u16string m_tag;
  bool m_rebase = false;
};

class OwnControlsReader
{
public:
  void Rebase() { m_rebase = true; }
  std::optional<std::vector<u8>> Read(const GuestMemory& memory,
                                      const std::vector<Events::PortInfo>& ports,
                                      const std::vector<u8>& own, u64 resyncs);

private:
  std::u16string m_last_worn;
  std::u16string m_last_edited;
  ControlsPageEdits m_edits;
  OwnControlsWatch m_watch;
  u64 m_resyncs = 0;
  bool m_rebase = false;
};

std::optional<std::vector<u8>> NameTagsFrame(GuestMemory& memory,
                                             const std::vector<Events::PortInfo>& write_ports,
                                             const std::vector<Events::PortInfo>& ports,
                                             OwnControlsReader* reader, const std::vector<u8>& own,
                                             u64 resyncs);

void NameTagsFrameHook(const Core::CPUThreadGuard& guard,
                       const std::vector<Events::PortInfo>& write_ports,
                       const std::vector<Events::PortInfo>& ports, bool read_own);

void LoadOwnControls();
bool SetOwnControlsFromApp(std::string_view hex);
}
