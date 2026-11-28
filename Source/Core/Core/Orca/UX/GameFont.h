// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

namespace Orca::UX::GameFont
{
std::optional<std::vector<u8>> DecompressLz10(std::span<const u8> data);

char32_t NextCodePoint(std::string_view text, std::size_t* at);

struct Widths
{
  int left = 0;
  int glyph = 0;
  int advance = 0;
};

class Rfnt
{
public:
  static std::optional<Rfnt> Parse(std::vector<u8> data, std::string* error = nullptr);

  int CellWidth() const { return m_cell_w; }
  int CellHeight() const { return m_cell_h; }
  int Ascent() const { return m_ascent; }
  int LineFeed() const { return m_linefeed; }
  int Baseline() const { return m_baseline; }
  std::size_t GlyphCount() const { return m_glyphs; }
  std::optional<u16> IndexOf(char32_t code) const;
  Widths WidthsOf(u16 index) const;
  bool GlyphAlpha(u16 index, std::vector<u8>* out) const;

private:
  struct Map
  {
    u16 begin, end, method;
    std::size_t info;
  };
  struct WidthBlock
  {
    u16 begin, end;
    std::size_t table;
  };
  u16 ReadU16(std::size_t at) const;
  const std::vector<u8>& Sheet(int sheet) const;

  std::vector<u8> m_data;
  int m_cell_w = 0, m_cell_h = 0, m_ascent = 0, m_linefeed = 0, m_baseline = 0;
  u16 m_alter = 0;
  Widths m_default;
  std::size_t m_glyphs = 0;
  u32 m_sheet_size = 0;
  int m_sheet_count = 0, m_sheet_format = 0, m_per_row = 0, m_per_column = 0;
  int m_sheet_w = 0, m_sheet_h = 0;
  std::size_t m_sheet_data = 0;
  std::vector<Map> m_maps;
  std::vector<WidthBlock> m_widths;
  mutable std::map<int, std::vector<u8>> m_sheets;
};

std::vector<std::optional<Rfnt>> ReadFromDisc(const std::string& disc,
                                              std::span<const char* const> files,
                                              std::string_view game_id, std::string* error = nullptr);

struct Atlas
{
  struct Glyph
  {
    float u0, v0, u1, v1;
    float left, width;
    float advance;
  };
  int width = 0, height = 0;
  std::vector<std::vector<u8>> levels;
  std::map<char32_t, Glyph> glyphs;
  float cell_height = 0;
  float ascent = 0;
  float space = 0;
};

std::vector<char32_t> OverlayCodePoints();
std::u32string Substitute(char32_t code);

Atlas BuildAtlas(const Rfnt& font, std::span<const char32_t> codes, int pad = 4, int levels = 4);
}
