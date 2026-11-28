// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Embed
{
struct Rect
{
  int x = 0;
  int y = 0;
  int w = 1;
  int h = 1;
};

struct WindowBounds
{
  double x, y, w, h;
  double scale;
};
bool TraceEnabled();
void Trace(const Rect& requested, const WindowBounds& actual);

struct Options
{
  bool enabled = false;
  unsigned long long parent = 0;
  Rect rect;
  bool too_small = false;
  bool adopted = false;
};

class Result
{
public:
  explicit Result(bool enabled = false) : m_enabled(enabled) {}
  void Decide(std::string_view reason = {});
  bool Decided() const { return !m_enabled || m_decided; }
  std::optional<std::string> TakeLine(bool ready);

private:
  bool m_enabled = false;
  bool m_decided = false;
  std::optional<std::string> m_line;
};

std::optional<Options> TakeArguments(std::vector<char*>* argv, std::string* error);

std::optional<Rect> ParseRect(const std::string& text, bool clamp_size = true);

constexpr int MIN_SIDE = 32;
bool TooSmall(const Rect& rect);

std::FILE* ClaimStdout();

void Out(const std::string& line);

void StartReading();
std::vector<std::string> TakeCommands();
}
