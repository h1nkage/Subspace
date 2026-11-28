// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>

#include "Core/Rollback/Rollback.h"

namespace Core
{
class CPUThreadGuard;
}

namespace Rollback::Harness
{
bool Active();
std::optional<GCPadStatus> InputOverride(int port);
bool SetInputScriptForTest(const std::string& path);
void RewindTo(int frame);
void Restart();
void OnFrameBoundary(const Core::CPUThreadGuard& guard);
int ShownFrame();
}
