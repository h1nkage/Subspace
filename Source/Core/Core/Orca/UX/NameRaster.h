// Copyright 2026 Subspace
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

namespace Orca::UX::NameRaster
{
struct Bitmap
{
  u32 width = 0;
  u32 height = 0;
  float left = 0;
  float top = 0;
  std::vector<u8> rgba;
};

std::optional<Bitmap> Outlined(const std::vector<u8>& ttf, float size, std::string_view text,
                               float outline);
}
