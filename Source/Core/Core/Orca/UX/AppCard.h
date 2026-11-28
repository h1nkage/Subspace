// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

#include "Core/Orca/UX/Kit.h"

namespace Orca::UX::AppCard
{
void Warm();
ImU32 AvatarColour(std::string_view name);
float TextWidth(std::string_view text, float px);
std::vector<std::string> Wrap(std::string_view text, float px, float width);
Kit::Box ChatCard(ImDrawList* dl, ImVec2 pos, float unit, std::string_view name,
                  std::string_view text, float width, float alpha = 1);
Kit::Box NoticeCard(ImDrawList* dl, ImVec2 pos, float unit, std::string_view text, float width,
                    float alpha = 1);
void Logo(ImDrawList* dl, ImVec2 pos, float size, float alpha = 1);

void RoundLogo(ImDrawList* dl, ImVec2 pos, float size, float alpha = 1);
void Text(ImDrawList* dl, ImVec2 pos, float px, ImU32 colour, std::string_view text);
void CardSlab(ImDrawList* dl, ImVec2 min, ImVec2 max, float unit, float alpha = 1);
ImU32 Ink(float alpha = 1);
ImU32 Ink2(float alpha = 1);
ImU32 Edge(float alpha = 1);
void Avatar(ImDrawList* dl, ImVec2 centre, float r, std::string_view name, float alpha = 1);
std::string InitialOf(std::string_view name);
void AvatarOf(ImDrawList* dl, ImVec2 centre, float r, ImU32 colour, std::string_view initial,
              float alpha = 1);
}
