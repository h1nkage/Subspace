// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "Core/Orca/Branding.h"

using Orca::BrandWords;

TEST(OrcaBranding, ProductName)
{
  EXPECT_EQ(Orca::PRODUCT_NAME, "Subspace");
}

TEST(OrcaBranding, ReplacesEachWord)
{
  EXPECT_EQ(BrandWords("Dolphin"), "Dolphin");
  EXPECT_EQ(BrandWords("Orca"), "Subspace");
  EXPECT_EQ(BrandWords("Subspace"), "Subspace");
  EXPECT_EQ(BrandWords("Dolphin has stopped."), "Dolphin has stopped.");
  EXPECT_EQ(BrandWords("Orca couldn't start the game"), "Subspace couldn't start the game");
}

TEST(OrcaBranding, ReplacesEveryWord)
{
  EXPECT_EQ(BrandWords("Orca is a fork of Dolphin"), "Subspace is a fork of Dolphin");
  EXPECT_EQ(BrandWords("Orca, Orca; Dolphin"), "Subspace, Subspace; Dolphin");
}

TEST(OrcaBranding, WordEdges)
{
  EXPECT_EQ(BrandWords("Reset Orca's files"), "Reset Subspace's files");
  EXPECT_EQ(BrandWords("Please restart Orca."), "Please restart Subspace.");
  EXPECT_EQ(BrandWords("(Orca)"), "(Subspace)");
  EXPECT_EQ(BrandWords("\"Orca\""), "\"Subspace\"");
  EXPECT_EQ(BrandWords("Orca\nOrca"), "Subspace\nSubspace");
  EXPECT_EQ(BrandWords("Orca: boot"), "Subspace: boot");
}

TEST(OrcaBranding, LeavesOtherWords)
{
  EXPECT_EQ(BrandWords(""), "");
  EXPECT_EQ(BrandWords("DolphinNoGUI"), "DolphinNoGUI");
  EXPECT_EQ(BrandWords("OrcaControls-RSBE01.txt"), "OrcaControls-RSBE01.txt");
  EXPECT_EQ(BrandWords("Orcas"), "Orcas");
  EXPECT_EQ(BrandWords("Orca2"), "Orca2");
  EXPECT_EQ(BrandWords("Dolphin_Emu"), "Dolphin_Emu");
  EXPECT_EQ(BrandWords("dolphin and orca"), "dolphin and orca");
  EXPECT_EQ(BrandWords("SuperDolphin"), "SuperDolphin");
  EXPECT_EQ(BrandWords("SuperOrca and Orcas"), "SuperOrca and Orcas");
}

TEST(OrcaBranding, LeavesPathsAndFileNames)
{
  EXPECT_EQ(BrandWords("/Volumes/Games/Library/Application Support/Dolphin/Config"),
            "/Volumes/Games/Library/Application Support/Dolphin/Config");
  EXPECT_EQ(BrandWords("C:\\Games\\Orca\\Sys"), "C:\\Games\\Orca\\Sys");
  EXPECT_EQ(BrandWords("Couldn't open Dolphin.ini"), "Couldn't open Dolphin.ini");
  EXPECT_EQ(BrandWords("Orca.app/Contents/MacOS/Orca"), "Orca.app/Contents/MacOS/Orca");
  EXPECT_EQ(BrandWords("Sys/Orca/RSBE01.ini"), "Sys/Orca/RSBE01.ini");
  EXPECT_EQ(BrandWords("~/.Dolphin"), "~/.Dolphin");
  EXPECT_EQ(BrandWords("Orca can't write to /tmp/Orca/x"), "Subspace can't write to /tmp/Orca/x");
  EXPECT_EQ(BrandWords("Dolphin can't write to /tmp/Dolphin/x"),
            "Dolphin can't write to /tmp/Dolphin/x");
}

TEST(OrcaBranding, TwiceIsOnce)
{
  for (const std::string_view text :
       {"Dolphin", "Orca", "Orca is a fork of Dolphin.", "DolphinNoGUI and Orca.app",
        "Dolphin's Orca's", "Subspace", "Subspace's settings",
        "OrcaDolphin Orca.Dolphin Orca Dolphin."})
  {
    const std::string once = BrandWords(text);
    EXPECT_EQ(BrandWords(once), once) << text;
    EXPECT_EQ(once.find("Subspace Subspace"), std::string::npos) << text;
  }
}
