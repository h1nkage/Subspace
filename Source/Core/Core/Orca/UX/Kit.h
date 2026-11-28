// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

namespace Orca::UX::GameFont
{
struct Atlas;
}

namespace Orca::UX::Kit
{
enum class Face
{
  Body,
  Heavy,
};

enum class Align
{
  Left,
  Centre,
  Right,
};

enum class Look
{
  Brawl,
  ProjectPlus,
};
void SetLook(Look look);
Look CurrentLook();

enum class Tone
{
  Panel,
  Gold,
  Grey,
  Red,
  Blue,
  Yellow,
  Green,
};
Tone PortTone(int port);

struct TextLook
{
  Face face = Face::Body;
  float px = 20;
  ImU32 top = IM_COL32_WHITE;
  ImU32 bottom = IM_COL32_WHITE;
  float outline = 0;
  ImU32 outline_colour = IM_COL32(12, 14, 24, 255);
  ImVec2 shadow{0, 0};
  ImU32 shadow_colour = IM_COL32(0, 0, 0, 150);
  float italic = 0;
  float tracking = 0;
};

TextLook LabelLook(float unit, float size = 3.0f);
TextLook TitleLook(float unit, float size = 5.0f);

void SetGameDisc(std::string path);
void BeginFrame();
void ReleaseGpu();
void Shutdown();
bool HasGameFont(Face face);

ImVec2 Measure(const TextLook& look, std::string_view text);
ImVec2 Text(ImDrawList* dl, ImVec2 pos, const TextLook& look, std::string_view text,
            float alpha = 1, Align align = Align::Left);

std::vector<std::string> Wrap(const TextLook& look, std::string_view text, float width);

struct Box
{
  ImVec2 min, max;
};

void Plate(ImDrawList* dl, ImVec2 min, ImVec2 max, float unit, Tone tone, float alpha = 1);
Box Banner(ImDrawList* dl, float left, float right, float top, float height, float unit,
           Tone tone, std::string_view title, std::string_view note = {}, float alpha = 1);
float BannerHeight(float row, std::size_t note_lines);
float BannerWidth(float height, float unit, std::string_view title, std::string_view note = {});
Box Title(ImDrawList* dl, ImVec2 pos, float unit, std::string_view text, float size = 5.0f,
          Align align = Align::Left, float alpha = 1);
Box Label(ImDrawList* dl, ImVec2 pos, float unit, std::string_view text, float size = 3.0f,
          Align align = Align::Left, ImU32 colour = IM_COL32_WHITE, float alpha = 1);
Box LabelPlate(ImDrawList* dl, ImVec2 pos, float unit, std::string_view text, float size = 3.0f,
               Align align = Align::Left, Tone tone = Tone::Panel, float alpha = 1);
Box Timer(ImDrawList* dl, ImVec2 centre, float unit, int seconds, double now_s, float alpha = 1);
Box Badge(ImDrawList* dl, ImVec2 centre, float unit, std::string_view text, Tone tone,
          float size = 3.6f, float alpha = 1);
struct Blink
{
  float on = 0;
  float swell = 0;
  std::array<float, 3> ripples{-1, -1, -1};
};
Blink Attention(double now_s, double since_s);
enum class PointerMode
{
  Plain,
  Acting,
  Waiting,
};
void Pointer(ImDrawList* dl, ImVec2 at, float radius, float unit, int port, std::string_view tag,
             PointerMode mode, bool tag_left, double now_s, double since_s);
void Pointer(ImDrawList* dl, ImVec2 at, float radius, float unit, int port, std::string_view tag,
             bool turn, bool tag_left, double now_s);
Box TurnTag(ImDrawList* dl, ImVec2 at, float radius, float unit, std::string_view title,
            std::string_view detail, Tone tone, int seconds, double now_s, bool lean_left,
            ImVec2 clamp_min, ImVec2 clamp_max, float rim = 0, float size = 3.4f,
            bool below = false);
struct Beat
{
  float swell = 0;
  float ripple = 0;
};
Beat TurnPulse(double now_s);
void PortraitFrame(ImDrawList* dl, ImVec2 min, ImVec2 max, float unit, Tone tone);
Box Ribbon(ImDrawList* dl, ImVec2 min, ImVec2 max, float unit, std::string_view text, Tone tone);
Box SetDots(ImDrawList* dl, ImVec2 centre, float unit, const std::array<int, 3>& games, int current);
void SearchRing(ImDrawList* dl, ImVec2 centre, float radius, float unit, double now_s);
enum class Lettering
{
  Band,
  Line,
};
Box GameTimer(ImDrawList* dl, ImVec2 centre, float unit, int seconds, double now_s,
              Lettering lettering, float size = 4.2f);
struct GuideItem
{
  std::string button;
  std::string label;
  bool operator==(const GuideItem&) const = default;
};
std::vector<GuideItem> GuideOf(std::string_view hint);
Box Guide(ImDrawList* dl, float right, float cy, float unit, const std::vector<GuideItem>& items,
          float alpha = 1, float max_width = 0);
void Frame(ImDrawList* dl, ImVec2 min, ImVec2 max, float unit, Tone tone, std::string_view tag,
           bool tag_left, float inset = 0);
void NameStroke(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 colour);
bool OutlinedText(ImDrawList* dl, const std::string& font_path, float size, ImVec2 at,
                  std::string_view text, float outline, ImU32 tint);
void Connection(ImDrawList* dl, ImVec2 centre, float radius, float unit, int bars,
                float alpha = 1);

ImU32 Accent();
TextLook InkLook(float unit, float size, Tone tone = Tone::Panel);
ImVec2 BadgeSize(float unit, std::string_view text, Tone tone, float size = 3.6f);
Box TextPlate(ImDrawList* dl, ImVec2 pos, float unit, const std::vector<std::string>& lines,
              float size = 3.0f, std::size_t lead = 0, Tone tone = Tone::Panel, float alpha = 1);
void Disc(ImDrawList* dl, ImVec2 centre, float radius, float unit, Tone tone, float alpha = 1);

void UseAtlasForTests(Face face, const GameFont::Atlas* atlas, ImTextureID texture);
void UseFontsForTests(const GameFont::Atlas* body, const GameFont::Atlas* heavy);

int DemoMode();
void DrawDemo(ImDrawList* dl, ImVec2 picture_min, ImVec2 picture_size, float unit, double now_s);
}
