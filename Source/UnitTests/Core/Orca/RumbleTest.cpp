// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Core/Orca/Status.h"
#include "Core/Orca/UX/ControllerSource.h"
#include "Core/Orca/UX/Rumble.h"

namespace
{
using Orca::UX::Rumble::Device;
using Orca::UX::Rumble::FormatLine;
using Orca::UX::Rumble::MIN_GAP_FRAMES;
using Orca::UX::Rumble::MotorOn;
using Orca::UX::Rumble::OFF_FRAMES;
using Orca::UX::Rumble::RESEND_FRAMES;
using Orca::UX::Rumble::State;
using Orca::UX::Rumble::Target;
using Orca::UX::Rumble::Tracker;

constexpr Target ADAPTER0{Device::Adapter, 0};
constexpr Target PAD1{Device::Pad, 1};

std::vector<std::pair<int, std::string>> Feed(Tracker& tracker, const std::vector<bool>& motor,
                                              Target target = ADAPTER0, bool live = true,
                                              int port = 0)
{
  std::vector<std::pair<int, std::string>> lines;
  for (int f = 0; f < static_cast<int>(motor.size()); ++f)
  {
    if (const std::optional<State> s = tracker.Boundary(motor[f], live, port, target))
      lines.emplace_back(f, FormatLine(*s));
  }
  return lines;
}

std::vector<bool> Repeat(const std::vector<bool>& pattern, int times)
{
  std::vector<bool> out;
  for (int i = 0; i < times; ++i)
    out.insert(out.end(), pattern.begin(), pattern.end());
  return out;
}
}

TEST(OrcaRumble, TheMotorIsOutput1OfTheControllersDirectCommand)
{
  EXPECT_TRUE(MotorOn(0x00400301));
  EXPECT_FALSE(MotorOn(0x00400300));
  EXPECT_FALSE(MotorOn(0x00400302));
  EXPECT_FALSE(MotorOn(0));
  EXPECT_FALSE(MotorOn(0x00410301));
  EXPECT_TRUE(MotorOn(0x00400001));
}

TEST(OrcaRumble, TheLineNamesThePortTheStateAndTheDevice)
{
  EXPECT_EQ(FormatLine({true, 1, ADAPTER0}), "orca rumble 2 1 adapter 0");
  EXPECT_EQ(FormatLine({false, 0, PAD1}), "orca rumble 1 0 pad 1");
  EXPECT_EQ(FormatLine({false, 3, {Device::None, 7}}), "orca rumble 4 0 none 0");
}

TEST(OrcaRumble, OnAtOnceOffAfterThreeFramesOff)
{
  Tracker t;
  std::vector<bool> motor{false};
  motor.insert(motor.end(), 8, true);
  motor.insert(motor.end(), 5, false);
  const auto lines = Feed(t, motor);
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0], std::make_pair(1, std::string("orca rumble 1 1 adapter 0")));
  EXPECT_EQ(lines[1].second, "orca rumble 1 0 adapter 0");
  EXPECT_EQ(lines[1].first, 9 + OFF_FRAMES - 1);
  EXPECT_FALSE(t.Sent().on);
}

TEST(OrcaRumble, ASteadyMotorIsSaidAgainEveryHalfSecond)
{
  Tracker t;
  const auto lines = Feed(t, std::vector<bool>(600, true));
  ASSERT_EQ(lines.size(), 600u / RESEND_FRAMES);
  for (std::size_t i = 0; i < lines.size(); ++i)
  {
    EXPECT_EQ(lines[i].first, static_cast<int>(i) * RESEND_FRAMES);
    EXPECT_EQ(lines[i].second, "orca rumble 1 1 adapter 0");
  }
  static_assert(RESEND_FRAMES <= 30, "the page's hold is about a second");
}

TEST(OrcaRumble, TheGamesOnOffPatternsAreOneBuzz)
{
  Tracker t;
  const auto lines = Feed(t, Repeat({true, false, false}, 100));
  ASSERT_EQ(lines.size(), 10u);
  for (std::size_t i = 0; i < lines.size(); ++i)
  {
    EXPECT_EQ(lines[i].second, "orca rumble 1 1 adapter 0");
    if (i > 0)
      EXPECT_EQ(lines[i].first - lines[i - 1].first, RESEND_FRAMES);
  }
}

TEST(OrcaRumble, NoRenewalWhileTheMotorIsOff)
{
  Tracker t;
  Feed(t, std::vector<bool>(RESEND_FRAMES - 1, true));
  const auto off = Feed(t, std::vector<bool>(OFF_FRAMES + 20, false));
  ASSERT_EQ(off.size(), 1u);
  EXPECT_EQ(off[0], std::make_pair(OFF_FRAMES - 1, std::string("orca rumble 1 0 adapter 0")));
  Tracker unfocused;
  Feed(unfocused, std::vector<bool>(RESEND_FRAMES - 1, true));
  const auto lines = Feed(unfocused, std::vector<bool>(60, true), ADAPTER0, false);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].second, "orca rumble 1 0 adapter 0");
}

TEST(OrcaRumble, AtMostTenOnLinesAndTwentyLinesASecond)
{
  for (const std::vector<bool>& pattern :
       {std::vector<bool>{true, false, false, false},
        std::vector<bool>{true, true, false, false, false},
        std::vector<bool>{true, false, false, false, false, false},
        std::vector<bool>{true, false, false, false, false, false, false, false}})
  {
    Tracker t;
    const auto lines = Feed(t, Repeat(pattern, 120));
    ASSERT_FALSE(lines.empty());
    int last_on = -MIN_GAP_FRAMES;
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
      if (i > 0)
        EXPECT_NE(lines[i].second, lines[i - 1].second);
      if (lines[i].second == "orca rumble 1 1 adapter 0")
      {
        EXPECT_GE(lines[i].first - last_on, MIN_GAP_FRAMES);
        last_on = lines[i].first;
      }
      std::size_t in_second = 0;
      for (const auto& line : lines)
        in_second += line.first >= lines[i].first && line.first < lines[i].first + 60;
      EXPECT_LE(in_second, 20u);
    }
  }
  static_assert(2 * (60 / MIN_GAP_FRAMES) <= 20);
}

TEST(OrcaRumble, AnOffNeverWaitsForTheGap)
{
  Tracker t;
  const auto lines = Feed(t, {true, false, false, false, false, false, false, false});
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[1], std::make_pair(OFF_FRAMES, std::string("orca rumble 1 0 adapter 0")));
  static_assert(OFF_FRAMES < MIN_GAP_FRAMES);
}

TEST(OrcaRumble, TheNextHitAfterALongBuzzIsOnAtOnce)
{
  Tracker t;
  std::vector<bool> motor(20, true);
  motor.insert(motor.end(), OFF_FRAMES, false);
  motor.push_back(true);
  const auto lines = Feed(t, motor);
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[1],
            std::make_pair(20 + OFF_FRAMES - 1, std::string("orca rumble 1 0 adapter 0")));
  EXPECT_EQ(lines[2], std::make_pair(20 + OFF_FRAMES, std::string("orca rumble 1 1 adapter 0")));
}

TEST(OrcaRumble, NoRumbleWithoutTheFocus)
{
  Tracker t;
  ASSERT_TRUE(t.Boundary(true, true, 0, ADAPTER0));
  for (int f = 0; f < MIN_GAP_FRAMES; ++f)
    t.Boundary(true, true, 0, ADAPTER0);
  std::vector<std::string> lines;
  for (int f = 0; f < 60; ++f)
  {
    if (const auto s = t.Boundary(true, false, 0, ADAPTER0))
      lines.push_back(FormatLine(*s));
  }
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0], "orca rumble 1 0 adapter 0");
}

TEST(OrcaRumble, NoAppDeviceNoRumble)
{
  Tracker t;
  EXPECT_TRUE(Feed(t, std::vector<bool>(120, true), Target{}).empty());
}

TEST(OrcaRumble, AnotherDeviceMidBuzzIsSaid)
{
  Tracker t;
  Feed(t, std::vector<bool>(MIN_GAP_FRAMES, true), ADAPTER0);
  const auto lines = Feed(t, {true}, PAD1);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].second, "orca rumble 1 1 pad 1");
  const auto off = Feed(t, std::vector<bool>(MIN_GAP_FRAMES + OFF_FRAMES, false), PAD1);
  ASSERT_EQ(off.size(), 1u);
  EXPECT_EQ(off[0].second, "orca rumble 1 0 pad 1");
}

TEST(OrcaRumble, AnOffDeviceChangeSaysNothing)
{
  Tracker t;
  EXPECT_TRUE(Feed(t, std::vector<bool>(30, false), ADAPTER0).empty());
  EXPECT_TRUE(Feed(t, std::vector<bool>(30, false), PAD1, true, 2).empty());
}

TEST(OrcaRumble, ThePortFollowsTheLocalPlayer)
{
  Tracker t;
  const auto first = Feed(t, std::vector<bool>(MIN_GAP_FRAMES, true), ADAPTER0, true, 1);
  ASSERT_EQ(first.size(), 1u);
  EXPECT_EQ(first[0].second, "orca rumble 2 1 adapter 0");
  const auto moved = Feed(t, {true}, ADAPTER0, true, 0);
  ASSERT_EQ(moved.size(), 1u);
  EXPECT_EQ(moved[0].second, "orca rumble 1 1 adapter 0");
}

TEST(OrcaRumble, AHaltSaysOffAtOnceAndOnlyOnce)
{
  Tracker t;
  ASSERT_TRUE(t.Boundary(true, true, 0, ADAPTER0));
  const std::optional<State> halted = t.Halt();
  ASSERT_TRUE(halted);
  EXPECT_EQ(FormatLine(*halted), "orca rumble 1 0 adapter 0");
  EXPECT_FALSE(t.Halt());
  const auto lines = Feed(t, std::vector<bool>(20, true));
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].first, MIN_GAP_FRAMES - 1);
  EXPECT_EQ(lines[0].second, "orca rumble 1 1 adapter 0");
}

TEST(OrcaRumble, NothingToHaltWhenOff)
{
  Tracker t;
  EXPECT_FALSE(t.Halt());
  Feed(t, {true, false, false, false, false, false, false, false, false});
  EXPECT_FALSE(t.Sent().on);
  EXPECT_FALSE(t.Halt());
}

TEST(OrcaRumble, NoLinesWithoutTheCap)
{
  const std::string offered = Orca::Status::OfferedCaps();
  EXPECT_TRUE(offered.ends_with(" rumble record hostui")) << offered;
  Orca::Status::SetAppCaps("join leave");
  EXPECT_FALSE(Orca::Status::Cap(Orca::Status::RUMBLE_CAP));
  Orca::UX::Rumble::Halt();
  Orca::Status::SetAppCaps("join rumble");
  EXPECT_TRUE(Orca::Status::Cap(Orca::Status::RUMBLE_CAP));
  Orca::Status::SetAppCaps("");
}

TEST(OrcaRumble, NoAppDeviceBeforeTheAppsStream)
{
  Orca::UX::StopControllerStream();
  EXPECT_FALSE(Orca::UX::LocalPad(true));
  EXPECT_FALSE(Orca::UX::LastLocalDevice());
}
