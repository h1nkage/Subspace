// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace VideoCommon
{
struct BroadcastPresentation
{
  bool capture;
  bool skip_window;
};

constexpr BroadcastPresentation ChooseBroadcastPresentation(bool active, bool has_xfb,
                                                            bool visible, int width, int height,
                                                            bool window_requested = true)
{
  return {active && has_xfb,
          !window_requested || (active && (!visible || width <= 0 || height <= 0))};
}
}
