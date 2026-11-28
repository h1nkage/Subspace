// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <string_view>

#include "Common/CommonTypes.h"

namespace Orca::GpuLost
{
inline constexpr std::string_view CODE = "gpu_lost";
inline constexpr std::string_view SENTENCE =
    "Your graphics driver stopped responding. Try the Performance preset or a lower resolution in "
    "Graphics, and update your GPU driver.";

const char* ReasonName(u32 hr);

size_t FormatLine(char* out, size_t size, std::string_view backend, u32 reason,
                  std::string_view where);

bool Report(std::string_view backend, u32 reason, std::string_view where);

bool Lost();

void DeviceCreated(std::string_view backend);
void DeviceClosed(std::string_view backend);

size_t DeviceState(char* out, size_t size);
void SetDeviceState(std::string_view state);
}
