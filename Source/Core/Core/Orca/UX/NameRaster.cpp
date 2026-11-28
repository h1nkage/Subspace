// Copyright 2026 Subspace
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/NameRaster.h"

#include <algorithm>
#include <cmath>

#if defined(_MSC_VER)
#pragma warning(disable : 4505)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <imstb_truetype.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace Orca::UX::NameRaster
{
namespace
{
std::vector<int> Codepoints(std::string_view text)
{
  std::vector<int> out;
  std::size_t i = 0;
  while (i < text.size())
  {
    const auto c = static_cast<unsigned char>(text[i]);
    int cp = '?';
    std::size_t n = 1;
    if (c < 0x80)
      cp = c;
    else if ((c & 0xE0) == 0xC0)
      n = 2, cp = c & 0x1F;
    else if ((c & 0xF0) == 0xE0)
      n = 3, cp = c & 0x0F;
    else if ((c & 0xF8) == 0xF0)
      n = 4, cp = c & 0x07;
    if (n > 1)
    {
      if (i + n > text.size())
        break;
      for (std::size_t k = 1; k < n; ++k)
      {
        const auto cc = static_cast<unsigned char>(text[i + k]);
        if ((cc & 0xC0) != 0x80)
        {
          cp = '?';
          n = k;
          break;
        }
        cp = (cp << 6) | (cc & 0x3F);
      }
    }
    out.push_back(cp);
    i += n;
  }
  return out;
}
}

std::optional<Bitmap> Outlined(const std::vector<u8>& ttf, float size, std::string_view text,
                               float outline)
{
  if (ttf.empty() || text.empty() || !(size > 0) || size > 512 || !(outline >= 0) || outline > 64)
    return std::nullopt;
  stbtt_fontinfo info;
  const int offset = stbtt_GetFontOffsetForIndex(ttf.data(), 0);
  if (offset < 0 || !stbtt_InitFont(&info, ttf.data(), offset))
    return std::nullopt;

  const float scale = stbtt_ScaleForPixelHeight(&info, size);
  int ascent = 0, descent = 0, gap = 0;
  stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
  const std::vector<int> cps = Codepoints(text);

  float advance = 0;
  for (const int cp : cps)
  {
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&info, cp, &adv, &lsb);
    advance += adv * scale;
  }
  const int pad = static_cast<int>(std::ceil(outline)) + 2;
  const float baseline = std::ceil(ascent * scale);
  const int width = static_cast<int>(std::ceil(advance)) + 2 * pad;
  const int height = static_cast<int>(std::ceil((ascent - descent) * scale)) + 2 * pad;
  if (width <= 0 || height <= 0 || width > 4096 || height > 1024)
    return std::nullopt;

  std::vector<float> fill(static_cast<std::size_t>(width) * height, 0.0f);
  float x = static_cast<float>(pad);
  std::vector<u8> glyph;
  for (const int cp : cps)
  {
    const float shift = x - std::floor(x);
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetCodepointBitmapBoxSubpixel(&info, cp, scale, scale, shift, 0, &x0, &y0, &x1, &y1);
    const int gw = x1 - x0;
    const int gh = y1 - y0;
    if (gw > 0 && gh > 0)
    {
      glyph.assign(static_cast<std::size_t>(gw) * gh, 0);
      stbtt_MakeCodepointBitmapSubpixel(&info, glyph.data(), gw, gh, gw, scale, scale, shift, 0,
                                        cp);
      const int ox = static_cast<int>(std::floor(x)) + x0;
      const int oy = pad + static_cast<int>(baseline) + y0;
      for (int gy = 0; gy < gh; ++gy)
      {
        const int py = oy + gy;
        if (py < 0 || py >= height)
          continue;
        for (int gx = 0; gx < gw; ++gx)
        {
          const int px = ox + gx;
          if (px < 0 || px >= width)
            continue;
          float& f = fill[static_cast<std::size_t>(py) * width + px];
          f = std::min(1.0f, f + glyph[static_cast<std::size_t>(gy) * gw + gx] / 255.0f);
        }
      }
    }
    int adv = 0, lsb = 0;
    stbtt_GetCodepointHMetrics(&info, cp, &adv, &lsb);
    x += adv * scale;
  }

  std::vector<float> ring(fill.size(), 0.0f);
  const int r = static_cast<int>(std::ceil(outline));
  for (int py = 0; py < height; ++py)
  {
    for (int px = 0; px < width; ++px)
    {
      float best = 0;
      for (int dy = -r; dy <= r && best < 1.0f; ++dy)
      {
        const int sy = py + dy;
        if (sy < 0 || sy >= height)
          continue;
        for (int dx = -r; dx <= r; ++dx)
        {
          const int sx = px + dx;
          if (sx < 0 || sx >= width)
            continue;
          const float weight =
              std::clamp(outline + 0.5f - std::sqrt(static_cast<float>(dx * dx + dy * dy)), 0.0f,
                         1.0f);
          if (weight > 0)
            best = std::max(best, weight * fill[static_cast<std::size_t>(sy) * width + sx]);
        }
      }
      ring[static_cast<std::size_t>(py) * width + px] = best;
    }
  }

  Bitmap out;
  out.width = static_cast<u32>(width);
  out.height = static_cast<u32>(height);
  out.left = static_cast<float>(-pad);
  out.top = static_cast<float>(-pad);
  out.rgba.resize(fill.size() * 4);
  for (std::size_t i = 0; i < fill.size(); ++i)
  {
    const float fa = fill[i];
    const float a = fa + ring[i] * (1.0f - fa);
    const float c = a > 0 ? fa / a : 0.0f;
    const auto shade = static_cast<u8>(std::lround(c * 255.0f));
    out.rgba[i * 4 + 0] = shade;
    out.rgba[i * 4 + 1] = shade;
    out.rgba[i * 4 + 2] = shade;
    out.rgba[i * 4 + 3] = static_cast<u8>(std::lround(a * 255.0f));
  }
  return out;
}
}
