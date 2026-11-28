// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

#include <dxgi.h>

namespace D3DCommon
{
using RemovedReasonQuery = HRESULT (*)();
using LossDiagnostics = void (*)(HRESULT removed_reason);
void SetDeviceRemovedQuery(const char* backend, RemovedReasonQuery query,
                           LossDiagnostics diagnostics = nullptr);

bool CheckDeviceLost(HRESULT hr, std::string_view what);

bool IsDeviceLost();
}
