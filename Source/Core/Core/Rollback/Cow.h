// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "Common/CommonTypes.h"

namespace Core
{
class System;
}

namespace Rollback::Cow
{
struct Area
{
  u8* alias;
  u32 physical_address;
  u32 size;
};

struct View
{
  u8* base;
  u32 physical_address;
  u32 size;
  bool writeable;
};

std::size_t PageSize();

bool Arm(const void* owner, const std::vector<Area>& areas, const std::vector<View>& views);
bool ArmForSystem(Core::System& system, const void* owner);
void Disarm(const void* owner);
bool IsArmedFor(const void* owner);

u64 Snapshot();
bool Has(u64 id);
bool Restore(u64 id, const std::function<void(u32 physical_address, u32 length)>& changed);
void Drop(u64 id);
std::optional<u64> Checksum(u64 id);

bool HandleFault(uintptr_t address);
void PrepareHostWrite(const void* ptr, std::size_t size);

void OnMappingsChanged(Core::System& system);
void StopTracking();

struct Counters
{
  u64 faults = 0;
  u64 pages_recorded = 0;
  u64 pages_copied = 0;
  u64 pages_reprotected = 0;
  u64 remaps = 0;
};
Counters GetCounters();
}
