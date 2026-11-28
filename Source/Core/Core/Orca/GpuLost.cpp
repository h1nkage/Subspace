// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/GpuLost.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>

#include "Core/Core.h"
#include "Core/Orca/CrashNote.h"
#include "Core/Orca/GpuDiagnostics.h"
#include "Core/Orca/Status.h"
#include "Core/System.h"

namespace Orca::GpuLost
{
namespace
{
constexpr size_t LINE_SIZE = 320;
constexpr size_t STATE_SIZE = 200;

std::atomic<bool> s_lost{false};

std::mutex s_state_writer;
std::atomic<u32> s_state_seq{0};
std::atomic<size_t> s_state_length{0};
std::array<std::atomic<char>, STATE_SIZE> s_state{};

using Line = CrashNote::FixedLine;

void Cause(Line& line, std::string_view backend, u32 reason)
{
  line << backend << ": " << ReasonName(reason) << ", ";
  line.Hex(reason, 8);
}
}

const char* ReasonName(u32 hr)
{
  switch (hr)
  {
  case 0x887A0005:
    return "device removed";
  case 0x887A0006:
    return "device hung";
  case 0x887A0007:
    return "device reset";
  case 0x887A0020:
    return "driver internal error";
  case 0x887A0001:
    return "invalid call";
  case 0x8007000E:
    return "out of memory";
  case 0x80070057:
    return "invalid argument";
  default:
    return "error";
  }
}

size_t FormatLine(char* out, size_t size, std::string_view backend, u32 reason,
                  std::string_view where)
{
  Line line(out, size);
  line << "Orca: [VIDEO] the graphics device is lost (";
  Cause(line, backend, reason);
  line << ") at " << where << "; Orca stops";
  return line.Length();
}

bool Report(std::string_view backend, u32 reason, std::string_view where)
{
  if (s_lost.exchange(true, std::memory_order_acq_rel))
    return false;

#ifdef _WIN32
  GpuDiagnostics::LogMemory(true);
#endif
  char text[LINE_SIZE];
  const size_t length = FormatLine(text, sizeof(text), backend, reason, where);
  CrashNote::WriteLine(stderr, std::string_view(text, length));

  {
    Line state(text, sizeof(text));
    state << "lost (";
    Cause(state, backend, reason);
    state << ") at " << where;
    SetDeviceState(std::string_view(text, state.Length()));
  }

  auto& system = Core::System::GetInstance();
  const Core::State core = Core::GetState(system);
  if (core == Core::State::Uninitialized || core == Core::State::Stopping)
    return true;

  Status::Error(CODE, SENTENCE);
  Core::QueueHostJob([](Core::System& host_system) { Core::Stop(host_system); });
  return true;
}

bool Lost()
{
  return s_lost.load(std::memory_order_acquire);
}

void DeviceCreated(std::string_view backend)
{
  s_lost.store(false, std::memory_order_release);
  char text[STATE_SIZE];
  Line state(text, sizeof(text));
  state << backend << " running";
  SetDeviceState(std::string_view(text, state.Length()));
}

void DeviceClosed(std::string_view backend)
{
  if (Lost())
    return;
  char text[STATE_SIZE];
  Line state(text, sizeof(text));
  state << backend << " closed";
  SetDeviceState(std::string_view(text, state.Length()));
}

void SetDeviceState(std::string_view state)
{
  std::lock_guard lock(s_state_writer);
  const u32 seq = s_state_seq.load(std::memory_order_relaxed);
  s_state_seq.store(seq + 1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  const size_t length = std::min(state.size(), STATE_SIZE);
  for (size_t i = 0; i < length; ++i)
    s_state[i].store(state[i], std::memory_order_relaxed);
  s_state_length.store(length, std::memory_order_relaxed);
  s_state_seq.store(seq + 2, std::memory_order_release);
}

size_t DeviceState(char* out, size_t size)
{
  if (!size)
    return 0;
  for (int attempt = 0; attempt < 64; ++attempt)
  {
    const u32 before = s_state_seq.load(std::memory_order_acquire);
    if (before & 1)
      continue;
    const size_t length =
        std::min({s_state_length.load(std::memory_order_relaxed), size - 1, STATE_SIZE});
    for (size_t i = 0; i < length; ++i)
      out[i] = s_state[i].load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (s_state_seq.load(std::memory_order_relaxed) == before)
    {
      out[length] = '\0';
      return length;
    }
  }
  out[0] = '\0';
  return 0;
}
}
