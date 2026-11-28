// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Events.h"

namespace Core
{
class CPUThreadGuard;
}

namespace Orca::UX
{
class GuestMemory;
}

namespace Orca::UX::NativeText
{
constexpr u32 MESSAGE_BUFFER_VTABLE = 0x804540E0;
constexpr u32 BUFFER_CAPACITY = 0x28;
constexpr u32 BUFFER_LENGTH = 0x2C;
constexpr u32 BUFFER_DATA = 0x30;
constexpr u8 END = 0x01;

struct Box
{
  u32 object = 0;
  u32 buffer = 0;
  u32 capacity = 0;
  u32 length = 0;
  u32 words = 0;
  u32 words_end = 0;
};
std::optional<Box> ReadBox(const GuestMemory& memory, u32 object);
std::string Words(const GuestMemory& memory, const Box& box);
int SetWords(GuestMemory& memory, const Box& box, std::string_view text);

std::string Ascii(std::string_view text, size_t max = 96);
std::string PlateName(std::string_view username);
std::string Clock(int frames);

struct CssWords
{
  bool on_css = false;
  std::array<std::string, 4> plates;
  std::string rules;
};
CssWords Wanted(const GuestMemory& memory, const std::vector<Events::PortInfo>& ports,
                std::optional<bool> band_clock = std::nullopt);

struct CssBoxes
{
  std::array<std::optional<Box>, 4> plates;
  std::optional<Box> rules;
};
CssBoxes FindCssBoxes(const GuestMemory& memory);

int Apply(GuestMemory& memory, const std::vector<Events::PortInfo>& ports, u8* shown = nullptr,
          bool rules_bar = true, std::optional<bool> band_clock = std::nullopt);
constexpr u8 SHOWN_RULES = 0x10;

void Frame(const Core::CPUThreadGuard& guard, bool resimulating,
           const std::vector<Events::PortInfo>& ports, std::optional<bool> band_clock = std::nullopt);
u8 Shown();
}
