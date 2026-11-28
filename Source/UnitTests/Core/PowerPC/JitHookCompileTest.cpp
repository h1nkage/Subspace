// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdlib>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FPURoundMode.h"
#include "Common/FileUtil.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/JitCommon/JitCache.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "UICommon/UICommon.h"

#include <gtest/gtest.h>

namespace
{
#if defined(_M_X86_64)
constexpr PowerPC::CPUCore HOST_JIT = PowerPC::CPUCore::JIT64;
#else
constexpr PowerPC::CPUCore HOST_JIT = PowerPC::CPUCore::JITARM64;
#endif

constexpr u32 STMW_R0_0_R5 = 0xBC050000;
constexpr u32 BRANCH_TO_SELF = 0x48000000;
constexpr u32 CODE_BASE = 0x00100000;
constexpr u32 CODE_STRIDE = 0x00020000;
constexpr u32 COPIES = (0x01800000 - CODE_BASE) / CODE_STRIDE;
constexpr std::size_t BIG_BLOCK = 2 * 1024 * 1024;

void SetEnv(const char* name, const char* value)
{
#ifdef _WIN32
  _putenv_s(name, value);
#else
  if (*value)
    setenv(name, value, 1);
  else
    unsetenv(name);
#endif
}

class OrcaJitHookCompile : public testing::Test
{
protected:
  static void SetUpTestSuite()
  {
    s_had_session = std::getenv("ORCA_SESSION") != nullptr;
    if (!s_had_session)
      SetEnv("ORCA_SESSION", "1");
    s_user_dir = File::CreateTempDir();
    Core::DeclareAsCPUThread();
    UICommon::SetUserDirectory(s_user_dir);
    Config::Init();
    SConfig::Init();
    Config::SetCurrent(Config::MAIN_FASTMEM, false);
    Config::SetCurrent(Config::MAIN_FASTMEM_ARENA, false);
    auto& system = Core::System::GetInstance();
    system.GetMemory().Init();
    system.GetPowerPC().Init(HOST_JIT);
    system.GetCoreTiming().Init();
    const Core::CPUThreadGuard guard(system);
    system.GetJitInterface().ClearCache(guard);
  }

  static void TearDownTestSuite()
  {
    auto& system = Core::System::GetInstance();
    system.GetCoreTiming().Shutdown();
    system.GetPowerPC().Shutdown();
    system.GetMemory().Shutdown();
    SConfig::Shutdown();
    Config::Shutdown();
    Common::FPU::LoadDefaultSIMDState();
    Core::UndeclareAsCPUThread();
    File::DeleteDirRecursively(s_user_dir);
    if (!s_had_session)
      SetEnv("ORCA_SESSION", "");
  }

  static JitBase& Jit()
  {
    return *static_cast<JitBase*>(Core::System::GetInstance().GetJitInterface().GetCore());
  }

  static JitBlock* Find(u32 address)
  {
    auto& ppc = Core::System::GetInstance().GetPPCState();
    return Jit().GetBlockCache()->GetBlockFromStartAddress(address, ppc.feature_flags);
  }

  static void WriteBlock(u32 address, u32 count)
  {
    auto& memory = Core::System::GetInstance().GetMemory();
    for (u32 i = 0; i < count; ++i)
      memory.Write_U32(STMW_R0_0_R5, address + i * 4);
    memory.Write_U32(BRANCH_TO_SELF, address + count * 4);
  }

  static std::size_t HostBytes(const JitBlock& block)
  {
    return static_cast<std::size_t>(block.near_end - block.near_begin) +
           static_cast<std::size_t>(block.far_end - block.far_begin);
  }

  static inline bool s_had_session = false;
  static inline std::string s_user_dir;
};
}

TEST_F(OrcaJitHookCompile, ABlockTooBigForTheFreeSpaceIsDroppedNotACacheClear)
{
  auto& system = Core::System::GetInstance();
  auto& ppc = system.GetPPCState();
  ppc.msr.Hex = 0;
  system.GetPowerPC().MSRUpdated();
  ppc.gpr[5] = 0x00F00000;
  const Core::CPUThreadGuard guard(system);
  system.GetJitInterface().ClearCache(guard);

  u32 count = 1000;
  std::size_t bytes = 0;
  while (true)
  {
    WriteBlock(CODE_BASE, count);
    ASSERT_TRUE(Jit().CanCompileFromHook());
    ASSERT_TRUE(Jit().CompileFromHook(CODE_BASE));
    const JitBlock* block = Find(CODE_BASE);
    ASSERT_NE(block, nullptr);
    bytes = HostBytes(*block);
    system.GetJitInterface().ClearCache(guard);
    if (bytes >= BIG_BLOCK || count >= 30000)
      break;
    count = std::min<u32>(30000, count * 2);
  }
  ASSERT_GE(bytes, BIG_BLOCK) << count << " stmw make only " << bytes << " host bytes";

  std::vector<u32> compiled;
  bool dropped = false;
  for (u32 i = 0; i < COPIES && Jit().CanCompileFromHook(); ++i)
  {
    const u32 address = CODE_BASE + i * CODE_STRIDE;
    WriteBlock(address, count);
    if (!Jit().CompileFromHook(address))
    {
      dropped = true;
      EXPECT_EQ(Find(address), nullptr);
      for (const u32 earlier : compiled)
        EXPECT_NE(Find(earlier), nullptr) << fmt::format("{:#x}", earlier);
      Jit().Jit(address);
      EXPECT_NE(Find(address), nullptr);
      break;
    }
    ASSERT_NE(Find(address), nullptr);
    if (!compiled.empty())
    {
      ASSERT_NE(Find(compiled.front()), nullptr)
          << fmt::format("the hook's compile of {:#x} cleared the code cache", address);
    }
    compiled.push_back(address);
  }
  EXPECT_TRUE(dropped) << compiled.size() << " blocks of " << bytes << " bytes all fit";
  system.GetJitInterface().ClearCache(guard);
}
