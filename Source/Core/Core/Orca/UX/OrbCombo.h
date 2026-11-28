// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>

#include "Common/CommonTypes.h"
#include "Core/Orca/UX/OverlayKey.h"
#include "InputCommon/GCPadStatus.h"

namespace Orca::UX
{
class OrbCombo
{
public:
  static constexpr u16 START = PAD_BUTTON_START;
  static constexpr u16 UP = PAD_BUTTON_UP;
  static constexpr u16 DPAD = PAD_BUTTON_UP | PAD_BUTTON_DOWN | PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT;

  struct Step
  {
    u16 buttons = 0;
    bool fired = false;
    bool released = false;
  };
  Step Next(u16 buttons);
  bool Held() const { return m_held; }

private:
  bool m_held = false;
  bool m_start = false;
  bool m_pressing = false;
};

GCPadStatus FilterLocalPad(GCPadStatus pad, bool first_run, bool live,
                           OverlayKey::Ms now = OverlayKey::NowMs());
GCPadStatus FilterCoSeatPad(GCPadStatus pad, OrbCombo& combo, bool first_run, bool live);
void LocalPadWaiting(std::optional<u16> buttons, OverlayKey::Ms now = OverlayKey::NowMs());
void LocalPadStopped();
}
