// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>

// Player-visible names say Subspace. Upstream attribution and licence text are unchanged. The
// code, its files and its protocols keep the codename Orca (ORCA.md).
namespace Orca
{
inline constexpr std::string_view PRODUCT_NAME = "Subspace";

std::string WindowTitle();

std::string BrandWords(std::string_view text);

std::string BrandText(std::string_view text);
}
