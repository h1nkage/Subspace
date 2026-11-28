// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/Logging/LogManager.h"
#include "Core/Orca/BootLogs.h"

namespace
{
using Common::Log::LogManager;
using Common::Log::LogType;
using Orca::BootLogs::EndAtFirstFrame;

class MemoryLayer final : public Config::ConfigLayerLoader
{
public:
  explicit MemoryLayer(Config::LayerType layer) : ConfigLayerLoader(layer) {}
  void Load(Config::Layer*) override {}
  void Save(Config::Layer*) override {}
};

Config::Info<bool> Category(const char* name)
{
  return {{Config::System::Logger, "Logs", name}, false};
}

class OrcaBootLogs : public testing::Test
{
protected:
  void SetUp() override
  {
    m_old_user = File::GetUserPath(D_USER_IDX);
    m_dir = File::CreateTempDir();
    File::SetUserPath(D_USER_IDX, m_dir + DIR_SEP);
    Config::Init();
    Config::AddLayer(std::make_unique<MemoryLayer>(Config::LayerType::Base));
    Config::AddLayer(std::make_unique<MemoryLayer>(Config::LayerType::CommandLine));
  }
  void TearDown() override
  {
    LogManager::Shutdown();
    Config::Shutdown();
    File::SetUserPath(D_USER_IDX, m_old_user);
    File::DeleteDirRecursively(m_dir);
  }
  static void LoggerIni(const char* name, bool on) { Config::SetBase(Category(name), on); }
  static void CommandLine(const char* name)
  {
    Config::Set(Config::LayerType::CommandLine, Category(name), true);
  }
  static LogManager& Logs()
  {
    LogManager::Init();
    return *LogManager::GetInstance();
  }

private:
  std::string m_old_user, m_dir;
};
}

TEST_F(OrcaBootLogs, TheAppsBootLogsEndAtTheFirstFrame)
{
  for (const char* name : {"OSREPORT", "IOS", "IOS_SD", "MEMMAP"})
  {
    LoggerIni(name, false);
    CommandLine(name);
  }
  CommandLine("ROLLBACK");
  LogManager& logs = Logs();
  ASSERT_TRUE(logs.IsEnabled(LogType::IOS_SD));
  ASSERT_FALSE(logs.IsEnabled(LogType::MEMMAP));

  EXPECT_EQ(EndAtFirstFrame(), "OSREPORT, IOS, IOS_SD");
  for (const LogType type : Orca::BootLogs::TYPES)
    EXPECT_FALSE(logs.IsEnabled(type)) << logs.GetShortName(type);
  EXPECT_TRUE(logs.IsEnabled(LogType::ROLLBACK));
  EXPECT_EQ(EndAtFirstFrame(), "");
}

TEST_F(OrcaBootLogs, WhatLoggerIniTurnsOnStaysOn)
{
  LoggerIni("IOS", true);
  LoggerIni("IOS_SD", true);
  CommandLine("IOS_SD");
  CommandLine("OSREPORT");
  CommandLine("MI");
  LogManager& logs = Logs();

  EXPECT_EQ(EndAtFirstFrame(), "OSREPORT, MI");
  EXPECT_TRUE(logs.IsEnabled(LogType::IOS));
  EXPECT_TRUE(logs.IsEnabled(LogType::IOS_SD));
  EXPECT_FALSE(logs.IsEnabled(LogType::OSREPORT));
  EXPECT_FALSE(logs.IsEnabled(LogType::MEMMAP));
}

TEST_F(OrcaBootLogs, NothingOnNothingEnds)
{
  Logs();
  EXPECT_EQ(EndAtFirstFrame(), "");
}

TEST(OrcaBootLogsNoManager, WithoutALogManagerNothingHappens)
{
  ASSERT_EQ(LogManager::GetInstance(), nullptr);
  EXPECT_EQ(EndAtFirstFrame(), "");
}
