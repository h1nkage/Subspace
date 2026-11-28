// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

namespace Orca::UX
{
enum class OnlinePick
{
  Casual,
  Ranked,
  Friends,
};

std::string_view MenuEventText(OnlinePick pick, bool queue = false);

void AnnounceOnlinePick(OnlinePick pick, bool arm = true);

bool PickSearchable(OnlinePick pick);

void AnnounceMenuCancel();
}
