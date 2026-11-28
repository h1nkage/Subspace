// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Core
{
class CPUThreadGuard;
}

namespace Orca::UX
{
void ProbeFrame(const Core::CPUThreadGuard& guard, int frame);
bool TestKnobsAllowed();
}
