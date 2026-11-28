// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <atomic>
#include <cstddef>
#include <span>
#include "Common/CommonTypes.h"

namespace Broadcast
{
void StartReplay();
void StopReplay();
void SetSilentAudio(bool enabled);
bool SilentAudioRunning();
inline std::atomic<bool> s_enabled{false};
inline bool Active() { return s_enabled.load(std::memory_order_acquire); }
u64 NowMicros();
void Video(const u8* rgba, size_t stride, u64 timestamp);
void VideoDrop();
void Audio(const s16* samples, size_t frames, u32 rate);
}
