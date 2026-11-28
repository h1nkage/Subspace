// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
class System;
}

namespace Rollback::Diag
{
bool KeepThrottle();
bool SnapTime();

void OnFirstPassBoundary(Core::System& system, int frame);
void LogSummary();

void CopyRam(std::vector<u8>* dst, const u8* src, std::size_t size);
void AddSnapshotSave(double state_ms, double mem1_ms, double mem2_ms);
void AddCowSave(double state_ms, double protect_ms);
void AddRingLoad(double ms, std::size_t changed_blocks);
void AddPortSave(double ms);
void AddPortLoad(double ms);

extern const bool g_jit_code_log;
void JitCodeSetFrame(int frame);
void JitCodeCompiled(u32 block_start, const std::vector<std::pair<u32, u32>>& words);
void JitCodeInvalidated(u32 address, u32 length, bool forced);
void JitCodeCleared(const char* why);
void JitCodeNote(const std::string& line);
void JitCodeCensus(const Core::CPUThreadGuard& guard, const char* why);
}
