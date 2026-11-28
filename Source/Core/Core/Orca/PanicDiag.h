// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/RangeSet.h"

namespace Core
{
class CPUThreadGuard;
class System;
}

namespace Orca::PanicDiag
{
struct View
{
  std::string name;
  const u8* base;
  u32 physical_address;
  u32 size;
};

extern const std::vector<u32> WORDS;

std::string DescribeWord(u32 physical_address, const std::vector<View>& views);

void DumpOnce(Core::System& system, u32 effective_address, bool write);

struct GuestRam
{
  std::span<const u8> mem1;
  std::span<const u8> mem2;

  static constexpr u32 MEM2_PHYSICAL = 0x10000000;

  std::optional<u32> Physical(u32 address) const;
  std::optional<u32> Word(u32 address) const;
  bool Valid(u32 address) const { return Word(address).has_value(); }
};

struct StackFrame
{
  u32 back_chain;
  std::optional<u32> saved_lr;
};
struct BackChain
{
  std::vector<StackFrame> frames;
  std::string stop;
};
constexpr int MAX_CHAIN_DEPTH = 10;
BackChain WalkBackChain(const GuestRam& ram, u32 sp, int max_depth = MAX_CHAIN_DEPTH);

std::vector<std::string> DescribeBackChain(const BackChain& chain, u32 sp);

std::vector<std::string> DescribeWords(const GuestRam& ram, std::string_view label, u32 address,
                                       int count);

constexpr u32 OS_MODULE_LIST = 0x800030C8;
std::string ModuleOf(const GuestRam& ram, u32 address);
std::string DescribeModules(const GuestRam& ram);

constexpr u32 OS_CURRENT_CONTEXT = 0x800000D4;
constexpr u32 OS_CURRENT_THREAD = 0x800000E4;
std::vector<std::string> DescribeOS(const GuestRam& ram, u32 r1);

u64 CodeHash(const GuestRam& ram, const Common::RangeSet<u32>& physical_ranges);
GuestRam RamOf(Core::System& system);

struct ExceptionNote
{
  u32 vector;
  u32 srr0;
  u32 r1;
  u32 lr;
};
constexpr u32 EXCEPTION_RING = 16;
inline std::array<ExceptionNote, EXCEPTION_RING> g_exceptions{};
inline u32 g_exception_count = 0;
inline void NoteException(u32 vector, u32 srr0, u32 r1, u32 lr)
{
  g_exceptions[g_exception_count++ % EXCEPTION_RING] = {vector, srr0, r1, lr};
}
std::vector<std::string> DescribeExceptions(std::span<const ExceptionNote> ring, u32 count);

void NoteScene(const Core::CPUThreadGuard& guard, int frame);
}
