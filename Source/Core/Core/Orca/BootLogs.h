// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>

#include "Common/Logging/Log.h"

namespace Orca::BootLogs
{
inline constexpr std::array TYPES = {Common::Log::LogType::OSREPORT, Common::Log::LogType::IOS,
                                     Common::Log::LogType::IOS_SD, Common::Log::LogType::MEMMAP};

std::string EndAtFirstFrame();
}
