// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Status.h"
#include "Core/Rollback/OnlineMatch.h"

namespace
{
void SetEnv(const char* name, const char* value)
{
#ifdef _WIN32
  _putenv_s(name, value ? value : "");
#else
  if (value)
    setenv(name, value, 1);
  else
    unsetenv(name);
#endif
}

class SessionEnv
{
public:
  explicit SessionEnv(const char* dev_game)
  {
    Save("ORCA_SESSION", &m_session);
    Save("ORCA_TEST_DEV_GAME", &m_dev_game);
    Save("SUBSPACE_BRIDGE", &m_bridge);
    SetEnv("ORCA_SESSION", "1");
    SetEnv("ORCA_TEST_DEV_GAME", dev_game);
    SetEnv("SUBSPACE_BRIDGE", nullptr);
  }
  ~SessionEnv()
  {
    SetEnv("ORCA_SESSION", m_session ? m_session->c_str() : nullptr);
    SetEnv("ORCA_TEST_DEV_GAME", m_dev_game ? m_dev_game->c_str() : nullptr);
    SetEnv("SUBSPACE_BRIDGE", m_bridge ? m_bridge->c_str() : nullptr);
  }
  SessionEnv(const SessionEnv&) = delete;
  SessionEnv& operator=(const SessionEnv&) = delete;

private:
  static void Save(const char* name, std::optional<std::string>* out)
  {
    if (const char* value = std::getenv(name))
      *out = value;
  }
  std::optional<std::string> m_session, m_dev_game, m_bridge;
};

void Clear()
{
  Orca::Online::TakePrepareJoin();
  Orca::Online::TakeLetIn();
  Orca::Online::TakeJoinRequest();
  Orca::Online::TakeLeaveRequest();
  Rollback::OnlineMatch::EndPause();
  Rollback::OnlineMatch::TakePausedTime();
}
}

TEST(OrcaPause, SessionWithoutARoomMayPause)
{
  SessionEnv env(nullptr);
  Clear();
  ASSERT_FALSE(Orca::Online::Enabled());
  EXPECT_TRUE(Rollback::OnlineMatch::SoloPauseAllowed());
  EXPECT_FALSE(Rollback::OnlineMatch::Pausing());
  EXPECT_TRUE(Rollback::OnlineMatch::BeginPause());
  EXPECT_TRUE(Rollback::OnlineMatch::Pausing());
  Rollback::OnlineMatch::EndPause();
  EXPECT_FALSE(Rollback::OnlineMatch::Pausing());
}

TEST(OrcaPause, OnlineSessionWaitsForItsFirstBoundary)
{
  SessionEnv env("orca-pause-test");
  Clear();
  ASSERT_TRUE(Orca::Online::Enabled());
  EXPECT_FALSE(Rollback::OnlineMatch::SoloPauseAllowed());
  EXPECT_FALSE(Rollback::OnlineMatch::BeginPause());
  EXPECT_FALSE(Rollback::OnlineMatch::Pausing());
  Rollback::OnlineMatch::EndPause();
  EXPECT_EQ(Rollback::OnlineMatch::TakePausedTime().count(), 0);
}

TEST(OrcaPause, DropInRequestsWaitForTheNextBoundary)
{
  SessionEnv env(nullptr);
  Clear();
  Orca::Status::SetAppCaps("");
  EXPECT_FALSE(Orca::Online::DropInPending());

  Orca::Online::RequestJoin("abcdef");
  Orca::Online::RequestLeave();
  EXPECT_FALSE(Orca::Online::DropInPending()) << "no caps yet: the boundary holds them";
  Orca::Status::SetAppCaps("join leave stats");
  EXPECT_TRUE(Orca::Online::DropInPending());
  EXPECT_EQ(Orca::Online::TakeJoinRequest(), "abcdef");
  EXPECT_TRUE(Orca::Online::DropInPending());
  EXPECT_TRUE(Orca::Online::TakeLeaveRequest());
  EXPECT_FALSE(Orca::Online::DropInPending());

  Orca::Online::PrepareJoin();
  EXPECT_FALSE(Orca::Online::DropInPending());
  EXPECT_TRUE(Orca::Online::TakePrepareJoin());
  Orca::Status::SetAppCaps("");
}

TEST(OrcaPause, PausedTimeIsLeftOutOfTheStats)
{
  SessionEnv env(nullptr);
  Clear();
  ASSERT_TRUE(Rollback::OnlineMatch::BeginPause());
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  ASSERT_TRUE(Rollback::OnlineMatch::BeginPause());
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  Rollback::OnlineMatch::EndPause();
  const auto paused = Rollback::OnlineMatch::TakePausedTime();
  EXPECT_GE(paused, std::chrono::milliseconds(60));
  EXPECT_LT(paused, std::chrono::seconds(5));
  EXPECT_EQ(Rollback::OnlineMatch::TakePausedTime().count(), 0);
  Rollback::OnlineMatch::EndPause();
  EXPECT_EQ(Rollback::OnlineMatch::TakePausedTime().count(), 0);
}

TEST(OrcaFightHold, LetInIsAOneShotBoundaryRequest)
{
  Orca::Online::TakeLetIn();
  Orca::Online::LetIn();
  EXPECT_TRUE(Orca::Online::TakeLetIn());
  EXPECT_FALSE(Orca::Online::TakeLetIn());
}
