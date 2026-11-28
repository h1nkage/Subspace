// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cmath>
#include <optional>

#include <gtest/gtest.h>

#include "Core/Orca/UX/Overlay.h"
#include "VideoCommon/PresentLayout.h"

using VideoCommon::ChooseLayout;
using VideoCommon::LayoutChoice;
using VideoCommon::LayoutBox;
using VideoCommon::LayoutBoxIn;
using VideoCommon::LayoutHint;
using VideoCommon::PictureInBackbuffer;
using VideoCommon::PlaceInBox;

namespace
{
LayoutBox ChooseLayoutBox(const std::optional<LayoutHint>& latest,
                          const std::optional<LayoutHint>& applied, int bb_w, int bb_h)
{
  return ChooseLayout(latest, applied, bb_w, bb_h).box;
}

constexpr LayoutHint WHOLE{1280, 720, 0, 0, 1280, 720};
constexpr LayoutHint CUT{840, 720, -440, 0, 1280, 720};
}

TEST(OrcaLayout, NoHintIsTheWholeBackbuffer)
{
  EXPECT_EQ(ChooseLayoutBox(std::nullopt, std::nullopt, 1280, 720), (LayoutBox{0, 0, 1280, 720}));
  EXPECT_EQ(ChooseLayoutBox(std::nullopt, CUT, 840, 720), (LayoutBox{0, 0, 840, 720}));
  EXPECT_EQ(ChooseLayoutBox(WHOLE, std::nullopt, 1280, 720), (LayoutBox{0, 0, 1280, 720}));
}

TEST(OrcaLayout, ThePanelsCutKeepsThePictureWhereItWas)
{
  const LayoutBox box = ChooseLayoutBox(CUT, WHOLE, 840, 720);
  EXPECT_EQ(box, (LayoutBox{-440, 0, 1280, 720}));
  const MathUtil::Rectangle<int> pic = PlaceInBox(box, 1280, 720);
  EXPECT_EQ(pic, (MathUtil::Rectangle<int>(-440, 0, 840, 720)));
  EXPECT_EQ(PlaceInBox(box, 960, 720), (MathUtil::Rectangle<int>(-280, 0, 680, 720)));
  const LayoutHint below_toast{1280, 600, 0, -120, 1280, 720};
  EXPECT_EQ(PlaceInBox(ChooseLayoutBox(below_toast, WHOLE, 1280, 600), 1280, 720),
            (MathUtil::Rectangle<int>(0, -120, 1280, 600)));
}

TEST(OrcaLayout, PlacementInTheWholeWindowIsDolphins)
{
  for (const int w : {640, 641, 1280, 1281, 1919})
  {
    for (const int h : {360, 480, 481, 720, 1080})
    {
      for (const int dw : {w, w - 1, w / 2, (h * 4) / 3})
      {
        const int dh = h - (dw % 3);
        const MathUtil::Rectangle<int> r =
            PlaceInBox(LayoutBox{0, 0, static_cast<float>(w), static_cast<float>(h)}, dw, dh);
        EXPECT_EQ(r.left, static_cast<int>(std::round(w / 2.0 - dw / 2.0))) << w << " " << dw;
        EXPECT_EQ(r.top, static_cast<int>(std::round(h / 2.0 - dh / 2.0))) << h << " " << dh;
        EXPECT_EQ(r.GetWidth(), dw);
        EXPECT_EQ(r.GetHeight(), dh);
      }
    }
  }
}

TEST(OrcaLayout, TheBackbufferFollowsTheWindowLate)
{
  std::optional<LayoutHint> applied;
  const auto present = [&](const std::optional<LayoutHint>& latest, int bb_w, int bb_h) {
    const LayoutChoice choice = ChooseLayout(latest, applied, bb_w, bb_h);
    applied = choice.hint;
    return choice.box;
  };
  EXPECT_EQ(present(WHOLE, 1280, 720), (LayoutBox{0, 0, 1280, 720}));
  EXPECT_EQ(applied, WHOLE);
  EXPECT_EQ(present(CUT, 1280, 720), (LayoutBox{0, 0, 1280, 720}));
  EXPECT_EQ(applied, WHOLE);
  EXPECT_EQ(present(CUT, 840, 720), (LayoutBox{-440, 0, 1280, 720}));
  EXPECT_EQ(applied, CUT);
  EXPECT_EQ(present(WHOLE, 840, 720), (LayoutBox{-440, 0, 1280, 720}));
  EXPECT_EQ(present(WHOLE, 1280, 720), (LayoutBox{0, 0, 1280, 720}));
  applied.reset();
  EXPECT_EQ(present(CUT, 1280, 720), (LayoutBox{0, 0, 1280, 720}));
  EXPECT_FALSE(applied);
  EXPECT_EQ(present(CUT, 840, 720), (LayoutBox{-440, 0, 1280, 720}));
  EXPECT_EQ(present(std::nullopt, 840, 720), (LayoutBox{0, 0, 840, 720}));
  EXPECT_FALSE(applied);
  EXPECT_EQ(ChooseLayoutBox(CUT, std::nullopt, 1280, 720), (LayoutBox{0, 0, 1280, 720}));
  EXPECT_EQ(ChooseLayoutBox(CUT, CUT, 1280, 720), (LayoutBox{0, 0, 1280, 720}));
}

TEST(OrcaLayout, TheHintIsScaledToTheBackbuffer)
{
  ASSERT_TRUE(LayoutBoxIn(CUT, 1680, 1440));
  EXPECT_EQ(*LayoutBoxIn(CUT, 1680, 1440), (LayoutBox{-880, 0, 2560, 1440}));
  const LayoutHint odd{841, 721, -440, 0, 1281, 721};
  const std::optional<LayoutBox> box = LayoutBoxIn(odd, 840, 720);
  ASSERT_TRUE(box);
  EXPECT_NEAR(box->x, -440 * 840.0 / 841, 1e-3);
  EXPECT_NEAR(box->w, 1281 * 840.0 / 841, 1e-3);
  EXPECT_NEAR(box->h, 720, 1e-3);
  EXPECT_FALSE(LayoutBoxIn(CUT, 1280, 720));
  EXPECT_FALSE(LayoutBoxIn(WHOLE, 840, 720));
  EXPECT_FALSE(LayoutBoxIn(LayoutHint{0, 720, 0, 0, 1280, 720}, 840, 720));
  EXPECT_FALSE(LayoutBoxIn(LayoutHint{840, 720, 0, 0, 0, 720}, 840, 720));
  EXPECT_FALSE(LayoutBoxIn(CUT, 0, 720));
}

TEST(OrcaLayout, AWindowOnlyOnTheBarDrawsNoPicture)
{
  using Rect = MathUtil::Rectangle<int>;
  const LayoutHint bar{120, 720, -1160, 0, 1280, 720};
  const Rect pic = PlaceInBox(ChooseLayoutBox(bar, std::nullopt, 120, 720), 960, 720);
  EXPECT_EQ(pic, Rect(-1000, 0, -40, 720));
  EXPECT_FALSE(PictureInBackbuffer(pic, 120, 720));
  EXPECT_FALSE(PictureInBackbuffer(Rect(0, -720, 1280, 0), 1280, 720));
  EXPECT_FALSE(PictureInBackbuffer(Rect(0, 720, 1280, 1440), 1280, 720));
  EXPECT_FALSE(PictureInBackbuffer(Rect(1280, 0, 2560, 720), 1280, 720));
  EXPECT_FALSE(PictureInBackbuffer(Rect(0, 0, 0, 720), 1280, 720));
  EXPECT_TRUE(PictureInBackbuffer(Rect(-1000, 0, 1, 720), 120, 720));
  EXPECT_TRUE(
      PictureInBackbuffer(PlaceInBox(ChooseLayoutBox(CUT, WHOLE, 840, 720), 960, 720), 840, 720));
  EXPECT_TRUE(PictureInBackbuffer(Rect(0, 0, 1280, 720), 1280, 720));
}

TEST(OrcaLayout, VisiblePictureIsThePictureInTheWindow)
{
  using Orca::UX::PictureArea;
  using Orca::UX::VisiblePicture;
  const auto same = [](const PictureArea& a, const PictureArea& b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
  };
  EXPECT_TRUE(same(VisiblePicture({0, 0, 1280, 720}, 1280, 720), {0, 0, 1280, 720}));
  EXPECT_TRUE(same(VisiblePicture({160, 0, 960, 720}, 1280, 720), {160, 0, 960, 720}));
  EXPECT_TRUE(same(VisiblePicture({-440, 0, 1280, 720}, 840, 720), {0, 0, 840, 720}));
  EXPECT_TRUE(same(VisiblePicture({-280, 0, 960, 720}, 840, 720), {0, 0, 680, 720}));
  EXPECT_TRUE(same(VisiblePicture({0, -120, 1280, 720}, 1280, 600), {0, 0, 1280, 600}));
  EXPECT_TRUE(same(VisiblePicture({-2000, 0, 1280, 720}, 840, 720), {-2000, 0, 1280, 720}));
  EXPECT_TRUE(same(VisiblePicture({0, 0, 1280, 720}, 0, 0), {0, 0, 1280, 720}));
  const PictureArea nan{std::nanf(""), 0, 1280, 720};
  EXPECT_TRUE(std::isnan(VisiblePicture(nan, 840, 720).x));
}
