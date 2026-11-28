// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "Common/Buffer.h"
#include "Common/CommonTypes.h"
#include "InputCommon/GCPadStatus.h"

namespace Core
{
class CPUThreadGuard;
class System;
}

namespace Rollback
{
bool InSnapshotDoState();

bool IsResimulating();
void SetResimulating(bool resimulating);

class SnapshotScope
{
public:
  SnapshotScope();
  ~SnapshotScope();
  SnapshotScope(const SnapshotScope&) = delete;
  SnapshotScope& operator=(const SnapshotScope&) = delete;
};

struct MachineImage
{
  std::vector<u8> state;
  std::vector<u8> mem1;
  std::vector<u8> mem2;
  std::vector<u8> l1_cache;
};

class SnapshotRing
{
public:
  explicit SnapshotRing(std::size_t slots);
  ~SnapshotRing();
  SnapshotRing(const SnapshotRing&) = delete;
  SnapshotRing& operator=(const SnapshotRing&) = delete;

  bool Save(Core::System& system, s64 frame);
  bool Load(Core::System& system, s64 frame);
  bool Has(s64 frame) const;
  void Reset(Core::System& system);
  std::optional<u64> RamChecksum(s64 frame) const;

  std::span<const u8> LastState() const;

  static bool Capture(Core::System& system, MachineImage* image, bool stop_journal);
  bool LoadMachineImage(Core::System& system, MachineImage image, s64 frame);

private:
  bool LoadSlot(Core::System& system, s64 frame, bool redisplay);

  struct Slot
  {
    s64 frame = -1;
    std::vector<u8> mem1;
    std::vector<u8> mem2;
    std::vector<u8> l1_cache;
    Common::UniqueBuffer<u8> state;
    std::size_t state_size = 0;
    u64 nand_journal_mark = 0;
    u64 cow_id = 0;
  };

  Slot* Find(s64 frame);
  const Slot* Find(s64 frame) const;
  void Forget(Slot* slot);
  bool UseCow(Core::System& system, bool* armed_now);

  std::vector<Slot> m_slots;
  std::size_t m_next = 0;
  std::size_t m_last = 0;
  std::optional<bool> m_cow;
};

u64 RamChecksum(std::span<const u8> mem1, std::span<const u8> mem2);

std::optional<GCPadStatus> InputOverride(int port);
std::optional<GCPadStatus> RawInputOverride(int port);

void OnFrameBoundary(const Core::CPUThreadGuard& guard);
u64 HostFloatControlForTests();
double LastFrameHeldMs();
int LastFrameHeldFrames();
}
