// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string_view>

#include <imgui.h>

namespace Orca::UX::Widgets
{
enum class Tone : int
{
  Neutral,
  Win,
  Loss,
  Warning,
};

constexpr float TITLE_SIZE = 10.0f;
constexpr float HEADING_SIZE = 5.0f;
constexpr float LINE_SIZE = 3.4f;
constexpr float SMALL_SIZE = 2.8f;
constexpr float HINT_SIZE = 3.2f;
constexpr float NUMBER_SIZE = 6.5f;
constexpr float COUNTDOWN_SIZE = 6.0f;

ImVec2 Measure(float size, std::string_view text);
ImVec2 MeasureTitle(float unit, std::string_view text);
ImVec2 MeasureNumber(float unit, std::string_view digits);

void Panel(ImDrawList* dl, ImVec2 min, ImVec2 max, float unit, Tone tone, float alpha);

ImVec2 Title(ImDrawList* dl, float centre_x, float top, float unit, std::string_view text,
             Tone tone, float alpha, float scale = 1);

ImVec2 Line(ImDrawList* dl, float centre_x, float top, float size, std::string_view text, Tone tone,
            float alpha);

ImVec2 BigNumber(ImDrawList* dl, float centre_x, float top, float unit, int value, float alpha,
                 float scale = 1);

float Countdown(ImDrawList* dl, ImVec2 centre, float unit, int seconds, float fraction, Tone tone,
                float alpha);
float CountdownRadius(float unit);

ImVec2 DeltaBadge(ImDrawList* dl, float left, float centre_y, float unit, int delta, float alpha,
                  float scale = 1);
ImVec2 DeltaBadgeSize(float unit, int delta);

ImVec2 Hint(ImDrawList* dl, float centre_x, float top, float unit, std::string_view text,
            float progress, float alpha);

std::string_view ClockText(int seconds, char (&buffer)[16]);
}
