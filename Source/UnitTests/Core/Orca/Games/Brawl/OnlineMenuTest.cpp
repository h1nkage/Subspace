// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/ScopeGuard.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/UX/GamePatches.h"
#include "Core/Orca/Games/Brawl/MenuText.h"
#include "Core/Orca/Engine/GuestMemory.h"
#include "Core/Orca/Games/Brawl/BrawlGame.h"
#include "Core/Orca/Games/Brawl/OnlineMenu.h"
#include "Core/Rollback/OnlineMatch.h"
#include "VideoCommon/HiresTextures.h"

using namespace Orca::UX;

namespace
{
std::filesystem::path SysSubspace()
{
  return std::filesystem::path(__FILE__).parent_path() / "../../../../../../Data/Sys/Subspace";
}

std::vector<GamePatch> Shipped(const char* name)
{
  std::string text;
  EXPECT_TRUE(File::ReadFileToString((SysSubspace() / name).string(), text)) << name;
  std::string error;
  const auto patches = ParseGamePatches(text, &error);
  EXPECT_TRUE(patches.has_value()) << name << ": " << error;
  return patches.value_or(std::vector<GamePatch>{});
}

std::vector<GamePatch> GroupAt(const std::vector<GamePatch>& patches, u32 address)
{
  const auto first = std::find_if(patches.begin(), patches.end(),
                                  [&](const GamePatch& p) { return p.address == address; });
  if (first == patches.end() || first->joins_previous)
    return {};
  std::vector<GamePatch> group{*first};
  for (auto it = first + 1; it != patches.end() && it->joins_previous; ++it)
    group.push_back(*it);
  return group;
}

bool AnyLineIn(const std::vector<GamePatch>& patches, u32 from, u32 to)
{
  return std::any_of(patches.begin(), patches.end(),
                     [&](const GamePatch& p) { return p.address >= from && p.address <= to; });
}

std::optional<u32> BranchTarget(u32 at, u32 word)
{
  if ((word >> 26) != 18 || (word & 2))
    return std::nullopt;
  s32 offset = static_cast<s32>(word & 0x03FFFFFC);
  if (offset & 0x02000000)
    offset -= 0x04000000;
  return at + static_cast<u32>(offset);
}

std::vector<u32> Values(const std::vector<GamePatch>& group)
{
  std::vector<u32> out;
  for (const GamePatch& p : group)
    out.push_back(p.value);
  return out;
}

std::vector<u32> Originals(const std::vector<GamePatch>& group)
{
  std::vector<u32> out;
  for (const GamePatch& p : group)
    out.push_back(p.original.value_or(0xDEADBEEF));
  return out;
}

void ExpectOnlineGates(const std::vector<GamePatch>& patches, u32 module)
{
  for (const GamePatch& p : patches)
    EXPECT_TRUE(p.original.has_value() || p.address >= 0x90000000) << std::hex << p.address;
  for (const u32 at : {0x81178A58u, 0x81178C30u})
  {
    SCOPED_TRACE(at);
    const auto g = GroupAt(patches, at + module);
    ASSERT_EQ(g.size(), 3u);
    EXPECT_EQ(Originals(g), (std::vector<u32>{0xA01D0042, 0x28000002, 0x4082007C}));
    EXPECT_EQ(Values(g), (std::vector<u32>{0xA01D0042, 0x28000002, 0x4800007C}));
    EXPECT_TRUE(GroupApplies(g, Originals(g)));
    EXPECT_FALSE(GroupApplies(g, Values(g)));
  }
  for (const u32 at : {0x81179F04u, 0x8117A104u})
  {
    SCOPED_TRACE(at);
    const u32 copy = at - 0x81179F04;
    const auto g = GroupAt(patches, at + module);
    ASSERT_EQ(g.size(), 39u);
    for (const GamePatch& p : g)
      EXPECT_TRUE(p.original.has_value());
    EXPECT_EQ(*g[0].original, 0x3800001Au);
    EXPECT_EQ(BranchTarget(g[0].address, g[0].value), std::optional<u32>(g[26].address));
    EXPECT_EQ(g[0].value & 1, 0u);
    EXPECT_EQ(*g[3].original, 0x3800001Bu);
    EXPECT_EQ(*g[6].original, 0x3800001Cu);
    for (size_t i = 1; i < 17; ++i)
      EXPECT_EQ(g[i].value, *g[i].original) << i;
    EXPECT_EQ(g[17].address, 0x81179F48u + copy + module);
    EXPECT_EQ(g[17].value, 0x7FA3EB78u);
    EXPECT_EQ(g[18].value, 0x38800000u);
    EXPECT_EQ(BranchTarget(g[19].address, g[19].value), std::optional<u32>(0x806A5A58));
    EXPECT_EQ(g[19].value & 1, 1u);
    EXPECT_EQ(g[20].value, 0x38000000u);
    EXPECT_EQ(g[21].value, 0x901D0634u);
    EXPECT_EQ(g[22].value, 0x38000003u);
    EXPECT_EQ(g[23].value, 0x901D0664u);
    EXPECT_EQ(g[24].value, 0x38600001u);
    EXPECT_EQ(BranchTarget(g[25].address, g[25].value), std::optional<u32>(0x8117A1D4 + module));
    EXPECT_EQ(g[25].value & 1, 0u);
    EXPECT_TRUE(BranchTarget(g[17].address, *g[17].original).has_value());
    EXPECT_EQ(g[26].value, 0x7FA3EB78u);
    EXPECT_EQ(g[27].value, 0x38800019u);
    EXPECT_EQ(g[28].value, 0x38A00000u);
    EXPECT_EQ(BranchTarget(g[29].address, g[29].value), std::optional<u32>(0x806A5604));
    EXPECT_EQ(g[29].value & 1, 1u);
    EXPECT_EQ(g[30].value, 0x3C60805Au);
    EXPECT_EQ(g[31].value, 0x38800013u);
    EXPECT_EQ(g[32].value, 0x806301D0u);
    EXPECT_EQ(g[33].value, 0x38A0FFFFu);
    EXPECT_EQ(g[34].value, 0x38C00000u);
    EXPECT_EQ(g[35].value, 0x38E00000u);
    EXPECT_EQ(g[36].value, 0x3900FFFFu);
    EXPECT_EQ(BranchTarget(g[37].address, g[37].value), std::optional<u32>(0x800742B0));
    EXPECT_EQ(g[37].value & 1, 1u);
    EXPECT_EQ(g[38].value, *g[38].original);
    EXPECT_EQ(BranchTarget(g[38].address, g[38].value), std::optional<u32>(0x8117A1D0 + module));
    EXPECT_TRUE(GroupApplies(g, Originals(g)));
    EXPECT_FALSE(GroupApplies(g, Values(g)));
  }
  for (const auto& [at, words, calls] : std::vector<std::tuple<u32, size_t, std::vector<size_t>>>{
           {0x81179A04, 8, {1, 7}}, {0x81191D7C, 3, {1}}})
  {
    SCOPED_TRACE(at);
    const auto g = GroupAt(patches, at + module);
    ASSERT_EQ(g.size(), words);
    for (size_t i = 0; i < g.size(); ++i)
    {
      const bool call = std::find(calls.begin(), calls.end(), i) != calls.end();
      if (call)
      {
        EXPECT_EQ(BranchTarget(g[i].address, *g[i].original), std::optional<u32>(0x8014FEDC));
        EXPECT_EQ(g[i].value, 0x38600000u);
      }
      else
      {
        EXPECT_EQ(g[i].value, *g[i].original);
      }
    }
  }
  {
    const auto g = GroupAt(patches, 0x80701A04);
    ASSERT_EQ(g.size(), 8u);
    EXPECT_EQ(Originals(g), (std::vector<u32>{0x806DC8C8, 0x806DC8C8, 0x806DC8C8, 0x806DC8C8,
                                              0x806DC940, 0x806DC8E0, 0x806DC8F8, 0x806DC910}));
    EXPECT_EQ(Values(g), (std::vector<u32>{0x806DC8C8, 0x806DC8C8, 0x806DC8C8, 0x806DC8C8,
                                           0x806DC940, 0x806DC8C8, 0x806DC8C8, 0x806DC8C8}));
  }
  {
    const auto block = GroupAt(patches, 0x806DC8C8);
    ASSERT_EQ(block.size(), 22u);
    EXPECT_EQ(block[0].value, 0x7E439378u);
    EXPECT_EQ(block[1].value, 0x38930030u);
    EXPECT_EQ(block[2].value, 0x38A00000u);
    EXPECT_EQ(BranchTarget(block[3].address, block[3].value), std::optional<u32>(0x8002D640));
    EXPECT_EQ(block[3].value & 1, 1u);
    EXPECT_EQ(block[4].value, 0x80720014u);
    EXPECT_EQ(block[5].value, 0x80120284u);
    EXPECT_EQ(block[6].value, 0x90030018u);
    EXPECT_EQ(block[7].value, 0x93CF0008u);
    EXPECT_EQ(BranchTarget(block[8].address, block[8].value), std::optional<u32>(0x806DC9D0));
    EXPECT_EQ(block[8].value & 1, 0u);
    EXPECT_EQ(block[9].address, 0x806DC8ECu);
    EXPECT_EQ(block[9].value, 0x80AF0018u);
    EXPECT_EQ(block[11].value, 0x900F0018u);
    EXPECT_EQ(block[15].value, 0x38A5FFFEu);
    EXPECT_EQ(block[16].value, 0x28000004u);
    EXPECT_EQ(block[17].value, 0x4C800020u);
    EXPECT_EQ(block[18].value, 0x38A0001Eu);
    EXPECT_EQ(block[19].value, 0x4E800020u);
    EXPECT_EQ(block[20].value, 0x38A00001u);
    EXPECT_EQ(block[21].value, 0x4E800020u);
    const auto back = GroupAt(patches, 0x806DCE34);
    ASSERT_EQ(back.size(), 4u);
    EXPECT_EQ(*back[2].original, 0x38A00001u);
    EXPECT_EQ(BranchTarget(back[2].address, back[2].value), std::optional<u32>(0x806DC8EC));
    EXPECT_EQ(back[2].value & 1, 1u);
    EXPECT_EQ(BranchTarget(back[3].address, back[3].value), std::optional<u32>(0x8002D640));
    for (const size_t guard : {0u, 1u, 3u})
      EXPECT_EQ(back[guard].value, *back[guard].original);
  }
  EXPECT_FALSE(AnyLineIn(patches, 0x800C9B44, 0x800C9D40));
}
}

TEST(OrcaOnlineMenuGates, BrawlOpensTheOnlinePageWithNoConnection)
{
  const auto patches = Shipped("RSBE01.patches");
  EXPECT_FALSE(AnyLineIn(patches, 0x811A4070, 0x811A40CC));
  ExpectOnlineGates(patches, 0);
}

TEST(OrcaOnlineMenuGates, ProjectPlusOpensTheOnlinePageWithNoConnection)
{
  const auto patches = Shipped("PPLUS32.patches");
  for (const GamePatch& p : patches)
    EXPECT_TRUE(p.original.has_value()) << std::hex << p.address;
  EXPECT_FALSE(AnyLineIn(patches, 0x811A54FC, 0x811A5558));
  ExpectOnlineGates(patches, 0x1480);
}

TEST(OrcaOnlineMenuGates, SceneGroupsAreTheSameInBothGames)
{
  const auto brawl = Shipped("RSBE01.patches");
  const auto pplus = Shipped("PPLUS32.patches");
  for (const u32 at : {0x80701A04u, 0x806DC8C8u, 0x806DCE34u})
  {
    SCOPED_TRACE(at);
    const auto b = GroupAt(brawl, at);
    const auto p = GroupAt(pplus, at);
    ASSERT_FALSE(b.empty());
    EXPECT_EQ(Originals(b), Originals(p));
    EXPECT_EQ(Values(b), Values(p));
  }
}

namespace
{
struct SliderLimits
{
  u32 left = 100;
  s32 right = 0;
  bool right_ble = false;
};

SliderLimits SliderGroups(const std::vector<GamePatch>& patches, u32 module)
{
  const auto left = GroupAt(patches, 0x8117B43C + module);
  const auto right = GroupAt(patches, 0x8117B500 + module);
  EXPECT_EQ(Originals(left), (std::vector<u32>{0xA003066C, 0x28000064, 0x408000B4}));
  EXPECT_EQ(Originals(right), (std::vector<u32>{0xA003066C, 0x2C000000, 0x418200B0}));
  if (left.size() != 3 || right.size() != 3)
  {
    ADD_FAILURE() << "no slider groups";
    return {};
  }
  EXPECT_EQ(left[0].value, *left[0].original);
  EXPECT_EQ(left[2].value, *left[2].original);
  EXPECT_EQ(right[0].value, *right[0].original);
  EXPECT_EQ(left[1].value & 0xFFFF0000, 0x28000000u);
  EXPECT_EQ(right[1].value & 0xFFFF0000, 0x2C000000u);
  EXPECT_EQ(right[2].value, 0x408100B0u);
  return {left[1].value & 0xFFFF, static_cast<s16>(right[1].value & 0xFFFF),
          right[2].value == 0x408100B0};
}

u16 SliderFrame(u16 value, u16* pause, bool left, bool right, const SliderLimits& limits)
{
  const auto step = [&](int by) -> u16 {
    if (value == 50 && *pause < 20)
    {
      ++*pause;
      return value;
    }
    *pause = 0;
    return static_cast<u16>(value + by);
  };
  if (left && value < limits.left)
    return step(1);
  const s32 v = value;
  if (right && !(limits.right_ble ? v <= limits.right : v == limits.right))
    return step(-1);
  return value;
}

std::pair<u16, u16> HoldBothWays(u16 start, int frames, const SliderLimits& limits,
                                 std::set<u16>* seen)
{
  u16 pause = 20, value = start;
  for (int i = 0; i < frames; ++i)
    seen->insert(value = SliderFrame(value, &pause, true, false, limits));
  const u16 after_left = value;
  for (int i = 0; i < frames; ++i)
    seen->insert(value = SliderFrame(value, &pause, false, true, limits));
  return {after_left, value};
}
}

TEST(OrcaOnlineMenuSound, TheBalanceSliderStaysInTheMiddleInBothGames)
{
  const auto brawl = Shipped("RSBE01.patches");
  const auto pplus = Shipped("PPLUS32.patches");
  for (const u32 at : {0x8117B43Cu, 0x8117B500u})
  {
    SCOPED_TRACE(at);
    EXPECT_EQ(Values(GroupAt(brawl, at)), Values(GroupAt(pplus, at + 0x1480)));
  }
  for (const auto& [name, patches, module] :
       {std::tuple{"RSBE01.patches", brawl, 0u}, std::tuple{"PPLUS32.patches", pplus, 0x1480u}})
  {
    SCOPED_TRACE(name);
    const SliderLimits limits = SliderGroups(patches, module);
    EXPECT_EQ(limits.left, 50u);
    EXPECT_EQ(limits.right, 50);
    std::set<u16> seen;
    EXPECT_EQ(HoldBothWays(50, 600, limits, &seen), (std::pair<u16, u16>{50, 50}));
    EXPECT_EQ(seen, std::set<u16>{50});
    u16 both_pause = 0;
    EXPECT_EQ(SliderFrame(50, &both_pause, true, true, limits), 50);
    for (const u16 start : {u16{0}, u16{10}, u16{49}, u16{51}, u16{90}, u16{100}})
    {
      SCOPED_TRACE(start);
      std::set<u16> way;
      HoldBothWays(start, 600, limits, &way);
      EXPECT_GE(*way.begin(), std::min<u16>(start, 50));
      EXPECT_LE(*way.rbegin(), std::max<u16>(start, 50));
      u16 pause = 20, value = start;
      for (int i = 0; i < 600; ++i)
        value = SliderFrame(value, &pause, false, true, limits);
      EXPECT_EQ(value, std::min<u16>(start, 50));
    }
  }
  std::set<u16> seen;
  EXPECT_EQ(HoldBothWays(50, 600, SliderLimits{100, 0, false}, &seen),
            (std::pair<u16, u16>{100, 0}));
  EXPECT_EQ(HoldBothWays(50, 600, SliderLimits{0x5A, 0x0A, true}, &seen),
            (std::pair<u16, u16>{90, 10}));
}

namespace
{
std::optional<std::pair<u32, u32>> PngSize(const std::string& path)
{
  File::IOFile f(path, "rb");
  u8 head[24] = {};
  if (!f.ReadBytes(head, sizeof(head)))
    return std::nullopt;
  static constexpr u8 SIGNATURE[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  if (!std::equal(std::begin(SIGNATURE), std::end(SIGNATURE), head) ||
      std::string(reinterpret_cast<const char*>(head + 12), 4) != "IHDR")
  {
    return std::nullopt;
  }
  const auto be32 = [&](int i) {
    return u32(head[i]) << 24 | u32(head[i + 1]) << 16 | u32(head[i + 2]) << 8 | head[i + 3];
  };
  return std::pair{be32(16), be32(20)};
}

struct TextureName
{
  u32 width = 0, height = 0;
  std::string hash, tlut, format;
};

std::optional<TextureName> ParseTextureName(const std::string& stem)
{
  TextureName n;
  char hash[17] = {}, tlut[17] = {}, format[4] = {};
  int used = 0;
  const bool plain = std::sscanf(stem.c_str(), "tex1_%ux%u_%16[0-9a-f]_%3[0-9]%n", &n.width,
                                 &n.height, hash, format, &used) == 4 &&
                     used == static_cast<int>(stem.size());
  if (!plain)
  {
    used = 0;
    if (std::sscanf(stem.c_str(), "tex1_%ux%u_%16[0-9a-f]_%16[0-9a-f]_%3[0-9]%n", &n.width,
                    &n.height, hash, tlut, format, &used) != 5 ||
        used != static_cast<int>(stem.size()) || std::string(tlut).size() != 16)
    {
      return std::nullopt;
    }
    n.tlut = tlut;
  }
  if (std::string(hash).size() != 16 || n.width == 0 || n.height == 0 || n.width > 1024 ||
      n.height > 1024)
  {
    return std::nullopt;
  }
  const std::string f = format;
  if (n.tlut.empty() == (f == "8" || f == "9" || f == "10"))
    return std::nullopt;
  n.hash = hash;
  n.format = format;
  return n;
}
}

TEST(OrcaOnlineMenuTextures, TextureNamesParse)
{
  EXPECT_TRUE(ParseTextureName("tex1_127x96_fdd5334e5af7357c_2").has_value());
  EXPECT_TRUE(ParseTextureName("tex1_132x48_44a1dd712ec999f0_159b390ed5eec04d_9").has_value());
  EXPECT_FALSE(ParseTextureName("tex1_132x48_44a1dd712ec999f0_159b390ed5eec04d_5").has_value());
  EXPECT_FALSE(ParseTextureName("tex1_132x48_44a1dd712ec999f0_9").has_value());
  EXPECT_FALSE(ParseTextureName("tex1_132x48_44a1dd712ec999f0_159b390ed5eec04_9").has_value());
  for (const char* bad : {"tex1_127x96_fdd5334e5af7357_2", "tex1_127x96_m_fdd5334e5af7357c_2",
                          "tex1_127x96_fdd5334e5af7357C_2", "tex1_0x96_fdd5334e5af7357c_2",
                          "tex1_127x96_fdd5334e5af7357c_2_arb", "tex1_127x96_fdd5334e5af7357c_",
                          "tex2_127x96_fdd5334e5af7357c_2", "tex1_127x96_fdd5334e5af7357c_14x"})
  {
    EXPECT_FALSE(ParseTextureName(bad).has_value()) << bad;
  }
}

TEST(OrcaOnlineMenuTextures, EveryShippedTextureIsAnIntegerMultipleOfItsName)
{
  const std::filesystem::path root = SysSubspace() / "Textures";
  std::map<std::string, std::pair<u32, u32>> bases;
  std::map<std::string, std::map<u32, std::pair<u32, u32>>> levels;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
  {
    if (!entry.is_regular_file())
      continue;
    const std::string path = entry.path().string();
    SCOPED_TRACE(path);
    EXPECT_EQ(entry.path().extension(), ".png");
    EXPECT_EQ(entry.path().parent_path().filename(), "RSBE01");
    std::string stem = entry.path().stem().string();
    const auto size = PngSize(path);
    ASSERT_TRUE(size.has_value());
    if (const size_t mip = stem.rfind("_mip"); mip != std::string::npos)
    {
      levels[stem.substr(0, mip)][static_cast<u32>(std::stoul(stem.substr(mip + 4)))] = *size;
      continue;
    }
    const auto name = ParseTextureName(stem);
    ASSERT_TRUE(name.has_value());
    EXPECT_EQ(!name->tlut.empty(), name->format == "9" || name->format == "8");
    const auto [w, h] = *size;
    EXPECT_EQ(w % name->width, 0u);
    EXPECT_EQ(h % name->height, 0u);
    EXPECT_EQ(w / name->width, h / name->height);
    EXPECT_GE(w / name->width, 1u);
    bases[stem] = *size;
  }
  EXPECT_EQ(bases.size(), 12u + 6u + 26u + 4u + 2u - 3u);
  std::map<std::pair<u32, u32>, std::set<std::string>> titles;
  for (const auto& [stem, size] : bases)
  {
    const auto name = ParseTextureName(stem);
    if (name && !name->tlut.empty())
      titles[{name->width, name->height}].insert(name->hash);
  }
  ASSERT_EQ(titles.size(), 3u);
  EXPECT_EQ(titles[std::pair(132u, 48u)].size(), 1u);
  EXPECT_EQ(titles[std::pair(128u, 56u)].size(), 1u);
  EXPECT_EQ(titles[std::pair(352u, 36u)].size(), 2u);
  size_t marked = 0;
  for (const auto& [stem, size] : bases)
    marked += !ParseTextureName(stem)->tlut.empty();
  EXPECT_EQ(marked, 10u);
  for (const auto& [stem, size] : bases)
  {
    SCOPED_TRACE(stem);
    const auto name = ParseTextureName(stem);
    ASSERT_TRUE(name.has_value());
    auto [w, h] = size;
    u32 level = 0;
    for (const auto& [n, mip] : levels[stem])
    {
      EXPECT_EQ(n, ++level);
      w /= 2;
      h /= 2;
      EXPECT_EQ(mip, std::pair(w, h)) << "level " << n;
    }
    EXPECT_EQ(std::pair(w, h), std::pair(name->width, name->height));
  }
  EXPECT_EQ(levels.size(), bases.size());
}

TEST(OrcaOnlineMenuTextures, OrcasPackKeepsItsOwnNamesAndThePlayersPackTheRest)
{
  const std::string root = File::CreateTempDir();
  ASSERT_FALSE(root.empty());
  Common::ScopeGuard remove_root([&] { File::DeleteDirRecursively(root); });
  const std::string player = root + "/Load/Textures/RSBE01";
  const std::string player2 = root + "/Load/Textures/RSBE01-more";
  const std::string title = "tex1_120x16_a56825159146a723_0";
  const std::string button = "tex1_127x96_fdd5334e5af7357c_2";
  const std::string other = "tex1_64x64_0123456789abcdef_5";
  for (const std::string& path :
       {player + "/" + title + ".png", player + "/" + title + "_mip1.png",
        player + "/hd/" + button + "_arb.png", player + "/" + other + ".png",
        player + "/" + other + "_mip1.png", player + "/notes.png", player2 + "/" + other + ".dds"})
  {
    ASSERT_TRUE(File::CreateFullPath(path));
    ASSERT_TRUE(File::WriteStringToFile(path, "not decoded here"));
  }
  const std::string orca = (SysSubspace() / "Textures" / "RSBE01").lexically_normal().string();
  const auto by_id = [](const std::vector<HiresTextureFile>& files) {
    std::map<std::string, HiresTextureFile> out;
    for (const HiresTextureFile& f : files)
      EXPECT_TRUE(out.emplace(f.id, f).second) << "two files for " << f.id;
    return out;
  };
  const auto under = [](const std::string& path, const std::string& directory) {
    return std::filesystem::path(path).lexically_normal().string().starts_with(
        std::filesystem::path(directory + "/").lexically_normal().string());
  };

  {
    const auto files = by_id(CollectHiresTextureFiles({player, player2}, {orca}));
    ASSERT_EQ(files.size(), 51u);
    for (const auto& [id, f] : files)
    {
      SCOPED_TRACE(id);
      EXPECT_EQ(f.orca_pack, id != other);
      EXPECT_TRUE(under(f.path, f.orca_pack ? orca : player));
      EXPECT_FALSE(f.has_arbitrary_mipmaps);
    }
    EXPECT_TRUE(files.contains(title) && files.contains(button));
    EXPECT_EQ(std::filesystem::path(files.at(other).path).extension(), ".png");
  }
  {
    const auto files = by_id(CollectHiresTextureFiles({player, player2}, {}));
    ASSERT_EQ(files.size(), 3u);
    for (const auto& [id, f] : files)
    {
      EXPECT_FALSE(f.orca_pack);
      EXPECT_TRUE(under(f.path, player)) << f.path;
    }
    EXPECT_TRUE(files.at(button).has_arbitrary_mipmaps);
    EXPECT_FALSE(files.at(title).has_arbitrary_mipmaps);
  }
  EXPECT_EQ(CollectHiresTextureFiles({}, {orca}).size(), 50u);
}

namespace
{
using Where = MenuState::Where;
MenuState Menu()
{
  return {Where::Menu, 0};
}
MenuState Leaving(u32 code)
{
  return {Where::Leaving, code};
}
MenuState Other()
{
  return {Where::Other, 0};
}

struct Feed
{
  OnlineMenuReader reader;
  u64 resyncs = 0;
  std::vector<OnlinePick> picks;
  void Step(const MenuState& s, bool alone = true, bool resimulating = false)
  {
    if (const auto pick = reader.Boundary(s, resimulating, alone, resyncs))
      picks.push_back(*pick);
  }
};
}

TEST(OrcaOnlineMenuReader, EachPickIsAnnouncedOnceAsTheMenuLeaves)
{
  for (const auto& [code, pick] :
       std::vector<std::pair<u32, OnlinePick>>{{30, OnlinePick::Casual},
                                               {31, OnlinePick::Ranked},
                                               {24, OnlinePick::Friends},
                                               {25, OnlinePick::Friends},
                                               {26, OnlinePick::Friends},
                                               {27, OnlinePick::Friends}})
  {
    SCOPED_TRACE(code);
    Feed r;
    r.Step(Other());
    r.Step(Menu());
    r.Step(Menu());
    EXPECT_TRUE(r.picks.empty());
    r.Step(Leaving(code));
    r.Step(Leaving(code));
    r.Step(Leaving(code));
    r.Step(Other());
    r.Step(Other());
    EXPECT_EQ(r.picks, std::vector<OnlinePick>{pick});
  }
  EXPECT_EQ(MenuEventText(OnlinePick::Casual), "online casual local");
  EXPECT_EQ(MenuEventText(OnlinePick::Ranked), "online ranked local");
  EXPECT_EQ(MenuEventText(OnlinePick::Friends), "online friends");
}

TEST(OrcaOnlineMenuReader, OnlyOnlineExitsFromTheMenu)
{
  Feed r;
  r.Step(Menu());
  r.Step(Leaving(0));
  r.Step(Leaving(30));
  r.Step(Other());
  ASSERT_EQ(r.picks, std::vector<OnlinePick>{OnlinePick::Casual});
  r.picks.clear();
  for (const u32 code : {1u, 2u, 23u, 28u, 29u, 32u, 0xFFFFFFFFu})
  {
    r.Step(Menu());
    r.Step(Leaving(code));
    r.Step(Other());
  }
  r.Step(Other());
  r.Step(Leaving(30));
  r.Step(Other());
  EXPECT_TRUE(r.picks.empty());
  r.Step(Menu());
  r.Step(Leaving(31));
  EXPECT_EQ(r.picks, std::vector<OnlinePick>{OnlinePick::Ranked});
}

TEST(OrcaOnlineMenuReader, NeverOnReRunFrames)
{
  Feed r;
  r.Step(Menu());
  r.Step(Leaving(30), true, true);
  EXPECT_TRUE(r.picks.empty());
  r.Step(Other(), true, true);
  r.Step(Leaving(30));
  EXPECT_EQ(r.picks, std::vector<OnlinePick>{OnlinePick::Casual});
  r.Step(Leaving(30), true, true);
  r.Step(Leaving(30));
  EXPECT_EQ(r.picks.size(), 1u);
}

TEST(OrcaOnlineMenuReader, APickMadeWhileNotAloneIsNeverAnnouncedLater)
{
  Feed r;
  r.Step(Menu(), false);
  r.Step(Leaving(30), false);
  r.Step(Leaving(30), true);
  r.Step(Leaving(30), true);
  r.Step(Other(), true);
  EXPECT_TRUE(r.picks.empty());
  r.Step(Menu());
  r.Step(Leaving(25));
  EXPECT_EQ(r.picks, std::vector<OnlinePick>{OnlinePick::Friends});
}

TEST(OrcaOnlineMenuReader, APickMadeWithFriendsGoesToTheSessionOnce)
{
  Feed r;
  r.Step(Menu(), false);
  EXPECT_FALSE(r.reader.TakeBusyPick());
  r.Step(Leaving(31), false);
  EXPECT_TRUE(r.picks.empty());
  EXPECT_EQ(r.reader.TakeBusyPick(), OnlinePick::Ranked);
  EXPECT_FALSE(r.reader.TakeBusyPick());
  r.Step(Leaving(31), false);
  EXPECT_FALSE(r.reader.TakeBusyPick());
  r.Step(Leaving(31), true);
  EXPECT_FALSE(r.reader.TakeBusyPick());
  r.Step(Other());
  EXPECT_TRUE(r.picks.empty());
  r.Step(Menu(), false);
  r.Step(Leaving(30), false);
  r.Step(Other(), false);
  EXPECT_FALSE(r.reader.TakeBusyPick());
  r.Step(Menu(), false);
  r.Step(Leaving(30), false, true);
  EXPECT_FALSE(r.reader.TakeBusyPick());
  r.Step(Leaving(25), false);
  EXPECT_EQ(r.reader.TakeBusyPick(), OnlinePick::Friends);
  r.Step(Menu());
  r.Step(Leaving(30));
  EXPECT_EQ(r.picks, std::vector<OnlinePick>{OnlinePick::Casual});
  EXPECT_FALSE(r.reader.TakeBusyPick());
}

TEST(OrcaOnlineMenuReader, AloneMeansNoFriendIsAboutToLand)
{
  using Rollback::OnlineMatch::AloneAt;
  using Rollback::OnlineMatch::KEYFRAME_FRESH_FRAMES;
  EXPECT_EQ(KEYFRAME_FRESH_FRAMES, 30 * 60);
  EXPECT_TRUE(AloneAt(5000, true, false, std::nullopt));
  EXPECT_FALSE(AloneAt(5000, false, false, std::nullopt));
  EXPECT_FALSE(AloneAt(5000, true, true, std::nullopt));
  EXPECT_FALSE(AloneAt(5000, true, false, 5000));
  EXPECT_FALSE(AloneAt(5000 + KEYFRAME_FRESH_FRAMES, true, false, 5000));
  EXPECT_TRUE(AloneAt(5000 + KEYFRAME_FRESH_FRAMES + 1, true, false, 5000));
  EXPECT_FALSE(AloneAt(5000 + KEYFRAME_FRESH_FRAMES + 1, true, true, 5000));
}

TEST(OrcaOnlineMenuReader, APickWhileAFriendIsAboutToLandIsNeverAnnounced)
{
  using Rollback::OnlineMatch::AloneAt;
  using Rollback::OnlineMatch::KEYFRAME_FRESH_FRAMES;
  {
    Feed r;
    const std::optional<int> invite = 1000;
    int frame = 1600;
    const auto step = [&](const MenuState& s) {
      r.Step(s, AloneAt(frame, true, false, invite));
      frame += 50;
    };
    step(Menu());
    step(Leaving(30));
    step(Leaving(30));
    frame = 1000 + KEYFRAME_FRESH_FRAMES + 1;
    step(Leaving(30));
    step(Other());
    EXPECT_TRUE(r.picks.empty());
    step(Menu());
    step(Leaving(31));
    EXPECT_EQ(r.picks, std::vector<OnlinePick>{OnlinePick::Ranked});
  }
  {
    Feed r;
    r.Step(Menu(), AloneAt(2000, true, false, std::nullopt));
    r.Step(Leaving(24), AloneAt(2001, true, true, std::nullopt));
    r.Step(Leaving(24), AloneAt(2002, false, false, std::nullopt));
    r.Step(Other(), AloneAt(2003, false, false, std::nullopt));
    EXPECT_TRUE(r.picks.empty());
  }
}

TEST(OrcaOnlineMenuReader, AResyncStartsOverWithoutAnnouncing)
{
  {
    Feed r;
    r.Step(Menu(), false);
    ++r.resyncs;
    r.Step(Leaving(30));
    r.Step(Leaving(30));
    r.Step(Other());
    EXPECT_TRUE(r.picks.empty());
  }
  {
    Feed r;
    r.Step(Other(), false);
    ++r.resyncs;
    r.Step(Menu(), false);
    r.Step(Leaving(30), false);
    EXPECT_TRUE(r.picks.empty());
    ++r.resyncs;
    r.Step(Other());
    r.Step(Menu());
    r.Step(Leaving(31));
    EXPECT_EQ(r.picks, std::vector<OnlinePick>{OnlinePick::Ranked});
  }
  {
    Feed r;
    r.Step(Other());
    ++r.resyncs;
    r.Step(Menu());
    r.Step(Leaving(24));
    EXPECT_EQ(r.picks, std::vector<OnlinePick>{OnlinePick::Friends});
  }
  {
    Feed r;
    r.Step(Leaving(30));
    r.Step(Other());
    EXPECT_TRUE(r.picks.empty());
  }
}

namespace
{
class FakeMemory final : public GuestMemory
{
public:
  bool Valid(u32 a) const override { return bytes.contains(a); }
  u8 Read8(u32 a) const override { return bytes.at(a); }
  u16 Read16(u32 a) const override { return static_cast<u16>(Read8(a) << 8 | Read8(a + 1)); }
  u32 Read32(u32 a) const override { return u32(Read16(a)) << 16 | Read16(a + 2); }
  void Write8(u32 a, u8 v) override
  {
    bytes[a] = v;
    if (track_writes)
      recorded.push_back(a);
    ++writes;
  }
  void Write16(u32 a, u16 v) override
  {
    Write8(a, static_cast<u8>(v >> 8));
    Write8(a + 1, static_cast<u8>(v));
  }
  void Write32(u32 a, u32 v) override
  {
    Write16(a, static_cast<u16>(v >> 16));
    Write16(a + 2, static_cast<u16>(v));
  }
  void Fill(u32 a, u32 n)
  {
    for (u32 i = 0; i < n; ++i)
      bytes[a + i] = 0;
  }
  void Text(u32 a, std::string_view s)
  {
    for (size_t i = 0; i < s.size(); ++i)
      bytes[a + static_cast<u32>(i)] = static_cast<u8>(s[i]);
    bytes[a + static_cast<u32>(s.size())] = 0;
  }
  bool track_writes = false;
  std::vector<u32> recorded;
  std::map<u32, u8> bytes;
  int writes = 0;
};

FakeMemory Scene(std::string_view name, u32 exit_code)
{
  FakeMemory m;
  m.Fill(0x805A0060, 4);
  m.Fill(0x80900000, 0x300);
  m.Fill(0x80910000, 4);
  m.Write32(0x805A0060, 0x80900000);
  m.Write32(0x80900004, 0x80910000);
  m.Write32(0x80910000, 0x80920000);
  m.Text(0x80920000, name);
  m.Write32(0x80900284, exit_code);
  m.writes = 0;
  return m;
}
}

TEST(OrcaOnlineMenuReader, ReadsTheSceneAndTheExitCode)
{
  {
    const FakeMemory m = Scene("muMenuMain", 0);
    const MenuState s = ReadMenuState(m);
    EXPECT_EQ(s.where, Where::Menu);
    EXPECT_EQ(s.exit_code, 0u);
  }
  {
    const FakeMemory m = Scene("scMemoryChange", 30);
    const MenuState s = ReadMenuState(m);
    EXPECT_EQ(s.where, Where::Leaving);
    EXPECT_EQ(s.exit_code, 30u);
  }
  {
    const FakeMemory m = Scene("scSelctCharacter", 30);
    EXPECT_EQ(ReadMenuState(m).where, Where::Other);
    EXPECT_EQ(ReadMenuState(m).exit_code, 0u);
  }
  {
    FakeMemory m;
    EXPECT_EQ(ReadMenuState(m).where, Where::Other);
    FakeMemory n = Scene("muMenuMain", 0);
    for (u32 i = 0; i < 40; ++i)
      n.bytes[0x80920000 + i] = 'x';
    EXPECT_EQ(ReadMenuState(n).where, Where::Other);
    FakeMemory p = Scene("muMenuMain", 0);
    p.Write32(0x80900004, 0x80910001);
    EXPECT_EQ(ReadMenuState(p).where, Where::Other);
  }
  FakeMemory m = Scene("scMemoryChange", 30);
  (void)ReadMenuState(m);
  EXPECT_EQ(m.writes, 0);
}

namespace
{
constexpr u32 ARCHIVE_OBJECT = 0x81500000;
constexpr u32 IMAGE = 0x92000000;

struct ArcFile
{
  u16 index;
  std::vector<std::string> messages;
};

std::map<u16, u32> WriteArchive(FakeMemory& m, const std::vector<ArcFile>& files,
                                std::string_view name = "mu_menumain_en")
{
  m.Fill(IMAGE, 0x40);
  m.Write32(IMAGE, 0x41524300);
  m.Write16(IMAGE + 6, static_cast<u16>(files.size() + 1));
  m.Text(IMAGE + 0x10, name);
  std::map<u16, u32> where;
  u32 entry = IMAGE + 0x40;
  auto add = [&](u16 index, const std::vector<u8>& data) {
    m.Fill(entry, 0x20);
    m.Write16(entry, 1);
    m.Write16(entry + 2, index);
    m.Write32(entry + 4, static_cast<u32>(data.size()));
    const u32 at = entry + 0x20;
    for (size_t i = 0; i < data.size(); ++i)
      m.bytes[at + static_cast<u32>(i)] = data[i];
    where[index] = at;
    entry = (at + static_cast<u32>(data.size()) + 31) & ~31u;
  };
  add(100, std::vector<u8>(0x40, 0xAA));
  for (const ArcFile& f : files)
  {
    std::vector<u8> data(4 * (f.messages.size() + 1), 0);
    u32 offset = static_cast<u32>(data.size());
    for (size_t k = 0; k <= f.messages.size(); ++k)
    {
      for (int b = 0; b < 4; ++b)
        data[4 * k + b] = static_cast<u8>(offset >> (24 - 8 * b));
      if (k < f.messages.size())
        offset += static_cast<u32>(f.messages[k].size());
    }
    for (const std::string& text : f.messages)
      data.insert(data.end(), text.begin(), text.end());
    add(f.index, data);
  }
  return where;
}

FakeMemory MainMenu()
{
  FakeMemory m = Scene("muMenuMain", 0);
  m.Fill(0x80910000, 0xAC4);
  m.Write32(0x80910000, 0x80920000);
  m.Write32(0x80910000 + 0xAC0, ARCHIVE_OBJECT);
  m.Fill(ARCHIVE_OBJECT, 0x80);
  m.Write32(ARCHIVE_OBJECT + 4, 0x92000004);
  m.Write32(ARCHIVE_OBJECT + 8, IMAGE);
  m.writes = 0;
  return m;
}

const MenuTextEdit& Edit(u32 file, u32 message, std::string_view prefix)
{
  for (const MenuTextEdit& e : MenuTextEdits())
  {
    if (e.file == file && e.message == message &&
        e.original.substr(0, std::min(prefix.size(), e.original.size())) == prefix)
    {
      return e;
    }
  }
  ADD_FAILURE() << file << "." << message;
  return MenuTextEdits()[0];
}

std::string Bytes(const FakeMemory& m, u32 at, size_t n)
{
  std::string out;
  for (size_t i = 0; i < n; ++i)
    out.push_back(static_cast<char>(m.Read8(at + static_cast<u32>(i))));
  return out;
}

std::vector<std::string> Placeholders(size_t n)
{
  std::vector<std::string> out;
  for (size_t i = 0; i < n; ++i)
    out.push_back("x" + std::to_string(i) + std::string(1, '\x13'));
  return out;
}

constexpr char BRAWL_COLOUR[] = "\x12\x02\x0c"
                                "333\xff";
constexpr char PPLUS_COLOUR[] = "\x12\x02\x0c\xbf\xc7\xc4\xff";

constexpr const char* TAKEN_OUT[] = {
    "Nintendo WFC",   "(Discontinued)",   "(Project M not compatible!)",
    "(No names)",     "2-on-2",           "Register",
    "registered",     "defeat enemies",   "Home-Run Contest",
    "10 bosses",      "(2 players)"};
}

TEST(OrcaOnlineMenuText, EveryEditFitsItsSlotAndSaysNothingOfNintendo)
{
  ASSERT_FALSE(MenuTextEdits().empty());
  for (const MenuTextEdit& e : MenuTextEdits())
  {
    SCOPED_TRACE(std::string(e.text));
    EXPECT_LE(e.text.size(), e.original.size());
    EXPECT_NE(e.text, e.original);
    EXPECT_EQ(e.text.substr(0, 7), e.original.substr(0, 7));
    EXPECT_EQ(e.text.back(), '\x13');
    for (size_t i = 7; i + 1 < e.text.size(); ++i)
      EXPECT_GE(static_cast<u8>(e.text[i]), 0x20) << i;
    for (const char* word : {"Nintendo", "WFC", "Wiimmfi", "Wi-Fi", "Discontinued", "Project M"})
      EXPECT_EQ(e.text.find(word), std::string_view::npos) << word;
    const bool names_it =
        std::any_of(std::begin(TAKEN_OUT), std::end(TAKEN_OUT), [&](const char* what) {
          return e.original.find(what) != std::string_view::npos;
        });
    EXPECT_TRUE(names_it);
    for (const char* what : TAKEN_OUT)
      EXPECT_EQ(e.text.find(what), std::string_view::npos) << what;
  }
}

TEST(OrcaOnlineMenuText, TheOnlinePagesWithFriendsPromisesNoRoster)
{
  for (const char* colour : {BRAWL_COLOUR, PPLUS_COLOUR})
  {
    SCOPED_TRACE(colour == BRAWL_COLOUR ? "Brawl" : "Project+");
    const MenuTextEdit& e = Edit(5, 18, colour);
    EXPECT_EQ(e.original,
              std::string(colour) + "Play with people you've registered as Friends!\x13");
    EXPECT_EQ(e.text, std::string(colour) + "Play with friends you invite! (2-4 players)\x13");
    std::vector<std::string> five = Placeholders(50);
    five[18] = std::string(e.original);
    FakeMemory m = MainMenu();
    const auto where = WriteArchive(m, {{5, five}});
    EXPECT_EQ(ApplyMenuText(m, MenuTextEdits()), 1);
    const u32 file = where.at(5);
    EXPECT_EQ(Bytes(m, file + m.Read32(file + 4 * 18), e.text.size()), e.text);
  }
}

TEST(OrcaOnlineMenuText, WithFriendsSaysWhereEachButtonGoes)
{
  for (const char* colour : {BRAWL_COLOUR, PPLUS_COLOUR})
  {
    SCOPED_TRACE(colour == BRAWL_COLOUR ? "Brawl" : "Project+");
    std::vector<std::string> five = Placeholders(50);
    five[46] = std::string(colour) + "Take on your friends! (2-4 players)\x13";
    for (const u32 k : {45u, 47u, 48u, 49u})
    {
      const MenuTextEdit& e = Edit(5, k, colour);
      ASSERT_EQ(e.original.substr(0, 7), colour);
      five[k] = std::string(e.original);
      std::string lower(e.text);
      std::transform(lower.begin(), lower.end(), lower.begin(),
                     [](char c) { return static_cast<char>(std::tolower(static_cast<u8>(c))); });
      EXPECT_NE(lower.find("friends"), std::string::npos) << k;
      EXPECT_NE(e.text.find("(2-4 players)"), std::string_view::npos) << k;
      EXPECT_NE(lower.find(colour == BRAWL_COLOUR ? "brawl" : "fight"), std::string::npos) << k;
    }
    FakeMemory m = MainMenu();
    const auto where = WriteArchive(m, {{5, five}});
    EXPECT_EQ(ApplyMenuText(m, MenuTextEdits()), 4);
    const u32 file = where.at(5);
    for (const u32 k : {45u, 47u, 48u, 49u})
    {
      const MenuTextEdit& e = Edit(5, k, colour);
      EXPECT_EQ(Bytes(m, file + m.Read32(file + 4 * k), e.text.size()), e.text) << k;
    }
    EXPECT_EQ(Bytes(m, file + m.Read32(file + 4 * 46), five[46].size()), five[46]);
  }
}

namespace
{
constexpr u32 PANEL = 0x81510000, WINDOW = 0x81511000, TEXT = 0x81512000, MSG = 0x81513000;
constexpr u32 BUFFER = 0x81514000;

void WriteDescription(FakeMemory& m, u32 messages, u32 shown, const std::string& formatted)
{
  m.Write32(0x80910000 + 0x7BC, PANEL);
  m.Fill(PANEL, 0x80);
  m.Write32(PANEL + 0x40, shown);
  m.Write32(PANEL + 0x48, WINDOW);
  m.Write32(PANEL + 0x5C, 0x92000040);
  m.Fill(WINDOW, 0x10);
  m.Write32(WINDOW + 0x4, messages);
  m.Write32(WINDOW + 0x8, TEXT);
  m.Fill(TEXT, 0xA0);
  m.Write32(TEXT + 0x9C, MSG);
  m.Fill(MSG, 0x60);
  m.Write32(MSG + 0x48, 0x200);
  m.Write32(MSG + 0x4C, static_cast<u32>(formatted.size()));
  m.Write32(MSG + 0x50, BUFFER);
  m.Fill(BUFFER, 0x200);
  for (size_t i = 0; i < formatted.size(); ++i)
    m.bytes[BUFFER + static_cast<u32>(i)] = static_cast<u8>(formatted[i]);
  m.bytes[BUFFER + static_cast<u32>(formatted.size())] = 0x01;
  m.writes = 0;
}

const std::string BOX_PREFIX("\x17\xfe}\x00\x0c\x01\x83\xff\xf4\x18\x01\x03\x18\x10"
                             "0\x1d \x1c\x00\xcc\r@@\x0f\xff\x00\x05\x01\x02\x08\x0c/..\xff"
                             "\x04/..\xff\x03\x00\x00",
                             0x2B);
}

TEST(OrcaOnlineMenuText, RewritesTheDescriptionBoxsCopy)
{
  for (const char* colour : {BRAWL_COLOUR, PPLUS_COLOUR})
  {
    SCOPED_TRACE(colour == BRAWL_COLOUR ? "Brawl" : "Project+");
    const MenuTextEdit& ranked = Edit(5, 52, colour);
    std::vector<std::string> five = Placeholders(53);
    five[52] = std::string(ranked.original);
    FakeMemory m = MainMenu();
    const auto where = WriteArchive(m, {{5, five}});
    const std::string formatted = BOX_PREFIX + std::string(ranked.original);
    WriteDescription(m, where.at(5), 52, formatted);
    const MenuDescription d = FindMenuDescription(m, where.at(5));
    EXPECT_EQ(d.buffer, BUFFER);
    EXPECT_EQ(d.length, formatted.size());
    EXPECT_EQ(d.message, 52u);

    int in_box = -1;
    EXPECT_EQ(ApplyMenuText(m, MenuTextEdits(), &in_box), 2);
    EXPECT_EQ(in_box, 1);
    const u32 at = BUFFER + static_cast<u32>(BOX_PREFIX.size());
    EXPECT_EQ(Bytes(m, BUFFER, BOX_PREFIX.size()), BOX_PREFIX);
    EXPECT_EQ(Bytes(m, at, ranked.text.size()), ranked.text);
    for (size_t i = ranked.text.size(); i < ranked.original.size(); ++i)
      EXPECT_EQ(m.Read8(at + static_cast<u32>(i)), 0) << i;
    EXPECT_EQ(m.Read8(BUFFER + static_cast<u32>(formatted.size())), 0x01);
    EXPECT_EQ(m.Read32(MSG + 0x4C), formatted.size());
    const u32 file = where.at(5);
    EXPECT_EQ(Bytes(m, at, ranked.original.size()),
              Bytes(m, file + m.Read32(file + 4 * 52), ranked.original.size()));

    m.writes = 0;
    EXPECT_EQ(ApplyMenuText(m, MenuTextEdits()), 0);
    EXPECT_EQ(m.writes, 0);
  }
}

TEST(OrcaOnlineMenuText, LeavesTheBoxAloneUnlessItShowsThatMessage)
{
  const MenuTextEdit& ranked = Edit(5, 52, BRAWL_COLOUR);
  const MenuTextEdit& casual = Edit(5, 51, BRAWL_COLOUR);
  std::vector<std::string> five = Placeholders(53);
  five[51] = std::string(casual.original);
  five[52] = std::string(ranked.original);
  const std::string formatted = BOX_PREFIX + std::string(ranked.original);
  {
    FakeMemory m = MainMenu();
    const auto where = WriteArchive(m, {{5, five}});
    WriteDescription(m, where.at(5), 51, formatted);
    EXPECT_EQ(ApplyMenuDescription(m, where.at(5), MenuTextEdits()), 0);
    EXPECT_EQ(m.writes, 0);
  }
  {
    FakeMemory m = MainMenu();
    const auto where = WriteArchive(m, {{5, five}});
    WriteDescription(m, where.at(5) + 0x20, 52, formatted);
    EXPECT_EQ(FindMenuDescription(m, where.at(5)).buffer, 0u);
    EXPECT_EQ(ApplyMenuDescription(m, where.at(5), MenuTextEdits()), 0);
  }
  {
    FakeMemory m = MainMenu();
    const auto where = WriteArchive(m, {{5, five}});
    WriteDescription(m, where.at(5), 52, formatted);
    m.Write32(MSG + 0x4C, 0x201);
    EXPECT_EQ(FindMenuDescription(m, where.at(5)).buffer, 0u);
    WriteDescription(m, where.at(5), 52, formatted);
    m.Write32(WINDOW + 0x8, TEXT + 2);
    EXPECT_EQ(FindMenuDescription(m, where.at(5)).buffer, 0u);
    m.writes = 0;
    EXPECT_EQ(ApplyMenuDescription(m, where.at(5), MenuTextEdits()), 0);
    EXPECT_EQ(m.writes, 0);
  }
  {
    FakeMemory m = MainMenu();
    const auto where = WriteArchive(m, {{5, five}});
    std::string rewritten(ranked.text);
    rewritten.resize(ranked.original.size(), '\0');
    WriteDescription(m, where.at(5), 52, BOX_PREFIX + rewritten);
    EXPECT_EQ(ApplyMenuDescription(m, where.at(5), MenuTextEdits()), 0);
    EXPECT_EQ(m.writes, 0);
  }
}

TEST(OrcaOnlineMenuText, RewritesTheGamesOwnWordsInPlace)
{
  const MenuTextEdit& footer = Edit(5, 2, BRAWL_COLOUR);
  const MenuTextEdit& record = Edit(10, 48, "");
  std::vector<std::string> five = Placeholders(80);
  five[1] = std::string(BRAWL_COLOUR) + "Play this mode solo or challenge it cooperatively.\x13";
  five[2] = std::string(footer.original);
  std::vector<std::string> ten = Placeholders(50);
  ten[48] = std::string(record.original);
  FakeMemory m = MainMenu();
  const auto where = WriteArchive(m, {{5, five}, {9, Placeholders(3)}, {10, ten}});
  const u32 file5 = where.at(5), file10 = where.at(10);
  ASSERT_EQ(FindMenuArchive(m), IMAGE);
  EXPECT_EQ(FindArchiveFile(m, IMAGE, 5), file5);
  EXPECT_EQ(FindArchiveFile(m, IMAGE, 10), file10);
  EXPECT_EQ(FindArchiveFile(m, IMAGE, 11), 0u);
  const FakeMemory before = m;

  EXPECT_EQ(ApplyMenuText(m, MenuTextEdits()), 2);
  const u32 at = file5 + m.Read32(file5 + 4 * 2);
  EXPECT_EQ(m.Read32(file5 + 4 * 3) - m.Read32(file5 + 4 * 2), footer.original.size());
  EXPECT_EQ(Bytes(m, at, footer.text.size()), footer.text);
  for (size_t i = footer.text.size(); i < footer.original.size(); ++i)
    EXPECT_EQ(m.Read8(at + static_cast<u32>(i)), 0) << i;
  const u32 at10 = file10 + m.Read32(file10 + 4 * 48);
  EXPECT_EQ(Bytes(m, at10, record.text.size()), record.text);
  size_t changed = 0;
  for (const auto& [address, value] : before.bytes)
    changed += m.bytes.at(address) != value;
  EXPECT_LE(changed, footer.original.size() + record.original.size());
  EXPECT_EQ(Bytes(m, file5 + m.Read32(file5 + 4), five[1].size()), five[1]);

  m.writes = 0;
  EXPECT_EQ(ApplyMenuText(m, MenuTextEdits()), 0);
  EXPECT_EQ(m.writes, 0);
}

TEST(OrcaOnlineMenuText, WritesNothingUnlessItIsTheMenusOwnText)
{
  const MenuTextEdit& footer = Edit(5, 2, BRAWL_COLOUR);
  std::vector<std::string> five = Placeholders(4);
  five[2] = std::string(footer.original);
  {
    std::vector<std::string> other = five;
    other[2][10] ^= 0x20;
    FakeMemory m = MainMenu();
    WriteArchive(m, {{5, other}});
    m.writes = 0;
    EXPECT_EQ(ApplyMenuText(m, MenuTextEdits()), 0);
    EXPECT_EQ(m.writes, 0);
  }
  {
    std::vector<std::string> longer = five;
    longer[2] += '\0';
    FakeMemory m = MainMenu();
    WriteArchive(m, {{5, longer}});
    EXPECT_EQ(ApplyMenuText(m, MenuTextEdits()), 0);
  }
  {
    FakeMemory m = MainMenu();
    WriteArchive(m, {{5, five}}, "mu_menumain_jp");
    EXPECT_EQ(FindMenuArchive(m), 0u);
    EXPECT_EQ(ApplyMenuText(m, MenuTextEdits()), 0);
    FakeMemory n = MainMenu();
    EXPECT_EQ(ApplyMenuText(n, MenuTextEdits()), 0);
  }
  {
    FakeMemory m = MainMenu();
    WriteArchive(m, {{5, five}});
    m.Text(0x80920000, "scSelctCharacter");
    m.writes = 0;
    EXPECT_EQ(FindMenuArchive(m), 0u);
    EXPECT_EQ(ApplyMenuText(m, MenuTextEdits()), 0);
    EXPECT_EQ(m.writes, 0);
  }
  {
    FakeMemory m = MainMenu();
    const auto where = WriteArchive(m, {{5, five}});
    m.Write32(where.at(5) + 4 * 3, 0x7FFFFFF0);
    m.writes = 0;
    EXPECT_EQ(ApplyMenuText(m, MenuTextEdits()), 0);
    EXPECT_EQ(m.writes, 0);
    FakeMemory n = MainMenu();
    WriteArchive(n, {{5, five}});
    n.Write32(IMAGE + 0x40 + 4, 0xFFFFFF00);
    EXPECT_EQ(FindArchiveFile(n, IMAGE, 5), 0u);
  }
}

TEST(OrcaOnlineMenuBoot, BothGamesBootToTheMainMenu)
{
  const auto brawl = Shipped("RSBE01.patches");
  const auto pplus = Shipped("PPLUS32.patches");
  for (const u32 at : {0x806CA1F8u, 0x806CA204u})
  {
    SCOPED_TRACE(at);
    const auto b = GroupAt(brawl, at);
    const auto p = GroupAt(pplus, at);
    ASSERT_EQ(b.size(), 1u);
    EXPECT_EQ(Originals(b), Originals(p));
    EXPECT_EQ(Values(b), Values(p));
    EXPECT_EQ(b[0].value, 0x38000010u);
  }
  const auto boot = GroupAt(pplus, 0x80559C20);
  ASSERT_EQ(boot.size(), 4u);
  EXPECT_EQ(Originals(boot), (std::vector<u32>{0x41A0FF84, 0x38951B54, 0x38A00000, 0x48000038}));
  EXPECT_EQ(Values(boot), (std::vector<u32>{0x41A0FF84, 0x38951C94, 0x38A00014, 0x48000038}));
  EXPECT_FALSE(AnyLineIn(brawl, 0x80550000, 0x80570000));
}

TEST(OrcaOnlineMenuDropIn, SingleTrackModesHoldAFriendsJoin)
{
  for (const char* held : {"sqSingleSimple", "sqSingleAllstar", "sqSingleBoss", "sqEvent",
                           "sqTraining", "sqHomerun", "sqTargetBreak", "sqKumite", "sqAdventure",
                           "sqCoinShooter", "sqEdit", "sqReplay", "sqTyFigDisp"})
  {
    EXPECT_TRUE(SequenceHoldsDropIn(held)) << held;
  }
  for (const char* open : {"sqMenuMain", "sqVsMelee", "sqSpMelee", "sqToMelee", "sqQuMelee",
                           "sqPrizeCheck", "sqTitle", "sqBoot", "sqButton", ""})
  {
    EXPECT_FALSE(SequenceHoldsDropIn(open)) << open;
  }
}

namespace
{
using FriendsMove::Decide;
using FriendsMove::Inputs;
using FriendsMove::Outputs;

Inputs OnMenu(int frame)
{
  Inputs in;
  in.present = true;
  in.plugged = 0x01;
  in.scene = "muMenuMain";
  in.sequence = "sqMenuMain";
  in.step = FriendsMove::STEP_RUNNING;
  in.menu_built = true;
  in.frame = frame;
  in.tag = 0xF5;
  in.seen = 0x01;
  return in;
}

Inputs After(Inputs in, const Outputs& out, int frames = 1)
{
  if (!out.keep)
  {
    in.frame += frames;
    return in;
  }
  in.tag = 0xF5;
  in.seen = out.seen;
  in.pending = out.pending;
  in.menu_since = out.menu_since;
  if (out.move)
  {
    in.exit_code = FriendsMove::EXIT_WITH_FRIENDS;
    in.step = FriendsMove::STEP_EXIT;
  }
  in.frame += frames;
  return in;
}

void ExpectIdempotent(const Inputs& in, const Outputs& out)
{
  Inputs again = After(in, out, 0);
  const Outputs second = Decide(again);
  EXPECT_EQ(second.keep, out.keep);
  EXPECT_EQ(second.seen, out.seen);
  EXPECT_EQ(second.pending, out.pending);
  EXPECT_EQ(second.menu_since, out.menu_since);
  EXPECT_FALSE(second.move);
}
}

namespace
{
int RunUntilMove(Inputs& in, int limit)
{
  for (int i = 0; i < limit; ++i)
  {
    const Outputs out = Decide(in);
    ExpectIdempotent(in, out);
    const int frame = in.frame;
    in = After(in, out);
    if (out.move)
      return frame;
  }
  return -1;
}
}

TEST(OrcaOnlineMenuDropIn, AFriendOnTheMainMenuTakesTheHostToWithFriends)
{
  Inputs in = OnMenu(1000);
  Outputs out = Decide(in);
  EXPECT_TRUE(out.keep);
  EXPECT_EQ(out.menu_since, 0u);
  EXPECT_FALSE(out.move);
  ExpectIdempotent(in, out);
  in = After(in, out, 100);
  in.plugged = 0x03;
  out = Decide(in);
  EXPECT_EQ(out.pending, 1);
  EXPECT_EQ(out.seen, 0x03);
  EXPECT_EQ(out.menu_since, 1101u);
  EXPECT_FALSE(out.move);
  ExpectIdempotent(in, out);
  in = After(in, out);
  EXPECT_EQ(RunUntilMove(in, 100), 1100 + static_cast<int>(FriendsMove::MENU_SETTLE_FRAMES));
  in.scene = "scMemoryChange";
  out = Decide(in);
  EXPECT_FALSE(out.move);
  EXPECT_EQ(out.pending, 0);
  in = After(in, out);
  in.scene = "scSelctCharacter";
  in.sequence = "sqVsMelee";
  in.step = FriendsMove::STEP_RUNNING;
  in.exit_code = 0;
  out = Decide(in);
  EXPECT_FALSE(out.move);
  in = After(in, out, 600);
  in.scene = "muMenuMain";
  in.sequence = "sqMenuMain";
  EXPECT_EQ(RunUntilMove(in, 120), -1);
}

TEST(OrcaOnlineMenuDropIn, AFriendWhoPluggedInDuringTheBootWaitsForTheMenu)
{
  Inputs in = OnMenu(1500);
  in.tag = 0x3A;
  in.scene = "scStrap";
  in.sequence = "sqBoot";
  in.plugged = 0x01;
  Outputs out = Decide(in);
  EXPECT_FALSE(out.keep);
  in.plugged = 0x03;
  out = Decide(in);
  EXPECT_TRUE(out.keep);
  EXPECT_EQ(out.pending, 1);
  EXPECT_FALSE(out.move);
  ExpectIdempotent(in, out);
  in = After(in, out, 500);
  in.scene = "muMenuMain";
  in.sequence = "sqMenuMain";
  EXPECT_EQ(RunUntilMove(in, 100), 2000 + static_cast<int>(FriendsMove::MENU_SETTLE_FRAMES));
}

TEST(OrcaOnlineMenuDropIn, AMenuStillLoadingIsNeverLeft)
{
  Inputs in = OnMenu(1000);
  in.scene = "scTitle";
  in.sequence = "sqTitle";
  in.plugged = 0x03;
  Outputs out = Decide(in);
  EXPECT_EQ(out.pending, 1);
  in = After(in, out, 444);
  in.scene = "muMenuMain";
  in.sequence = "sqMenuMain";
  in.step = 0;
  in.menu_built = true;
  out = Decide(in);
  EXPECT_EQ(out.menu_since, 0u);
  EXPECT_EQ(out.pending, 1);
  in = After(in, out);
  in.step = FriendsMove::STEP_RUNNING;
  in.menu_built = false;
  for (; in.frame < 1502;)
  {
    out = Decide(in);
    ExpectIdempotent(in, out);
    EXPECT_FALSE(out.move) << in.frame;
    EXPECT_EQ(out.menu_since, 0u) << in.frame;
    EXPECT_EQ(out.pending, 1) << in.frame;
    in = After(in, out);
  }
  in.menu_built = true;
  EXPECT_EQ(RunUntilMove(in, 100), 1502 + static_cast<int>(FriendsMove::MENU_SETTLE_FRAMES));

  in = OnMenu(3000);
  in.menu_built = false;
  in.plugged = 0x03;
  out = Decide(in);
  EXPECT_EQ(out.pending, 1);
  EXPECT_EQ(out.menu_since, 0u);
  in = After(in, out);
  for (int i = 0; i < 120; ++i)
  {
    out = Decide(in);
    ExpectIdempotent(in, out);
    EXPECT_FALSE(out.move) << in.frame;
    EXPECT_EQ(out.menu_since, 0u) << in.frame;
    EXPECT_EQ(out.pending, 1) << in.frame;
    in = After(in, out);
  }
  in.menu_built = true;
  EXPECT_EQ(RunUntilMove(in, 100), 3121 + static_cast<int>(FriendsMove::MENU_SETTLE_FRAMES));
}

TEST(OrcaOnlineMenuDropIn, TheNameKeypadIsNeverLeft)
{
  Inputs in = OnMenu(700);
  in.keypad = true;
  in.plugged = 0x03;
  Outputs out = Decide(in);
  EXPECT_EQ(out.pending, 1);
  EXPECT_EQ(out.menu_since, 0u);
  EXPECT_FALSE(out.move);
  in = After(in, out);
  for (; in.frame < 2000;)
  {
    out = Decide(in);
    ExpectIdempotent(in, out);
    EXPECT_FALSE(out.move) << in.frame;
    EXPECT_EQ(out.menu_since, 0u) << in.frame;
    EXPECT_EQ(out.pending, 1) << in.frame;
    in = After(in, out);
  }
  in.keypad = false;
  EXPECT_EQ(RunUntilMove(in, 100), 2000 + static_cast<int>(FriendsMove::MENU_SETTLE_FRAMES));

  in = OnMenu(3000);
  in.plugged = 0x03;
  out = Decide(in);
  EXPECT_EQ(out.menu_since, 3001u);
  in = After(in, out, 10);
  in.keypad = true;
  out = Decide(in);
  EXPECT_FALSE(out.move);
  EXPECT_EQ(out.menu_since, 0u);
  EXPECT_EQ(out.pending, 1);
  in = After(in, out, 100);
  in.keypad = false;
  EXPECT_EQ(RunUntilMove(in, 100), 3110 + static_cast<int>(FriendsMove::MENU_SETTLE_FRAMES));
}

TEST(OrcaOnlineMenuDropIn, NoMoveForQueueRoomsVersusOrAFriendWhoLeft)
{
  {
    Inputs in = OnMenu(1000);
    in.menu_since = 1;
    in.pending = 1;
    in.plugged = 0x03;
    in.queue = true;
    const Outputs out = Decide(in);
    EXPECT_FALSE(out.move);
    EXPECT_EQ(out.pending, 0);
  }
  {
    Inputs in = OnMenu(1000);
    in.scene = "scSelctCharacter";
    in.sequence = "sqVsMelee";
    in.plugged = 0x03;
    Outputs out = Decide(in);
    EXPECT_EQ(out.pending, 0);
    in = After(in, out, 300);
    in.scene = "muMenuMain";
    in.sequence = "sqMenuMain";
    for (int i = 0; i < 60; ++i)
    {
      out = Decide(in);
      EXPECT_FALSE(out.move);
      in = After(in, out);
    }
  }
  {
    Inputs in = OnMenu(1000);
    in.scene = "scMelee";
    in.sequence = "sqSingleSimple";
    in.plugged = 0x03;
    Outputs out = Decide(in);
    EXPECT_EQ(out.pending, 1);
    in = After(in, out, 100);
    in.plugged = 0x01;
    out = Decide(in);
    EXPECT_EQ(out.pending, 0);
  }
  {
    Inputs in = OnMenu(1000);
    in.tag = 0x3A;
    in.seen = 0x20;
    in.pending = 0x00;
    in.menu_since = 0x3A200000;
    in.plugged = 0x03;
    Outputs out = Decide(in);
    EXPECT_TRUE(out.keep);
    EXPECT_EQ(out.seen, 0x03);
    EXPECT_EQ(out.pending, 1);
    EXPECT_EQ(out.menu_since, 1001u);
    EXPECT_FALSE(out.move);
    ExpectIdempotent(in, out);
    in.frame = FriendsMove::FIRST_FRIEND_FRAME - 1;
    out = Decide(in);
    EXPECT_TRUE(out.keep);
    EXPECT_EQ(out.seen, 0x03);
    EXPECT_EQ(out.pending, 0);
    in.frame = 1000;
    in.plugged = 0x01;
    EXPECT_FALSE(Decide(in).keep);
  }
  {
    Inputs in = OnMenu(1000);
    in.menu_since = 1;
    in.pending = 1;
    in.plugged = 0x03;
    in.exit_code = 1;
    EXPECT_FALSE(Decide(in).move);
    in.exit_code = 0;
    in.step = FriendsMove::STEP_EXIT;
    EXPECT_FALSE(Decide(in).move);
  }
}

namespace
{
FakeMemory MainMenu(bool built, u32 step = FriendsMove::STEP_RUNNING, u32 exit_code = 0)
{
  FakeMemory m = Scene("muMenuMain", exit_code);
  m.Fill(0x80910000, FriendsMove::MENU_BUILT + 4);
  m.Write32(0x80910000, 0x80920000);
  m.bytes[0x80910000 + FriendsMove::MENU_BUILT] = built ? 1 : 0;
  m.Write32(0x80900288, step);
  m.writes = 0;
  return m;
}

FakeMemory CharacterSelect(bool readable)
{
  FakeMemory m = Scene("scSelctCharacter", 30);
  m.Fill(0x80910000, 0x404);
  m.Write32(0x80910000, 0x80920000);
  if (readable)
  {
    m.Fill(0x80930000, 0x50);
    m.Write32(0x80910400, 0x80930000);
  }
  m.writes = 0;
  return m;
}
}

namespace
{
void Task(FakeMemory& m, u32 task, std::string_view name, u32 prev, u32 next)
{
  m.Fill(task, 0x40);
  m.Write32(task, task + 0x100);
  m.Text(task + 0x100, name);
  m.Write32(task + FriendsMove::TASK_PREV, prev);
  m.Write32(task + FriendsMove::TASK_NEXT, next);
}

FakeMemory MenuWithTasks(bool keypad)
{
  FakeMemory m = MainMenu(true);
  m.Write32(0x80910000 + FriendsMove::MENU_TASK, 0x81000000);
  Task(m, 0x81000000, "MainMenuTask", 0, 0x81001000);
  Task(m, 0x81001000, "muPanelCtrl", 0x81000000, 0x81002000);
  Task(m, 0x81002000, "Menu", 0x81001000, 0x81003000);
  Task(m, 0x81003000, "Menu", 0x81002000, keypad ? 0x81004000 : 0);
  if (keypad)
  {
    Task(m, 0x81004000, "MuName", 0x81003000, 0x81005000);
    Task(m, 0x81005000, "MuNameRemoteTask", 0x81004000, 0);
  }
  m.writes = 0;
  return m;
}
}

TEST(OrcaOnlineMenuDropIn, ReadsTheNameKeypadFromTheMenusTasks)
{
  using FriendsMove::KeypadOpen;
  EXPECT_TRUE(KeypadOpen(MenuWithTasks(true), 0x80910000));
  EXPECT_FALSE(KeypadOpen(MenuWithTasks(false), 0x80910000));
  {
    FakeMemory m = MenuWithTasks(false);
    Task(m, 0x81004000, "MuName", 0, 0x81000000);
    m.Write32(0x81000000 + FriendsMove::TASK_PREV, 0x81004000);
    EXPECT_TRUE(KeypadOpen(m, 0x80910000));
  }
  {
    FakeMemory m = MenuWithTasks(false);
    Task(m, 0x81004000, "MuNameRemoteTask", 0x81003000, 0);
    m.Write32(0x81003000 + FriendsMove::TASK_NEXT, 0x81004000);
    EXPECT_FALSE(KeypadOpen(m, 0x80910000));
  }
  {
    FakeMemory m = MenuWithTasks(false);
    m.Write32(0x81003000 + FriendsMove::TASK_NEXT, 0x81001000);
    EXPECT_FALSE(KeypadOpen(m, 0x80910000));
    m.Write32(0x81003000 + FriendsMove::TASK_NEXT, 0x8F000000);
    EXPECT_FALSE(KeypadOpen(m, 0x80910000));
    m.Write32(0x81003000 + FriendsMove::TASK_NEXT, 0x81004000);
    Task(m, 0x81004000, "MuName", 0x81003000, 0);
    m.Write32(0x81004000, 0x8F000000);
    EXPECT_FALSE(KeypadOpen(m, 0x80910000));
  }
  EXPECT_FALSE(KeypadOpen(MainMenu(true), 0x80910000));
  EXPECT_FALSE(KeypadOpen(FakeMemory{}, 0x80910000));
  FakeMemory m = MenuWithTasks(true);
  (void)KeypadOpen(m, 0x80910000);
  EXPECT_EQ(m.writes, 0);
}

TEST(OrcaOnlineMenuFresh, ReadsTheBuiltMainMenuAndTheCharacterSelect)
{
  using FreshMove::Read;
  EXPECT_TRUE(Read(MainMenu(true)).menu_built);
  EXPECT_FALSE(Read(MainMenu(true)).css);
  EXPECT_FALSE(Read(MainMenu(false)).menu_built);
  EXPECT_FALSE(Read(MainMenu(true, FriendsMove::STEP_EXIT)).menu_built);
  EXPECT_FALSE(Read(MainMenu(true, FriendsMove::STEP_RUNNING, 30)).menu_built);
  EXPECT_FALSE(Read(Scene("scMemoryChange", 30)).menu_built);
  EXPECT_TRUE(Read(CharacterSelect(true)).css);
  EXPECT_FALSE(Read(CharacterSelect(false)).css);
  EXPECT_FALSE(Read(CharacterSelect(true)).menu_built);
  EXPECT_EQ(Read(FakeMemory{}), FreshMove::Seen{});
  FakeMemory m = MainMenu(true);
  (void)Read(m);
  EXPECT_EQ(m.writes, 0);
}

TEST(OrcaOnlineMenuFresh, AFreshStartLeavesOnlyABuiltMainMenuWithItsExit)
{
  for (const u32 exit : {25u, 30u, 31u})
  {
    SCOPED_TRACE(exit);
    FakeMemory m = MainMenu(true);
    ASSERT_TRUE(FreshMove::Apply(m, exit));
    EXPECT_EQ(m.Read32(0x80900284), exit);
    EXPECT_EQ(m.Read32(0x80900288), FriendsMove::STEP_EXIT);
    EXPECT_EQ(ReadMenuState(m).exit_code, exit);
    const int writes = m.writes;
    EXPECT_FALSE(FreshMove::Apply(m, exit));
    EXPECT_EQ(m.writes, writes);
  }
  for (const u32 exit : {0u, 24u, 26u, 27u, 29u, 32u})
  {
    FakeMemory m = MainMenu(true);
    EXPECT_FALSE(FreshMove::Apply(m, exit)) << exit;
    EXPECT_EQ(m.writes, 0);
  }
  for (FakeMemory m : {MainMenu(false), MainMenu(true, FriendsMove::STEP_EXIT),
                       MainMenu(true, FriendsMove::STEP_RUNNING, 25), CharacterSelect(true),
                       Scene("scMemoryChange", 30)})
  {
    EXPECT_FALSE(FreshMove::Apply(m, 30));
    EXPECT_EQ(m.writes, 0);
  }
}

namespace
{
FakeMemory Generators(u32 default_seed, u32 menu_seed)
{
  FakeMemory m;
  for (const auto& [rng, seed] :
       {std::pair{FreshMove::RNG_DEFAULT, default_seed}, std::pair{FreshMove::RNG_MENU, menu_seed}})
  {
    m.Fill(rng, 8);
    m.Write32(rng, FreshMove::RNG_VTABLE);
    m.Write32(rng + FreshMove::RNG_SEED, seed);
  }
  m.writes = 0;
  return m;
}
}

TEST(OrcaOnlineMenuFresh, AHistorysSeedStartsBothRandomNumberGenerators)
{
  FakeMemory m = Generators(0x4C561667, 0x207D5B71);
  ASSERT_TRUE(FreshMove::ApplySeed(m, 0x12345678));
  const u32 fight = m.Read32(FreshMove::RNG_DEFAULT + FreshMove::RNG_SEED);
  const u32 menu = m.Read32(FreshMove::RNG_MENU + FreshMove::RNG_SEED);
  EXPECT_EQ(fight, FreshMove::RngSeed(0x12345678, FreshMove::RNG_DEFAULT));
  EXPECT_EQ(menu, FreshMove::RngSeed(0x12345678, FreshMove::RNG_MENU));
  EXPECT_NE(fight, menu);
  EXPECT_LE(fight, 0x7FFFFFFFu);
  EXPECT_LE(menu, 0x7FFFFFFFu);
  EXPECT_EQ(m.Read32(FreshMove::RNG_DEFAULT), FreshMove::RNG_VTABLE);
  EXPECT_EQ(m.Read32(FreshMove::RNG_MENU), FreshMove::RNG_VTABLE);
  EXPECT_TRUE(FreshMove::ApplySeed(m, 0x12345678));
  EXPECT_EQ(m.Read32(FreshMove::RNG_DEFAULT + FreshMove::RNG_SEED), fight);
  EXPECT_EQ(m.Read32(FreshMove::RNG_MENU + FreshMove::RNG_SEED), menu);
  FakeMemory n = Generators(1, 2);
  ASSERT_TRUE(FreshMove::ApplySeed(n, 0x12345679));
  EXPECT_NE(n.Read32(FreshMove::RNG_MENU + FreshMove::RNG_SEED), menu);
  EXPECT_NE(n.Read32(FreshMove::RNG_DEFAULT + FreshMove::RNG_SEED), fight);
  FakeMemory zero = Generators(5, 6);
  EXPECT_FALSE(FreshMove::ApplySeed(zero, 0));
  EXPECT_EQ(zero.writes, 0);
  FakeMemory none;
  EXPECT_FALSE(FreshMove::ApplySeed(none, 7));
  EXPECT_EQ(none.writes, 0);
  for (const u32 rng : {FreshMove::RNG_DEFAULT, FreshMove::RNG_MENU})
  {
    FakeMemory other = Generators(5, 6);
    other.Write32(rng, 0x80001234);
    other.writes = 0;
    EXPECT_FALSE(FreshMove::ApplySeed(other, 7)) << std::hex << rng;
    EXPECT_EQ(other.writes, 0);
  }
}

TEST(OrcaOnlineMenuFresh, TheCharacterSelectsRandomDrawsDependOnTheSeed)
{
  const auto next = [](u32 seed) { return (seed * 0x41C64E6Du + 12345u) & 0x7FFFFFFFu; };
  const auto first_draw = [&](u32 menu_seed) {
    const u32 drawn = next(menu_seed);
    return static_cast<int>(static_cast<double>(drawn) / 2147483648.0 * 42);
  };
  std::set<int> draws;
  for (u32 seed = 1; seed <= 64; ++seed)
    draws.insert(first_draw(FreshMove::RngSeed(seed, FreshMove::RNG_MENU)));
  EXPECT_GT(draws.size(), 10u);
}

TEST(OrcaOnlineMenuLobby, TheMenusAreTheMainMenuAndACharacterSelect)
{
  EXPECT_TRUE(MenusScene("muMenuMain", "sqMenuMain"));
  EXPECT_TRUE(MenusScene("scSelctCharacter", "sqVsMelee"));
  EXPECT_TRUE(MenusScene("scSelctCharacter", "sqSpMelee"));
  for (const char* scene : {"scMelee", "scSelStage", "scVsResult", "scMemoryChange", "", "scTitle"})
  {
    SCOPED_TRACE(scene);
    EXPECT_FALSE(MenusScene(scene, "sqVsMelee"));
  }
  EXPECT_FALSE(MenusScene("scSelctCharacter", "sqSingleSimple"));
  EXPECT_FALSE(MenusScene("scSelctCharacter", "sqTraining"));
  EXPECT_FALSE(MenusScene("muMenuMain", "sqHomerun"));
}

TEST(OrcaOnlineMenuLobby, APeerFaultOrAVerdictThatNeverCameVoidsFirst)
{
  using Rollback::OnlineMatch::VerdictWaitInputs;
  using Rollback::OnlineMatch::VoidFirst;
  VerdictWaitInputs in;
  in.queue_room = true;
  in.match_live = true;
  in.from_peer = true;
  EXPECT_TRUE(VoidFirst(in));
  in.from_peer = false;
  EXPECT_FALSE(VoidFirst(in)) << "this machine's own failure";
  in.waited_out = true;
  EXPECT_TRUE(VoidFirst(in));
  const auto with = [&in](auto change) {
    VerdictWaitInputs other = in;
    change(other);
    return VoidFirst(other);
  };
  EXPECT_FALSE(with([](VerdictWaitInputs& o) { o.queue_room = false; }));
  EXPECT_FALSE(with([](VerdictWaitInputs& o) { o.match_live = false; }));
  EXPECT_FALSE(with([](VerdictWaitInputs& o) { o.set_over = true; }));
  EXPECT_FALSE(with([](VerdictWaitInputs& o) { o.room_ended = true; }));
}

TEST(OrcaStallBudget, ATrickleUnderTheSilenceLimitRunsTheBudgetOut)
{
  using Rollback::OnlineMatch::StallBudget;
  using namespace std::chrono_literals;
  const StallBudget::Clock::time_point t0{};
  StallBudget budget;
  auto t = t0 + 100s;
  budget.Add(t, t + 9900ms);
  t += 9900ms + 17ms;
  EXPECT_FALSE(budget.Spent(t, t));
  EXPECT_FALSE(budget.Spent(t + 5s, t));
  EXPECT_TRUE(budget.Spent(t + 5200ms, t));
  StallBudget honest;
  for (auto at = t0 + 100s; at < t0 + 400s; at += 10s)
  {
    honest.Add(at, at + 1s);
    EXPECT_FALSE(honest.Spent(at + 1s)) << (at - t0) / 1s;
  }
  StallBudget old;
  old.Add(t0 + 100s, t0 + 114s);
  EXPECT_EQ(old.Stalled(t0 + 3700s), StallBudget::Clock::duration::zero());
  EXPECT_FALSE(old.Spent(t0 + 3700s, t0 + 3699s));
  EXPECT_EQ(old.Stalled(t0 + 170s), 4s);
}

TEST(OrcaStallBudget, ARecoveredOutageIsExcusedAFewTimesOnly)
{
  using Rollback::OnlineMatch::StallBudget;
  using namespace std::chrono_literals;
  const StallBudget::Clock::time_point t0{};
  StallBudget budget;
  auto t = t0 + 100s;
  EXPECT_TRUE(budget.ExcuseRecovered(t + 15s));
  t += 15s + 2s;
  budget.Add(t, t + 40ms);
  EXPECT_FALSE(budget.Spent(t + 40ms));
  for (int i = 1; i < StallBudget::MAX_RECOVERIES; ++i)
    EXPECT_TRUE(budget.ExcuseRecovered(t + std::chrono::seconds(i)));
  EXPECT_FALSE(budget.ExcuseRecovered(t + 10s));
  EXPECT_TRUE(budget.ExcuseRecovered(t + StallBudget::RECOVERY_WINDOW + 20s));
}

TEST(OrcaStallBudget, RoomHoldsExcuseOnlyTheOngoingStall)
{
  using Rollback::OnlineMatch::StallBudget;
  using namespace std::chrono_literals;
  const StallBudget::Clock::time_point t0{};
  StallBudget budget;
  auto start = t0 + 100s;
  for (int input = 0; input < 2; ++input)
  {
    EXPECT_FALSE(budget.Spent(start + 9900ms, start, true));
    budget.Add(start, start + 9900ms);
    start += 9900ms + 17ms;
  }
  EXPECT_TRUE(budget.Spent(start, start, true));
  StallBudget outage;
  EXPECT_FALSE(outage.Spent(t0 + 160s, t0 + 100s, true));
  EXPECT_TRUE(outage.Spent(t0 + 160s, t0 + 100s, false));
}

TEST(OrcaOnlineMenuLobby, AJoinersOwnPortValuesAreItsOwn)
{
  using Orca::Net::KeyframeInfo;
  using Orca::Net::NEVER;
  using Rollback::OnlineMatch::OwnValues;
  using Rollback::OnlineMatch::OwnValuesKept;
  const std::vector<u8> controls{0x10, 0x20, 0x30};
  const std::vector<u8> queue{0x01, 0xff, 0xff, 0x05, 0x00, 0, 0, 0, 0, 0, 0, 0, 0};
  const OwnValues own{"bo", controls, queue};
  const KeyframeInfo::Name host{0, 1524, "ada", {}, {}};
  const KeyframeInfo::Name honest{1, 1730, "bo", controls, queue};
  EXPECT_TRUE(OwnValuesKept({host, honest}, 1, NEVER, own));
  EXPECT_TRUE(OwnValuesKept({host, honest}, 1, 1730, own));
  KeyframeInfo::Name earlier = honest;
  earlier.from = 300;
  earlier.controls = {0x99};
  EXPECT_TRUE(OwnValuesKept({host, earlier, honest}, 1, 1730, own));
  KeyframeInfo::Name forced = honest;
  forced.queue[3] = 0x11;
  EXPECT_FALSE(OwnValuesKept({host, forced}, 1, NEVER, own));
  EXPECT_FALSE(OwnValuesKept({host, forced}, 1, 1730, own));
  KeyframeInfo::Name remapped = honest;
  remapped.controls = {0x10, 0x20, 0x31};
  EXPECT_FALSE(OwnValuesKept({host, remapped}, 1, 1730, own));
  KeyframeInfo::Name renamed = honest;
  renamed.name = "bob";
  EXPECT_FALSE(OwnValuesKept({host, renamed}, 1, 1730, own));
  EXPECT_TRUE(OwnValuesKept({host, renamed}, 1, 1730, OwnValues{"", controls, queue}));
  EXPECT_FALSE(OwnValuesKept({host, honest}, 1, 1730, OwnValues{"bo", controls, {}}));
  KeyframeInfo::Name bare = honest;
  bare.controls.clear();
  bare.queue.clear();
  EXPECT_FALSE(OwnValuesKept({host, bare}, 1, 1730, own));
  EXPECT_FALSE(OwnValuesKept({host}, 1, 1730, own));
  EXPECT_TRUE(OwnValuesKept({host}, 1, 1730, OwnValues{"bo", {}, {}}));
  KeyframeInfo::Name moved = honest;
  moved.from = 1800;
  EXPECT_FALSE(OwnValuesKept({host, forced, moved}, 1, 1730, own));
  KeyframeInfo::Name later = forced;
  later.from = 2400;
  EXPECT_FALSE(OwnValuesKept({host, honest, later}, 1, 1730, own));
  KeyframeInfo::Name other = forced;
  other.seat = 2;
  EXPECT_TRUE(OwnValuesKept({host, honest, other}, 1, 1730, own));
}

TEST(OrcaOnlineMenuLobby, AFormerJoinerComesHomeOnTheMenusAlone)
{
  using Rollback::OnlineMatch::HomeInputs;
  using Rollback::OnlineMatch::MayComeHome;
  HomeInputs in;
  in.local_seat = 1;
  in.solo_idle = true;
  in.on_menus = true;
  EXPECT_TRUE(MayComeHome(in));
  in.local_seat = 3;
  EXPECT_TRUE(MayComeHome(in));
  in.local_seat = 1;
  const auto without = [&in](auto change) {
    HomeInputs other = in;
    change(other);
    return MayComeHome(other);
  };
  EXPECT_FALSE(without([](HomeInputs& i) { i.local_seat = 0; }));
  EXPECT_FALSE(without([](HomeInputs& i) { i.joining = true; }));
  EXPECT_FALSE(without([](HomeInputs& i) { i.session = true; }));
  EXPECT_FALSE(without([](HomeInputs& i) { i.solo_idle = false; }));
  EXPECT_FALSE(without([](HomeInputs& i) { i.drop_in_pending = true; }));
  EXPECT_FALSE(without([](HomeInputs& i) { i.on_menus = false; }));
  EXPECT_FALSE(without([](HomeInputs& i) { i.room_up = true; }));
  EXPECT_FALSE(without([](HomeInputs& i) { i.lingering = true; }));
  EXPECT_FALSE(without([](HomeInputs& i) { i.queue_image = true; }));
}

TEST(OrcaOnlineMenuLobby, CasualOrRankedWithFriendsLeavesTheFriendsLobby)
{
  using Rollback::OnlineMatch::DecideLobbyPick;
  using Rollback::OnlineMatch::LobbyPickInputs;
  using Rollback::OnlineMatch::LobbyPickStep;
  LobbyPickInputs host;
  host.host_cap = true;
  host.pick_cap = true;
  host.session = true;
  const auto step = [](const LobbyPickInputs& in) { return DecideLobbyPick(in).step; };
  const auto with = [&host](auto change) {
    LobbyPickInputs other = host;
    change(other);
    return other;
  };
  EXPECT_EQ(step(host), LobbyPickStep::Leave);
  EXPECT_FALSE(DecideLobbyPick(host).drop_keyframe);
  EXPECT_EQ(step(with([](LobbyPickInputs& i) {
              i.session = false;
              i.drop_in_friends = true;
            })),
            LobbyPickStep::Leave);
  EXPECT_EQ(step(with([](LobbyPickInputs& i) {
              i.session = false;
              i.arrival_pending = true;
            })),
            LobbyPickStep::Leave);
  EXPECT_EQ(step(with([](LobbyPickInputs& i) {
              i.session_idle = true;
              i.arrival_pending = true;
            })),
            LobbyPickStep::Leave);
  EXPECT_EQ(step(with([](LobbyPickInputs& i) {
              i.joining = true;
              i.local_seat = 1;
            })),
            LobbyPickStep::Ignore);
  EXPECT_EQ(step(with([](LobbyPickInputs& i) {
              i.joining = true;
              i.local_seat = 1;
              i.session = false;
            })),
            LobbyPickStep::Ignore);
  EXPECT_EQ(step(with([](LobbyPickInputs& i) { i.queue_room = true; })), LobbyPickStep::Ignore);
  EXPECT_EQ(step(with([](LobbyPickInputs& i) {
              i.queue_room = true;
              i.session = false;
            })),
            LobbyPickStep::Ignore);
  EXPECT_EQ(step(with([](LobbyPickInputs& i) { i.host_cap = false; })), LobbyPickStep::Ignore);
  EXPECT_EQ(step(with([](LobbyPickInputs& i) { i.local_seat = 1; })), LobbyPickStep::Ignore);
}

TEST(OrcaOnlineMenuLobby, WithFriendsAQueueThePageCantSearchKeepsThem)
{
  using Rollback::OnlineMatch::DecideLobbyPick;
  using Rollback::OnlineMatch::LobbyPickInputs;
  using Rollback::OnlineMatch::LobbyPickPlan;
  using Rollback::OnlineMatch::LobbyPickStep;
  LobbyPickInputs host;
  host.host_cap = true;
  host.session = true;
  const auto with = [&host](auto change) {
    LobbyPickInputs other = host;
    change(other);
    return other;
  };
  const auto stays = [](const LobbyPickPlan& plan) {
    return plan.step == LobbyPickStep::Announce && !plan.arm && !plan.drop_keyframe;
  };
  EXPECT_TRUE(stays(DecideLobbyPick(host)));
  EXPECT_TRUE(stays(DecideLobbyPick(with([](LobbyPickInputs& i) {
    i.session = false;
    i.drop_in_friends = true;
    i.keyframe_kept = true;
  }))));
  EXPECT_TRUE(stays(DecideLobbyPick(with([](LobbyPickInputs& i) {
    i.session = false;
    i.arrival_pending = true;
  }))));
  EXPECT_TRUE(stays(DecideLobbyPick(with([](LobbyPickInputs& i) {
    i.session_idle = true;
    i.arrival_pending = true;
  }))));
  const LobbyPickPlan leave = DecideLobbyPick(with([](LobbyPickInputs& i) { i.pick_cap = true; }));
  EXPECT_EQ(leave.step, LobbyPickStep::Leave);
  EXPECT_TRUE(leave.arm);
  EXPECT_EQ(DecideLobbyPick(with([](LobbyPickInputs& i) { i.host_cap = false; })).step,
            LobbyPickStep::Ignore);
  EXPECT_EQ(DecideLobbyPick(with([](LobbyPickInputs& i) {
              i.joining = true;
              i.local_seat = 1;
            })).step,
            LobbyPickStep::Ignore);
  EXPECT_EQ(DecideLobbyPick(with([](LobbyPickInputs& i) { i.local_seat = 1; })).step,
            LobbyPickStep::Ignore);
  EXPECT_EQ(DecideLobbyPick(with([](LobbyPickInputs& i) { i.queue_room = true; })).step,
            LobbyPickStep::Ignore);
  const LobbyPickPlan alone = DecideLobbyPick(with([](LobbyPickInputs& i) { i.session = false; }));
  EXPECT_EQ(alone.step, LobbyPickStep::Announce);
  EXPECT_TRUE(alone.arm);
}

TEST(OrcaOnlineMenuLobby, APickKeptWithFriendsIsArmedOnceTheyAreGone)
{
  using Rollback::OnlineMatch::DecideKeptPick;
  using Rollback::OnlineMatch::KeptPickInputs;
  using Rollback::OnlineMatch::KeptPickStep;
  KeptPickInputs in;
  in.stands = true;
  EXPECT_EQ(DecideKeptPick(in), KeptPickStep::Wait);
  in.alone = true;
  EXPECT_EQ(DecideKeptPick(in), KeptPickStep::Arm);
  in.stands = false;
  EXPECT_EQ(DecideKeptPick(in), KeptPickStep::Drop);
  in.alone = false;
  EXPECT_EQ(DecideKeptPick(in), KeptPickStep::Drop);
  in.stands = true;
  in.queue_or_search = true;
  EXPECT_EQ(DecideKeptPick(in), KeptPickStep::Drop);
  in.alone = true;
  EXPECT_EQ(DecideKeptPick(in), KeptPickStep::Drop);
}

TEST(OrcaOnlineMenuLobby, EveryCapFitsTheAppsCapsLine)
{
  const std::string all = fmt::format(
      "{} {} {} {} {} {} {} {} {} {}", Orca::Status::CAPS, Orca::Status::MUSIC_CAP,
      Orca::Status::RESULTS_CAP, Orca::Status::LOCKS_CAP, Orca::Status::QUEUE2_CAP,
      Orca::Status::PICK_CAPS, Orca::Status::TALK_CAP, Orca::Status::RUMBLE_CAP,
      Orca::Status::RECORD_CAP, Orca::Status::HOSTUI_CAP);
  std::istringstream words{all};
  int count = 0;
  for (std::string word; words >> word;)
  {
    ++count;
    EXPECT_LE(word.size(), 24u) << word;
  }
  EXPECT_LE(count, 24);
  EXPECT_EQ(count, 21);
  const std::string offered = Orca::Status::OfferedCaps();
  const std::string tail = fmt::format(" {} {} {} {}", Orca::Status::TALK_CAP,
                                       Orca::Status::RUMBLE_CAP, Orca::Status::RECORD_CAP,
                                       Orca::Status::HOSTUI_CAP);
  ASSERT_GE(offered.size(), tail.size());
  EXPECT_EQ(offered.substr(offered.size() - tail.size()), tail);
}

TEST(OrcaOnlineMenuLobby, APickWithNobodyToLeaveIsAnnounced)
{
  using Rollback::OnlineMatch::DecideLobbyPick;
  using Rollback::OnlineMatch::LobbyPickInputs;
  using Rollback::OnlineMatch::LobbyPickStep;
  LobbyPickInputs in;
  in.host_cap = true;
  EXPECT_EQ(DecideLobbyPick(in).step, LobbyPickStep::Announce);
  EXPECT_TRUE(DecideLobbyPick(in).arm);
  in.host_cap = false;
  EXPECT_EQ(DecideLobbyPick(in).step, LobbyPickStep::Announce);
  in.local_seat = 1;
  EXPECT_EQ(DecideLobbyPick(in).step, LobbyPickStep::Announce);
  in = {};
  in.host_cap = true;
  in.session = true;
  in.session_idle = true;
  EXPECT_EQ(DecideLobbyPick(in).step, LobbyPickStep::Announce);
  in.keyframe_kept = true;
  EXPECT_FALSE(DecideLobbyPick(in).drop_keyframe);
  in = {};
  in.host_cap = true;
  in.keyframe_kept = true;
  EXPECT_EQ(DecideLobbyPick(in).step, LobbyPickStep::Announce);
  EXPECT_TRUE(DecideLobbyPick(in).drop_keyframe);
  in.keyframe_kept = false;
  EXPECT_FALSE(DecideLobbyPick(in).drop_keyframe);
  in.keyframe_kept = true;
  in.queue_room = true;
  EXPECT_EQ(DecideLobbyPick(in).step, LobbyPickStep::Ignore);
  EXPECT_FALSE(DecideLobbyPick(in).drop_keyframe);
  in.queue_room = false;
  in.joining = true;
  EXPECT_FALSE(DecideLobbyPick(in).drop_keyframe);
  in = {};
  in.host_cap = true;
  in.pick_cap = true;
  in.drop_in_friends = true;
  in.keyframe_kept = true;
  EXPECT_EQ(DecideLobbyPick(in).step, LobbyPickStep::Leave);
}

TEST(OrcaOnlineMenuLobby, AFriendHomeOnThePicksCharacterSelectTakesThePickToo)
{
  using Rollback::OnlineMatch::AnnounceHomePick;
  EXPECT_TRUE(AnnounceHomePick(true, true, false));
  EXPECT_FALSE(AnnounceHomePick(false, true, false));
  EXPECT_FALSE(AnnounceHomePick(true, false, false));
  EXPECT_FALSE(AnnounceHomePick(true, true, true));
  EXPECT_EQ(QueuePickForCssCode(30), OnlinePick::Casual);
  EXPECT_EQ(QueuePickForCssCode(31), OnlinePick::Ranked);
  for (const u32 code : {0u, 24u, 25u, 26u, 27u, 28u, 29u, 32u, 0xCCCCCCCCu})
  {
    SCOPED_TRACE(code);
    EXPECT_FALSE(QueuePickForCssCode(code));
  }
}

TEST(OrcaOnlineMenuLobby, AFormerJoinersMoveToPort1NeverTakesTheMenuToWithFriends)
{
  {
    Inputs in = OnMenu(5000);
    in.seen = 0x03;
    in.pending = 1;
    in.menu_since = 4990;
    in.plugged = 0x01;
    const Outputs out = Decide(in);
    EXPECT_FALSE(out.move);
    EXPECT_EQ(out.pending, 0);
    EXPECT_EQ(out.menu_since, 0u);
    ExpectIdempotent(in, out);
    Inputs next = After(in, out);
    EXPECT_EQ(RunUntilMove(next, 120), -1);
  }
  {
    Inputs in = OnMenu(5000);
    in.scene = "scSelctCharacter";
    in.sequence = "sqVsMelee";
    in.seen = 0x03;
    in.plugged = 0x02;
    Outputs out = Decide(in);
    EXPECT_EQ(out.pending, 0);
    in = After(in, out);
    in.plugged = 0x01;
    out = Decide(in);
    EXPECT_EQ(out.pending, 0);
    EXPECT_EQ(out.seen, 0x01);
    ExpectIdempotent(in, out);
    in = After(in, out, 300);
    in.scene = "muMenuMain";
    in.sequence = "sqMenuMain";
    EXPECT_EQ(RunUntilMove(in, 120), -1);
  }
}

TEST(OrcaOnlineMenuLobby, AFriendOnTheWayEndsTheHostsQueue)
{
  using Rollback::OnlineMatch::FriendEndsQueue;
  using Rollback::OnlineMatch::FriendQueueInputs;
  FriendQueueInputs in;
  in.friend_coming = true;
  in.queue_or_search = true;
  in.friends_room = true;
  EXPECT_TRUE(FriendEndsQueue(in));
  const auto without = [&in](auto change) {
    FriendQueueInputs other = in;
    change(other);
    return FriendEndsQueue(other);
  };
  EXPECT_FALSE(without([](FriendQueueInputs& i) { i.friend_coming = false; }));
  EXPECT_FALSE(without([](FriendQueueInputs& i) { i.queue_or_search = false; }));
  EXPECT_FALSE(without([](FriendQueueInputs& i) { i.friends_room = false; }));
  EXPECT_FALSE(without([](FriendQueueInputs& i) { i.joining = true; }));
  EXPECT_FALSE(without([](FriendQueueInputs& i) { i.local_seat = 1; }));
}

TEST(OrcaOnlineMenuLobby, JoiningAFriendsRoomEndsTheJoinersQueue)
{
  using Rollback::OnlineMatch::JoinEndsQueue;
  EXPECT_TRUE(JoinEndsQueue(false, true, true));
  EXPECT_TRUE(JoinEndsQueue(false, true, false));
  EXPECT_TRUE(JoinEndsQueue(false, false, true));
  EXPECT_FALSE(JoinEndsQueue(false, false, false));
  EXPECT_FALSE(JoinEndsQueue(true, true, true));
  EXPECT_FALSE(JoinEndsQueue(true, false, true));
}

TEST(OrcaOnlineMenuLobby, AHostsPickIsArmedAgainOnceTheFriendsItEndedForAreGone)
{
  using Rollback::OnlineMatch::DecideFriendsPick;
  using Rollback::OnlineMatch::FriendsPickInputs;
  using Rollback::OnlineMatch::FriendsPickStep;
  FriendsPickInputs in;
  in.played = true;
  in.alone = true;
  in.on_pick_select = true;
  in.host_cap = true;
  EXPECT_EQ(DecideFriendsPick(in), FriendsPickStep::Arm);
  const auto with = [&in](auto change) {
    FriendsPickInputs other = in;
    change(other);
    return DecideFriendsPick(other);
  };
  EXPECT_EQ(with([](FriendsPickInputs& i) { i.played = false; }), FriendsPickStep::Wait);
  EXPECT_EQ(with([](FriendsPickInputs& i) { i.alone = false; }), FriendsPickStep::Wait);
  EXPECT_EQ(with([](FriendsPickInputs& i) { i.on_pick_select = false; }), FriendsPickStep::Wait);
  EXPECT_EQ(with([](FriendsPickInputs& i) { i.queue_or_search = true; }), FriendsPickStep::Drop);
  EXPECT_EQ(with([](FriendsPickInputs& i) { i.host_cap = false; }), FriendsPickStep::Drop);
  EXPECT_EQ(with([](FriendsPickInputs& i) { i.elsewhere = true; }), FriendsPickStep::Drop);
}

TEST(OrcaOnlineMenuReader, SceneExitWritesOnlyItsDeclaredSingletonWords)
{
  FakeMemory m = Scene("muMenuMain", 0);
  constexpr u32 manager = 0x805B8BA0;
  m.Fill(manager, 0x300);
  m.Write32(0x805A0060, manager);
  m.Write32(manager + 4, 0x80910000);
  m.Write32(manager + 0x288, FriendsMove::STEP_RUNNING);
  m.Fill(0x80910000, FriendsMove::MENU_BUILT + 1);
  m.Write32(0x80910000, 0x80920000);
  m.Write8(0x80910000 + FriendsMove::MENU_BUILT, 1);
  m.track_writes = true;
  ASSERT_TRUE(FreshMove::Apply(m, 25));
  EXPECT_EQ(m.Read32(0x805B8E24), 25u);
  EXPECT_EQ(m.recorded.size(), 8u);
  const auto regions = BrawlGame::Regions();
  for (const u32 address : m.recorded)
    EXPECT_TRUE(std::any_of(regions.begin(), regions.end(),
                           [&](const auto& region) { return region.Contains(address, 1); }))
        << std::hex << address;
}

TEST(OrcaOnlineMenuDropIn, VersusFightAndResultsHoldUntilTheSelectOrMenus)
{
  for (const char* sequence : {"sqVsMelee", "sqSpMelee"})
  {
    EXPECT_TRUE(FightHoldsDropIn("scMelee", sequence, false));
    EXPECT_TRUE(FightHoldsDropIn("scVsResult", sequence, false));
    EXPECT_TRUE(FightHoldsDropIn("scMemoryChange", sequence, true));
    EXPECT_TRUE(FightHoldsDropIn("", "", true));
    EXPECT_FALSE(FightHoldsDropIn("scSelctCharacter", sequence, true));
    EXPECT_FALSE(FightHoldsDropIn("muMenuMain", "sqMenuMain", true));
    EXPECT_FALSE(FightHoldsDropIn("scSelStage", sequence, false));
  }
  EXPECT_FALSE(FightHoldsDropIn("scMelee", "sqTraining", false));
  EXPECT_FALSE(FightHoldsDropIn("scMelee", "sqSingleSimple", false));
}

TEST(OrcaOnlineMenuDropIn, TourneyAndRotationKeepImmediateJoins)
{
  for (const char* sequence : {"sqToMelee", "sqQuMelee"})
  {
    EXPECT_FALSE(FightHoldsDropIn("scMelee", sequence, false));
    EXPECT_FALSE(FightHoldsDropIn("scVsResult", sequence, false));
    EXPECT_FALSE(FightHoldsDropIn("scMemoryChange", sequence, true));
  }
}

TEST(OrcaOnlineMenuDropIn, FreshStartDiscardsTheOldFightEvenBeforeTheNextSelect)
{
  for (const char* scene : {"scTitle", "scMemoryChange", ""})
  {
    FightHoldLatch latch;
    latch.Observe("scMelee", "sqVsMelee");
    ASSERT_TRUE(latch.Held());
    latch.Reset();
    latch.Observe(scene, "");
    EXPECT_FALSE(latch.Held()) << scene;
    latch.Observe("scMelee", "sqVsMelee");
    EXPECT_TRUE(latch.Held());
  }
}

namespace
{
struct FightExitBoundary
{
  std::string_view scene;
  std::string_view sequence = "sqVsMelee";
  bool fight_held = true;
  bool single_player_held = false;
};

void ExpectFightExit(std::initializer_list<FightExitBoundary> boundaries)
{
  using namespace Rollback::OnlineMatch;
  FightHoldLatch latch;
  FightHoldInputs fight;
  fight.waiting = true;
  FreshInputs start;
  start.enabled = true;
  start.origin_ready = true;
  start.waiting = true;
  start.history_frames = FRESH_FRIENDS_AFTER + 1;
  for (const auto& boundary : boundaries)
  {
    SCOPED_TRACE(std::string(boundary.scene) + " / " + std::string(boundary.sequence));
    latch.Observe(boundary.scene, boundary.sequence);
    ASSERT_EQ(latch.Held(), boundary.fight_held);
    fight.fight = latch.Held();
    EXPECT_EQ(FightJoinHeld(fight), boundary.fight_held);
    fight.enabled = false;
    EXPECT_FALSE(FightJoinHeld(fight));
    fight.enabled = true;
    start.single_player = SequenceHoldsDropIn(boundary.sequence);
    EXPECT_EQ(start.single_player, boundary.single_player_held);
    if (start.single_player)
      EXPECT_EQ(DecideFreshStart(start), FreshStep::Hold);
    if (FightJoinHeld(fight) || start.single_player)
      continue;
    EXPECT_EQ(DecideFreshStart(start), FreshStep::Now);
  }
  ASSERT_FALSE(latch.Held());
  ASSERT_FALSE(start.single_player);
  ASSERT_EQ(DecideFreshStart(start), FreshStep::Now);
  ASSERT_TRUE(FreshTailSettled(FreshKind::Menu, FRESH_CSS_SETTLE, 72, 300, 300));
  const auto fresh_offer = ChooseJoinCapture(nullptr, false, 301, 300);
  ASSERT_TRUE(fresh_offer);
  EXPECT_EQ(fresh_offer->frame, 301);
  start.history_frames = FRESH_FRIENDS_AFTER;
  ASSERT_EQ(DecideFreshStart(start), FreshStep::None);
  const auto in_place_offer = ChooseJoinCapture(nullptr, false, 1000, 300);
  ASSERT_TRUE(in_place_offer);
  EXPECT_EQ(in_place_offer->frame, 1000);
}
}

TEST(OrcaFightHold, PauseQuitReleasesAtCharacterSelect)
{
  ExpectFightExit({{"scMelee"},
                   {"scMelee"},
                   {"scMemoryChange"},
                   {"", ""},
                   {"scSelctCharacter", "sqVsMelee", false}});
}

TEST(OrcaFightHold, PauseQuitWithNoContestResultsReleasesAtCharacterSelect)
{
  ExpectFightExit({{"scMelee"},
                   {"scMelee"},
                   {"scMemoryChange"},
                   {"scVsResult"},
                   {"scMemoryChange", "sqPrizeCheck"},
                   {"scSelctCharacter", "sqVsMelee", false}});
}

TEST(OrcaFightHold, ResultsContinueReleasesAfterPrizeCheck)
{
  ExpectFightExit({{"scMelee"},
                   {"scMemoryChange"},
                   {"scVsResult"},
                   {"scVsResult"},
                   {"scMemoryChange", "sqPrizeCheck"},
                   {"", ""},
                   {"scSelctCharacter", "sqVsMelee", false}});
}

TEST(OrcaFightHold, ResultsThenBackOutReachesMainMenu)
{
  ExpectFightExit({{"scMelee"},
                   {"scMemoryChange"},
                   {"scVsResult"},
                   {"scMemoryChange", "sqPrizeCheck"},
                   {"scSelctCharacter", "sqVsMelee", false},
                   {"scMemoryChange", "sqMenuMain", false},
                   {"muMenuMain", "sqMenuMain", false}});
}

TEST(OrcaFightHold, ResultsToMainMenuReleasesEvenWithoutASelectObservation)
{
  ExpectFightExit({{"scMelee"},
                   {"scVsResult"},
                   {"scMemoryChange", "sqMenuMain"},
                   {"muMenuMain", "sqMenuMain", false}});
}

TEST(OrcaFightHold, TitleTransitionReleasesAtThePatchedMainMenu)
{
  ExpectFightExit({{"scMelee"},
                   {"scVsResult"},
                   {"scMemoryChange", "sqTitle"},
                   {"scTitle", "sqTitle"},
                   {"scMemoryChange", "sqMenuMain"},
                   {"muMenuMain", "sqMenuMain", false}});
}

TEST(OrcaFightHold, LastStockFinishReleasesAfterResults)
{
  ExpectFightExit({{"scMelee"},
                   {"scMelee"},
                   {"scMemoryChange"},
                   {"scVsResult"},
                   {"scMemoryChange", "sqPrizeCheck"},
                   {"scSelctCharacter", "sqVsMelee", false}});
}

TEST(OrcaFightHold, TimeoutFinishReleasesAfterResults)
{
  ExpectFightExit({{"scMelee"},
                   {"scMelee"},
                   {"scMemoryChange"},
                   {"scVsResult"},
                   {"scMemoryChange", "sqPrizeCheck"},
                   {"scSelctCharacter", "sqVsMelee", false}});
}

TEST(OrcaFightHold, SuddenDeathSceneReentryHoldsUntilTheFinalResultsAndSelect)
{
  ExpectFightExit({{"scMelee"},
                   {"scMemoryChange"},
                   {"", ""},
                   {"scMelee"},
                   {"scMemoryChange"},
                   {"scVsResult"},
                   {"scMemoryChange", "sqPrizeCheck"},
                   {"scSelctCharacter", "sqVsMelee", false}});
}

TEST(OrcaFightHold, BrawlSuddenDeathWithinTheFightSceneReleasesAfterResults)
{
  ExpectFightExit({{"scMelee"},
                   {"scMelee"},
                   {"scMelee"},
                   {"scMemoryChange"},
                   {"scVsResult"},
                   {"scMemoryChange", "sqPrizeCheck"},
                   {"scSelctCharacter", "sqVsMelee", false}});
}

TEST(OrcaFightHold, SpecialBrawlFinishAndPauseQuitReleaseAtItsSelect)
{
  ExpectFightExit({{"scMelee", "sqSpMelee"},
                   {"scMemoryChange", "sqSpMelee"},
                   {"scVsResult", "sqSpMelee"},
                   {"scMemoryChange", "sqPrizeCheck"},
                   {"scSelctCharacter", "sqSpMelee", false}});
  ExpectFightExit({{"scMelee", "sqSpMelee"},
                   {"scMelee", "sqSpMelee"},
                   {"scMemoryChange", "sqSpMelee"},
                   {"scSelctCharacter", "sqSpMelee", false}});
}

TEST(OrcaFightHold, ProjectPlusResultsSkipReleasesWithoutAResultsScene)
{
  ExpectFightExit({{"scMelee"},
                   {"scMemoryChange"},
                   {"", "sqVsMelee"},
                   {"scSelctCharacter", "sqVsMelee", false}});
}

TEST(OrcaFightHold, ProjectPlusRunbackKeepsTheFightHeldUntilTheHostExits)
{
  ExpectFightExit({{"scMelee"},
                   {"scMemoryChange"},
                   {"scMelee"},
                   {"scMemoryChange"},
                   {"scMelee"},
                   {"scMemoryChange"},
                   {"scSelctCharacter", "sqVsMelee", false}});
}

TEST(OrcaFightHold, TrainingExitReleasesTheSinglePlayerHoldAtMainMenu)
{
  ExpectFightExit({{"scMelee", "sqTraining", false, true},
                   {"scMemoryChange", "sqTraining", false, true},
                   {"scSelctCharacter", "sqTraining", false, true},
                   {"scMemoryChange", "sqMenuMain", false},
                   {"muMenuMain", "sqMenuMain", false}});
}

TEST(OrcaFightHold, TourneyAndRotationExitsKeepImmediateJoins)
{
  for (const char* sequence : {"sqToMelee", "sqQuMelee"})
  {
    SCOPED_TRACE(sequence);
    ExpectFightExit({{"scMelee", sequence, false},
                     {"scMemoryChange", sequence, false},
                     {"scVsResult", sequence, false},
                     {"scMemoryChange", sequence, false},
                     {"scSelctCharacter", sequence, false},
                     {"muMenuMain", "sqMenuMain", false}});
  }
}
