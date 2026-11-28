// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string>

#include <gtest/gtest.h>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Core/Orca/LocalStore.h"

namespace LocalStore = Orca::LocalStore;

namespace
{
class UserFolder
{
public:
  UserFolder() : m_old(File::GetUserPath(D_USER_IDX)), m_dir(File::CreateTempDir())
  {
    File::SetUserPath(D_USER_IDX, m_dir + DIR_SEP);
    File::CreateFullPath(File::GetUserPath(D_CONFIG_IDX));
  }
  ~UserFolder()
  {
    File::SetUserPath(D_USER_IDX, m_old);
    File::DeleteDirRecursively(m_dir);
  }

private:
  std::string m_old;
  std::string m_dir;
};
}

TEST(OrcaLocalStore, SeenReadsTheFileThePlayerAlreadyHas)
{
  UserFolder user;
  ASSERT_TRUE(File::WriteStringToFile(LocalStore::SeenPath(), "other\r\n  hold-b-leave \n"));
  EXPECT_TRUE(LocalStore::Seen("hold-b-leave"));
  EXPECT_TRUE(LocalStore::Seen("other"));
  EXPECT_FALSE(LocalStore::Seen("hold-b"));
}

TEST(OrcaLocalStore, TheSeenFileKeepsOneKeyPerLine)
{
  constexpr std::string_view key = "hold-b-leave";
  EXPECT_FALSE(LocalStore::SeenIn("", key));
  EXPECT_FALSE(LocalStore::SeenIn("hold-b-leaves\n", key));
  EXPECT_TRUE(LocalStore::SeenIn("hold-b-leave", key));
  EXPECT_TRUE(LocalStore::SeenIn("other\r\n  hold-b-leave \r\n", key));
  EXPECT_EQ(LocalStore::SeenWith("", key), "hold-b-leave\n");
  EXPECT_EQ(LocalStore::SeenWith("other", key), "other\nhold-b-leave\n");
  EXPECT_EQ(LocalStore::SeenWith("hold-b-leave\nother\n", key), "hold-b-leave\nother\n");
  EXPECT_TRUE(LocalStore::SeenIn(LocalStore::SeenWith("a\nb\n", key), key));
}

TEST(OrcaLocalStore, MarkSeenKeepsTheKeyBesideTheOthers)
{
  UserFolder user;
  const std::string path = LocalStore::SeenPath();
  EXPECT_EQ(path, File::GetUserPath(D_CONFIG_IDX) + "OrcaSeen.txt");
  ASSERT_TRUE(File::WriteStringToFile(path, "other"));
  EXPECT_TRUE(LocalStore::MarkSeen("new-tip"));
  std::string text;
  ASSERT_TRUE(File::ReadFileToString(path, text));
  EXPECT_EQ(text, "other\nnew-tip\n");
  EXPECT_TRUE(LocalStore::Seen("new-tip"));
  EXPECT_TRUE(LocalStore::Seen("other"));
  EXPECT_TRUE(LocalStore::MarkSeen("other"));
  ASSERT_TRUE(File::ReadFileToString(path, text));
  EXPECT_EQ(text, "other\nnew-tip\n");
}
