// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace Orca::BootThrottle
{
void Begin();
void Finish(bool pacing_owned_elsewhere);
void FinishAtFirstFrame();
}
