// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>

namespace Orca::Sandbox
{
enum class NetMode
{
  All,
  Internet,
  InternetLan,
};
NetMode ParseNetMode(std::string_view text, bool* known = nullptr);
NetMode NetModeFromEnv();
std::string_view NetModeName(NetMode mode);
bool LanAllowed(NetMode mode);
bool LoopbackAllowed(NetMode mode);
bool LanAddress(std::string_view address);
bool LoopbackAddress(std::string_view address);
bool AddressAllowed(NetMode mode, std::string_view address);

bool Jailed();
bool PackJailed();

bool AdapterAllowed(bool session);

enum class Os
{
  Windows,
  MacOS,
  Other,
};
Os ThisOs();
bool OsKeyboardAllowed(Os os, bool session, bool stdio);

inline constexpr const char* EMBED_ERROR = "sandbox_embed";
inline constexpr const char* EMBED_SENTENCE =
    "Subspace couldn't open in the app window. Try again.";
inline constexpr const char* DISC_ERROR = "sandbox_disc";
inline constexpr const char* DISC_SENTENCE =
    "Subspace can't read this disc where it is. Copy it into a folder of yours, such as Documents, "
    "and choose the copy.";
inline constexpr const char* DENIED_ERROR = "sandbox_denied";
inline constexpr const char* DENIED_SENTENCE =
    "Subspace isn't allowed into its own folder, so it didn't start. In the desktop app's Help menu, reset "
    "its files, then press Play again.";

bool ReadDenied(const std::string& path);
bool WriteDenied(const std::string& dir);
}
