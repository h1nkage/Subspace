// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "Core/Orca/Engine/Module.h"

namespace Orca
{
struct Profile;
}

namespace Orca::UX
{
constexpr int kCompatVersion = 32;

void SetGameDisc(std::string path);
void Init();
void SelectModule(const Profile* profile);
void Shutdown();
Orca::Engine::Module& Game();
}
