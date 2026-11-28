// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <filesystem>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Common/FileUtil.h"
#include "Core/Orca/UX/GamePatches.h"
#include "Core/Orca/Engine/GuestMemory.h"

using namespace Orca::UX;

namespace
{
class Memory final : public GuestMemory
{
public:
  bool Valid(u32 a) const override { return bytes.contains(a); }
  u8 Read8(u32 a) const override { return bytes.at(a); }
  u16 Read16(u32 a) const override { return static_cast<u16>(Read8(a) << 8 | Read8(a + 1)); }
  u32 Read32(u32 a) const override { return u32(Read16(a)) << 16 | Read16(a + 2); }
  void Write8(u32 a, u8 v) override { bytes[a] = v; }
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
  void Unmap(u32 a)
  {
    for (u32 i = 0; i < 4; ++i)
      bytes.erase(a + i);
  }

  std::map<u32, u8> bytes;
};

std::vector<GamePatch> Parse(const std::string& text)
{
  std::string error;
  const auto patches = ParseGamePatches(text, &error);
  EXPECT_TRUE(patches.has_value()) << error << "\n" << text;
  return patches.value_or(std::vector<GamePatch>{});
}

void Write(const PatchPlan& plan, Memory& memory)
{
  for (const PatchWrite& w : plan.writes)
    memory.Write32(w.address, w.value);
}

std::vector<u32> Addresses(const std::vector<PatchPlan::Group>& groups)
{
  std::vector<u32> out;
  for (const PatchPlan::Group& g : groups)
    out.push_back(g.address);
  return out;
}

constexpr u32 MAGIC = 0x90001000;
constexpr u32 MODE = 0x90001004;
constexpr u32 SSM1 = 0x53534D31;

const char* const FILE_WITH_TOGGLES = "# toggled while the header says an online mode\n"
                                      "when 90001000 == 53534D31      # the magic\n"
                                      "when 90001004 & FF000000 != 0  # the mode: not none\n"
                                      "81001000 4082007C 4800007C     # bne -> b\n"
                                      "81001004 38600001 38600001     # guard only\n"
                                      "81001010 41820010 60000000     # beq -> nop\n"
                                      "81001014 38000000 38000000     # guard only\n"
                                      "end\n"
                                      "81002000 11111111 22222222     # lands once\n"
                                      "9017B640 * 000003FF            # data, every frame\n";

Memory Booted(u32 mode)
{
  Memory m;
  m.Write32(0x81001000, 0x4082007C);
  m.Write32(0x81001004, 0x38600001);
  m.Write32(0x81001010, 0x41820010);
  m.Write32(0x81001014, 0x38000000);
  m.Write32(0x81002000, 0x11111111);
  m.Write32(0x9017B640, 0);
  m.Write32(MAGIC, SSM1);
  m.Write32(MODE, mode);
  return m;
}
}

TEST(OrcaGamePatches, ParsesWhenBlocks)
{
  const auto p = Parse("80FFFFFC 1 2                 # before the block, 4 bytes before its first\n"
                       "when 90001000 == 53534D31\r\n"
                       "when 90001004 & ff000000 != 0  # lower-case hex too\n"
                       "81000000 4082007C 4800007C\n"
                       "81000004 38600001 38600001\n"
                       "81000010 11111111 22222222\n"
                       "81000014 33333333 33333333     # a guard\n"
                       "end   # a comment\n"
                       "81000018 44444444 55555555     # right after the block's last word\n"
                       "when 90001000 == 53534D31\n"
                       "8100001C 5 6                   # the same condition, a new block\n"
                       "81000020 7 7\n"
                       "end\n"
                       "81000024 8 9\n");
  ASSERT_EQ(p.size(), 9u);
  const std::vector<PatchCondition> when{{0x90001000, 0xFFFFFFFF, SSM1, true},
                                         {0x90001004, 0xFF000000, 0, false}};
  const std::vector<PatchCondition> magic{{0x90001000, 0xFFFFFFFF, SSM1, true}};
  EXPECT_TRUE(p[0].when.empty());
  for (size_t i = 1; i <= 4; ++i)
    EXPECT_EQ(p[i].when, when) << i;
  EXPECT_TRUE(p[5].when.empty());
  EXPECT_EQ(p[6].when, magic);
  EXPECT_EQ(p[7].when, magic);
  EXPECT_TRUE(p[8].when.empty());
  EXPECT_FALSE(p[0].joins_previous);
  EXPECT_FALSE(p[1].joins_previous);
  EXPECT_TRUE(p[2].joins_previous);
  EXPECT_FALSE(p[3].joins_previous);
  EXPECT_TRUE(p[4].joins_previous);
  EXPECT_FALSE(p[5].joins_previous);
  EXPECT_FALSE(p[6].joins_previous);
  EXPECT_TRUE(p[7].joins_previous);
  EXPECT_FALSE(p[8].joins_previous);
  EXPECT_EQ(p[1].original, std::optional<u32>(0x4082007C));
  EXPECT_EQ(p[1].value, 0x4800007Cu);
  for (const GamePatch& g : Parse("81000000 1 2\n81000004 3 4\n9017B640 * 5\n"))
    EXPECT_TRUE(g.when.empty());
}

TEST(OrcaGamePatches, RefusesMalformedBlocks)
{
  struct Bad
  {
    const char* text;
    const char* line;
  };
  for (const Bad& bad : std::initializer_list<Bad>{
           {"end\n", "line 1"},
           {"81000000 1 2\nwhen 90001000 == 1\n81000008 1 2\n", "line 2"},
           {"when 90001000 == 1\nend\n", "line 2"},
           {"when 90001000 == 1\n9017B640 * 5\nend\n", "line 2"},
           {"when 90001000 == 1\n81000000 1 2\nwhen 90001004 == 1\nend\n", "line 3"},
           {"when 90001000 == 1\n81000000 1 2\nend 1\n", "line 3"},
           {"when 90001002 == 1\n81000000 1 2\nend\n", "line 1"},
           {"when 70001000 == 1\n81000000 1 2\nend\n", "line 1"},
           {"when 94000000 == 1\n81000000 1 2\nend\n", "line 1"},
           {"when 90001000 = 1\n81000000 1 2\nend\n", "line 1"},
           {"when 90001000 1\n81000000 1 2\nend\n", "line 1"},
           {"when 90001000\n81000000 1 2\nend\n", "line 1"},
           {"when 90001000 == 1 2\n81000000 1 2\nend\n", "line 1"},
           {"when 90001000 == G\n81000000 1 2\nend\n", "line 1"},
           {"when 90001000 == 123456789\n81000000 1 2\nend\n", "line 1"},
           {"when 90001000 & 0 == 0\n81000000 1 2\nend\n", "line 1"},
           {"when 90001000 & FF == 100\n81000000 1 2\nend\n", "line 1"},
           {"when 90001000 | FF == 1\n81000000 1 2\nend\n", "line 1"},
           {"when 90001000 & FF ==\n81000000 1 2\nend\n", "line 1"},
           {"WHEN 90001000 == 1\n81000000 1 2\nEND\n", "line 1"},
           {"9017B640 * 5\nwhen 9017B640 == 5\n81000000 1 2\nend\n", "line 2"},
           {"when 81000000 == 1\n81000000 1 2\nend\n", "line 1"},
           {"when 90001000 == 1\n81000000 1 2\nend\nwhen 81000000 == 2\n81000010 1 2\nend\n",
            "line 4"},
           {"when 90001000 == 1\n81000000 1 2\nend\n90001000 * 3\n", "line 1"},
           {"81000000 1 2\nwhen 90001000 == 1\n81000000 1 3\nend\n", "line 3"},
           {"when 90001000 == 1\n81000000 1 2\nend\n", "line 2"},
           {"when 90001000 == 1\n81000000 1 2\n81000004 3 3\n81000010 5 6\nend\n", "line 4"},
           {"when 80000000 == 0\nwhen 90000000 == 0\n81000000 1 1\n81000004 2 2\nend\n",
            "line 3"},
       })
  {
    std::string error;
    EXPECT_FALSE(ParseGamePatches(bad.text, &error).has_value()) << bad.text;
    EXPECT_TRUE(error.starts_with(std::string(bad.line) + ":")) << bad.text << "\n" << error;
  }
  std::string error;
  for (const char* good : {"when 90001000 != 0\n81000000 1 2\n81000004 3 3\nend\n",
                           "when 93FFFFFC & 80000000 == 80000000\n817FFFF8 1 1\n817FFFFC 1 2\nend\n",
                           "when 80000000 == 0\nwhen 90000000 == 0\n81000000 1 1\n81000004 2 3\nend\n"})
  {
    EXPECT_TRUE(ParseGamePatches(good, &error).has_value()) << good << "\n" << error;
  }
}

TEST(OrcaGamePatches, ConditionsReadOneMaskedWordEach)
{
  Memory m;
  m.Write32(MAGIC, SSM1);
  m.Write32(MODE, 0x02000000);
  const auto holds = [&](std::vector<PatchCondition> when) { return ConditionsHold(when, m); };
  EXPECT_TRUE(holds({}));
  EXPECT_TRUE(holds({{MAGIC, 0xFFFFFFFF, SSM1, true}}));
  EXPECT_FALSE(holds({{MAGIC, 0xFFFFFFFF, SSM1 + 1, true}}));
  EXPECT_TRUE(holds({{MAGIC, 0xFFFFFFFF, SSM1 + 1, false}}));
  EXPECT_FALSE(holds({{MAGIC, 0xFFFFFFFF, SSM1, false}}));
  EXPECT_TRUE(holds({{MODE, 0xFF000000, 0x02000000, true}}));
  EXPECT_TRUE(holds({{MODE, 0xFF000000, 0, false}}));
  EXPECT_FALSE(holds({{MODE, 0x00FF0000, 0, false}}));
  EXPECT_TRUE(holds({{MAGIC, 0xFFFFFFFF, SSM1, true}, {MODE, 0xFF000000, 0, false}}));
  EXPECT_FALSE(holds({{MAGIC, 0xFFFFFFFF, SSM1, true}, {MODE, 0xFF000000, 0, true}}));
  EXPECT_FALSE(holds({{MAGIC, 0xFFFFFFFF, 0, true}, {MODE, 0xFF000000, 0, false}}));
  EXPECT_FALSE(holds({{0x90002000, 0xFFFFFFFF, 0, true}}));
  EXPECT_FALSE(holds({{0x90002000, 0xFFFFFFFF, 0, false}}));
}

TEST(OrcaGamePatches, AToggledGroupOnlyEverMovesBetweenItsTwoStates)
{
  const auto p = Parse("when 90001000 == 1\n"
                       "81000000 1 A\n81000004 2 2\n81000008 3 C\n"
                       "end\n");
  ASSERT_EQ(p.size(), 3u);
  const auto toggle = [&](std::vector<u32> now, bool holds) { return ToggleGroup(p, now, holds); };
  EXPECT_EQ(toggle({1, 2, 3}, true), Toggle::On);
  EXPECT_EQ(toggle({0xA, 2, 0xC}, true), Toggle::Leave);
  EXPECT_EQ(toggle({0xA, 2, 0xC}, false), Toggle::Off);
  EXPECT_EQ(toggle({1, 2, 3}, false), Toggle::Leave);
  for (const bool holds : {true, false})
  {
    SCOPED_TRACE(holds);
    EXPECT_EQ(toggle({1, 2, 0xC}, holds), Toggle::Leave);
    EXPECT_EQ(toggle({0xA, 2, 3}, holds), Toggle::Leave);
    EXPECT_EQ(toggle({1, 9, 3}, holds), Toggle::Leave);
    EXPECT_EQ(toggle({0xA, 9, 0xC}, holds), Toggle::Leave);
    EXPECT_EQ(toggle({1, 2, 9}, holds), Toggle::Leave);
    EXPECT_EQ(toggle({1, 2}, holds), Toggle::Leave);
    EXPECT_EQ(toggle({}, holds), Toggle::Leave);
  }
  const std::vector<PatchCondition> when{{0x90001000, 0xFFFFFFFF, 1, true}};
  const std::vector<GamePatch> guards{{0x81000000, 7, 7, false, when},
                                      {0x81000004, 8, 8, true, when}};
  EXPECT_EQ(ToggleGroup(guards, std::vector<u32>{7, 8}, true), Toggle::Leave);
  EXPECT_EQ(ToggleGroup(guards, std::vector<u32>{7, 8}, false), Toggle::Leave);
  EXPECT_TRUE(GroupApplies(p, std::vector<u32>{1, 2, 3}));
  EXPECT_FALSE(GroupApplies(p, std::vector<u32>{0xA, 2, 0xC}));
}

TEST(OrcaGamePatches, ToggledGroupsFollowTheirConditionBothWays)
{
  const auto patches = Parse(FILE_WITH_TOGGLES);
  ASSERT_EQ(patches.size(), 6u);
  Memory m = Booted(0);
  const auto toggled = [&] { return PlanGamePatches(patches, m, true); };

  PatchPlan plan = toggled();
  EXPECT_TRUE(plan.writes.empty());
  EXPECT_TRUE(plan.on.empty() && plan.off.empty() && plan.landed.empty());

  m.Write32(MODE, 0x01000000);
  plan = toggled();
  EXPECT_EQ(Addresses(plan.on), (std::vector<u32>{0x81001000, 0x81001010}));
  EXPECT_EQ(plan.on[0].words, 2u);
  EXPECT_EQ(plan.on[1].words, 2u);
  EXPECT_TRUE(plan.off.empty() && plan.landed.empty());
  ASSERT_EQ(plan.writes.size(), 2u);
  EXPECT_EQ(plan.writes[0].address, 0x81001000u);
  EXPECT_EQ(plan.writes[0].value, 0x4800007Cu);
  EXPECT_EQ(plan.writes[1].address, 0x81001010u);
  EXPECT_EQ(plan.writes[1].value, 0x60000000u);
  for (const PatchWrite& w : plan.writes)
    EXPECT_TRUE(w.code) << std::hex << w.address;
  Write(plan, m);
  EXPECT_TRUE(toggled().writes.empty());
  m.Write32(MODE, 0x02000000);
  EXPECT_TRUE(toggled().writes.empty());

  m.Write32(MODE, 0);
  plan = toggled();
  EXPECT_EQ(Addresses(plan.off), (std::vector<u32>{0x81001000, 0x81001010}));
  EXPECT_TRUE(plan.on.empty());
  ASSERT_EQ(plan.writes.size(), 2u);
  EXPECT_EQ(plan.writes[0].value, 0x4082007Cu);
  EXPECT_EQ(plan.writes[1].value, 0x41820010u);
  for (const PatchWrite& w : plan.writes)
    EXPECT_TRUE(w.code) << std::hex << w.address;
  Write(plan, m);
  EXPECT_EQ(m.Read32(0x81001000), 0x4082007Cu);
  EXPECT_EQ(m.Read32(0x81001004), 0x38600001u);
  EXPECT_EQ(m.Read32(0x81001010), 0x41820010u);
  EXPECT_TRUE(toggled().writes.empty());

  m.Write32(MAGIC, 0);
  m.Write32(MODE, 0x01000000);
  EXPECT_TRUE(toggled().writes.empty());
  m.Write32(MAGIC, SSM1);
  Write(toggled(), m);
  EXPECT_EQ(m.Read32(0x81001000), 0x4800007Cu);

  m.Unmap(MODE);
  plan = toggled();
  EXPECT_EQ(Addresses(plan.off), (std::vector<u32>{0x81001000, 0x81001010}));
  Write(plan, m);
  m.Write32(MODE, 0x01000000);
  Write(toggled(), m);

  m.Write32(0x81001000, 0xDEADBEEF);
  m.Write32(0x81001004, 0xDEADBEEF);
  m.Write32(0x81001010, 0xDEADBEEF);
  EXPECT_TRUE(toggled().writes.empty());
  m.Write32(MODE, 0);
  EXPECT_TRUE(toggled().writes.empty());
  m.Write32(MODE, 0x01000000);
  m.Write32(0x81001000, 0x4082007C);
  m.Write32(0x81001004, 0x38600001);
  m.Write32(0x81001010, 0x41820010);
  EXPECT_EQ(Addresses(toggled().on), (std::vector<u32>{0x81001000, 0x81001010}));
  m.Write32(0x81001004, 0x38600002);
  EXPECT_EQ(Addresses(toggled().on), (std::vector<u32>{0x81001010}));
}

TEST(OrcaGamePatches, EachPassTakesItsOwnKind)
{
  const auto patches = Parse(FILE_WITH_TOGGLES);
  Memory m = Booted(0x01000000);
  PatchPlan plan = PlanGamePatches(patches, m, false);
  EXPECT_EQ(Addresses(plan.landed), (std::vector<u32>{0x81002000}));
  EXPECT_EQ(plan.landed[0].words, 1u);
  EXPECT_TRUE(plan.on.empty() && plan.off.empty());
  ASSERT_EQ(plan.writes.size(), 2u);
  EXPECT_EQ(plan.writes[0].address, 0x81002000u);
  EXPECT_EQ(plan.writes[0].value, 0x22222222u);
  EXPECT_TRUE(plan.writes[0].code);
  EXPECT_EQ(plan.writes[1].address, 0x9017B640u);
  EXPECT_EQ(plan.writes[1].value, 0x3FFu);
  EXPECT_FALSE(plan.writes[1].code);
  Write(plan, m);
  EXPECT_TRUE(PlanGamePatches(patches, m, false).writes.empty());
  m.Write32(0x9017B640, 7);
  plan = PlanGamePatches(patches, m, false);
  ASSERT_EQ(plan.writes.size(), 1u);
  EXPECT_EQ(plan.writes[0].address, 0x9017B640u);
  EXPECT_TRUE(plan.landed.empty());
  m = Booted(0x01000000);
  plan = PlanGamePatches(patches, m, true);
  for (const PatchWrite& w : plan.writes)
    EXPECT_TRUE(w.address >= 0x81001000 && w.address <= 0x81001010) << std::hex << w.address;
  EXPECT_TRUE(plan.landed.empty());
}

TEST(OrcaGamePatches, TogglesArePureFunctionsOfMemory)
{
  const auto patches = Parse(FILE_WITH_TOGGLES);
  Memory played = Booted(0);
  for (int round = 0; round < 5; ++round)
  {
    played.Write32(MODE, round % 2 ? 0x01000000 : 0x02000000);
    Write(PlanGamePatches(patches, played, false), played);
    Write(PlanGamePatches(patches, played, true), played);
    played.Write32(MODE, 0);
    Write(PlanGamePatches(patches, played, false), played);
    Write(PlanGamePatches(patches, played, true), played);
  }
  played.Write32(MODE, 0x01000000);
  Memory keyframe;
  keyframe.bytes = played.bytes;
  for (int frame = 0; frame < 3; ++frame)
  {
    SCOPED_TRACE(frame);
    if (frame == 2)
    {
      played.Write32(MODE, 0);
      keyframe.Write32(MODE, 0);
    }
    for (const bool toggled : {false, true})
    {
      const PatchPlan a = PlanGamePatches(patches, played, toggled);
      const PatchPlan b = PlanGamePatches(patches, keyframe, toggled);
      ASSERT_EQ(a.writes.size(), b.writes.size());
      for (size_t i = 0; i < a.writes.size(); ++i)
      {
        EXPECT_EQ(a.writes[i].address, b.writes[i].address);
        EXPECT_EQ(a.writes[i].value, b.writes[i].value);
        EXPECT_EQ(a.writes[i].code, b.writes[i].code);
      }
      EXPECT_EQ(a.on, b.on);
      EXPECT_EQ(a.off, b.off);
      EXPECT_EQ(a.landed, b.landed);
      Write(a, played);
      Write(b, keyframe);
    }
    EXPECT_EQ(played.bytes, keyframe.bytes);
    EXPECT_TRUE(PlanGamePatches(patches, played, false).writes.empty());
    EXPECT_TRUE(PlanGamePatches(patches, played, true).writes.empty());
  }
  const auto before = played.bytes;
  played.Write32(MODE, 0x01000000);
  const auto with_mode = played.bytes;
  (void)PlanGamePatches(patches, played, true);
  (void)PlanGamePatches(patches, played, false);
  EXPECT_EQ(played.bytes, with_mode);
  EXPECT_NE(before, with_mode);
}

namespace
{
std::vector<GamePatch> Shipped(const char* name)
{
  const std::string path =
      (std::filesystem::path(__FILE__).parent_path() / "../../../../Data/Sys/Subspace" / name).string();
  std::string text;
  EXPECT_TRUE(File::ReadFileToString(path, text)) << path;
  return Parse(text);
}
}

TEST(OrcaGamePatches, TheShippedFilesLandAsBefore)
{
  for (const char* name : {"RSBE01.patches", "PPLUS32.patches"})
  {
    SCOPED_TRACE(name);
    const auto patches = Shipped(name);
    ASSERT_FALSE(patches.empty());
    Memory m;
    std::set<u32> written;
    for (const GamePatch& p : patches)
    {
      m.Write32(p.address, p.original.value_or(0));
      written.insert(p.address);
    }
    std::set<u32> expected;
    for (size_t start = 0; start < patches.size();)
    {
      size_t end = start + 1;
      while (end < patches.size() && patches[end].joins_previous)
        ++end;
      const std::span<const GamePatch> group(patches.data() + start, end - start);
      start = end;
      if (!group[0].when.empty())
      {
        for (const GamePatch& p : group)
        {
          EXPECT_TRUE(p.original.has_value()) << std::hex << p.address;
          EXPECT_EQ(p.when, group[0].when) << std::hex << p.address;
        }
        for (const PatchCondition& c : group[0].when)
          EXPECT_FALSE(written.contains(c.address)) << std::hex << c.address;
        continue;
      }
      std::vector<u32> now;
      for (const GamePatch& p : group)
        now.push_back(p.original.value_or(0));
      if (!GroupApplies(group, now))
        continue;
      for (const GamePatch& p : group)
      {
        if (p.value != now[&p - group.data()])
          expected.insert(p.address);
      }
    }
    std::set<u32> planned;
    for (const PatchWrite& w : PlanGamePatches(patches, m, false).writes)
      planned.insert(w.address);
    EXPECT_EQ(planned, expected);
    EXPECT_GT(planned.size(), 10u);
  }
}
