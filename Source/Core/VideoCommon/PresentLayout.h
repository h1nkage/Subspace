// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>

#include "Common/MathUtil.h"

namespace VideoCommon
{
struct LayoutHint
{
  int win_w = 1;
  int win_h = 1;
  int x = 0;
  int y = 0;
  int w = 1;
  int h = 1;

  constexpr bool operator==(const LayoutHint&) const = default;
};

struct LayoutBox
{
  float x = 0;
  float y = 0;
  float w = 0;
  float h = 0;

  constexpr bool operator==(const LayoutBox&) const = default;
};

constexpr float LAYOUT_SHAPE_TOLERANCE = 0.02f;

std::optional<LayoutBox> LayoutBoxIn(const LayoutHint& hint, int bb_w, int bb_h);

struct LayoutChoice
{
  LayoutBox box;
  std::optional<LayoutHint> hint;
};

LayoutChoice ChooseLayout(const std::optional<LayoutHint>& latest,
                          const std::optional<LayoutHint>& applied, int bb_w, int bb_h);

MathUtil::Rectangle<int> PlaceInBox(const LayoutBox& box, int draw_w, int draw_h);

bool PictureInBackbuffer(const MathUtil::Rectangle<int>& picture, int bb_w, int bb_h);
}
