// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

namespace Orca::Status
{
void State(std::string_view state);
void Event(std::string_view state);
void Error(std::string_view code, std::string_view sentence);
void Menu(std::string_view event);
void Result(std::string_view json);
void Line(std::string_view line);
void Report(std::string_view code, std::string_view sentence);

inline constexpr const char* CAPS =
    "join leave stats pause delay perf direct host chat orb gfx";
inline constexpr const char* MUSIC_CAP = "music";
inline constexpr const char* RESULTS_CAP = "results";
inline constexpr const char* LOCKS_CAP = "locks";
inline constexpr const char* QUEUE2_CAP = "queue2";
inline constexpr const char* PICK_CAPS = "pick-casual pick-ranked";
inline constexpr const char* TALK_CAP = "talk";
inline constexpr const char* RUMBLE_CAP = "rumble";
inline constexpr const char* REPLAY_CAP = "replay";
inline constexpr const char* RECORD_CAP = "record";
inline constexpr const char* HOSTUI_CAP = "hostui";
inline constexpr const char* SHORTCUTS_CAP = "shortcuts";
void SetShortcutsAvailable(bool available);
std::string OfferedCaps();
void PrintCaps();
inline constexpr std::chrono::seconds CAPS_SETTLE_LIMIT{20};
bool CapsSettled();
void UpdateCaps();
void WithdrawOnline();
void ResetOnline();
void SetAppCaps(std::string_view list);
bool Cap(std::string_view name);
void Stats(std::string_view json);
void Shaders(std::size_t compiled, std::size_t total);
int ExitCode();
void GameStarted();
void SetFirstFrameListener(void (*listener)());
void SetOutput(std::FILE* out);
void SetErrorListener(void (*listener)(std::string_view code, std::string_view sentence));
void Stopping();
bool IsStopping();
int Finish();
}
