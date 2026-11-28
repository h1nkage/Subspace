// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"

struct JitBlock;
namespace Core
{
class CPUThreadGuard;
}

namespace Orca::JitWarm
{
struct Block
{
  u32 effective = 0;
  u32 physical = 0;
  u32 flags = 0;
  u64 hash = 0;
  u16 when = 0;
  u8 age = 0;
  std::vector<std::pair<u32, u32>> ranges;
};

std::optional<u64> HashRanges(std::span<const u8> mem1, std::span<const u8> mem2,
                              const std::vector<std::pair<u32, u32>>& ranges);

constexpr std::size_t MAX_FILE_BYTES = std::size_t{16} << 20;
std::vector<u8> Encode(const std::vector<Block>& blocks);
std::optional<std::vector<Block>> Decode(std::span<const u8> bytes);

constexpr int MAX_AGE = 24;
constexpr std::size_t MAX_BLOCKS = 200'000;
std::vector<Block> Merge(std::vector<Block> known, const std::vector<bool>& seen,
                         std::vector<Block> learned);

std::size_t PacedTarget(std::span<const std::pair<int, std::size_t>> due, std::size_t cursor,
                        int frame, int lead);

void OnBlockFinalized(const JitBlock& block);

void OnBoundary(const Core::CPUThreadGuard& guard);

bool ScenesReadable();
std::optional<bool> SceneIsMatch(const Core::CPUThreadGuard& guard);

struct Scene
{
  u32 object = 0;
  u32 name = 0;
  bool operator==(const Scene&) const = default;
};
std::optional<Scene> CurrentScene(const Core::CPUThreadGuard& guard);
std::string SceneName(const Core::CPUThreadGuard& guard, u32 name);
}
