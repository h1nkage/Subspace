// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

struct IDXGIAdapter;

namespace DX11::Diagnostics
{
void Initialize(IDXGIAdapter* adapter);
void Shutdown();
void Poll();
void Presented();
void Lost(u32 removed_reason);
}
