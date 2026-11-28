// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <string_view>

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
}

namespace Orca::UX
{
class GuestMemory;

struct MenuTextEdit
{
  u32 file;
  u32 message;
  std::string_view original;
  std::string_view text;
};

std::span<const MenuTextEdit> MenuTextEdits();

u32 FindMenuArchive(const GuestMemory& memory);
u32 FindArchiveFile(const GuestMemory& memory, u32 archive, u32 file);
constexpr u32 DESCRIPTION_FILE = 5;

struct MenuDescription
{
  u32 buffer = 0;
  u32 length = 0;
  u32 message = 0;
};
MenuDescription FindMenuDescription(const GuestMemory& memory, u32 messages);
int ApplyMenuDescription(GuestMemory& memory, u32 messages, std::span<const MenuTextEdit> edits);
int ApplyMenuText(GuestMemory& memory, std::span<const MenuTextEdit> edits,
                  int* in_box = nullptr);

void WriteMenuText(const Core::CPUThreadGuard& guard);
}
