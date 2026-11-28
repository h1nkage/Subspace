// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>

namespace DiscIO
{
class VolumeDisc;
}

namespace Orca
{
inline constexpr std::string_view APP_GAME_ID = "RSBE01";
inline constexpr std::string_view APP_DISC_PROMPT =
    "Choose your own Super Smash Bros. Brawl disc image (USA).";

struct DiscCheck
{
  bool ok = false;
  std::string code;
  std::string sentence;
};

DiscCheck CheckDisc(const std::string& path, std::string_view game_id);

bool AliasRevision(DiscIO::VolumeDisc& volume, std::string* reason = nullptr);
std::string DiscRevisionRefusal(std::string prefix, std::string_view reason);

bool SessionDiscReady(const DiscIO::VolumeDisc& volume);

DiscCheck CheckBootFile(const std::string& path);
}
