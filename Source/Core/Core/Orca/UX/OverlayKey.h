// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>
#include <string_view>

#include "Common/CommonTypes.h"

namespace Orca::UX::OverlayKey
{
inline constexpr std::string_view TOGGLE = "orca orb";

using Ms = s64;
Ms NowMs();

inline constexpr Ms PAGE_HOLD_MS = 250;
inline constexpr Ms READ_GAP_MS = 100;

enum class Source
{
  Keys,
  Pad,
};

class Press
{
public:
  explicit Press(Source source) : m_source(source) {}
  std::string_view Down(bool talk, Ms now = NowMs());
  std::string_view Up();
  void Seen(Ms now);
  std::string_view Release(Ms now);
  std::string_view Cancel();
  bool IsDown() const;

private:
  mutable std::mutex m_lock;
  const Source m_source;
  bool m_down = false;
  Ms m_down_at = 0;
  Ms m_seen_at = 0;
};

Press& Keys();
Press& Pad();

using Output = void (*)(std::string_view);
void Emit(std::string_view line, Output output = nullptr);
void Tap(Output output = nullptr);
}
