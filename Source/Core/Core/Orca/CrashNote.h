// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

namespace Orca::CrashNote
{
bool WriteLine(std::FILE* out, std::string_view text) noexcept;

std::string TypeName(std::string_view decorated);

struct ThrownTypes
{
  std::vector<std::string> decorated;
  std::optional<s32> std_exception_offset;
};
ThrownTypes ReadThrowInfo(const u8* image_base, const u8* throw_info);

class FixedLine
{
public:
  FixedLine(char* out, size_t size);
  FixedLine& operator<<(std::string_view text);
  FixedLine& Hex(u64 value, int digits = 1);
  FixedLine& Dec(u64 value);
  size_t Length() const { return m_length; }

private:
  char* m_out;
  size_t m_size;
  size_t m_length = 0;
};

struct Fault
{
  u32 code = 0;
  u64 address = 0;
  std::string_view module;
  u64 offset = 0;
  bool has_access = false;
  u64 access_kind = 0;
  u64 access_address = 0;
  u32 thread = 0;
  std::string_view graphics;
  bool first_chance = false;
};
const char* FaultName(u32 code);
size_t FormatFault(char* out, size_t size, const Fault& fault);

constexpr size_t MAX_FAULT_STACK_FRAMES = 24;
constexpr size_t MAX_FAULT_STACK_LINE = 300;
struct StackFrame
{
  std::string_view module;
  u64 offset = 0;
};
size_t FormatFaultStack(char* out, size_t size, std::span<const StackFrame> frames) noexcept;

void InstallFaultReporter();

void InstallThrowReporter();
}
