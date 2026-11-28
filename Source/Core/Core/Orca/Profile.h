// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Core/Orca/Mod.h"

namespace PowerPC
{
enum class CPUCore;
}
enum class ShaderCompilationMode : int;

struct Sram;
namespace Core
{
class CPUThreadGuard;
}

namespace Orca
{
namespace BootAudit
{
class Audit;
}
struct Profile
{
  std::string game_id;
  std::string title;
  u16 revision = 0;
  std::optional<u32> frame_hook;
  std::optional<u32> frame_hook_word;
  std::optional<u32> boot_nand_frame;
  std::optional<u32> jit_clear_frame;
  struct KeptCode
  {
    u32 address = 0;
    u32 game = 0;
    u32 loader = 0;
    bool operator==(const KeptCode&) const = default;
  };
  std::vector<KeptCode> kept_code;
  std::vector<std::pair<Config::Location, std::string>> forced;

  std::string launcher_hash;
  std::optional<u64> launcher_size;
  std::string disc;
  std::string sd_image;
  std::string sd_image_hash;
  std::optional<u64> sd_image_size;
  std::string sd_image_path;
  std::shared_ptr<BootAudit::Audit> boot_audit;
  std::optional<u64> save_title;
  std::string boot_game_id;
  std::optional<Mod::ModInfo> mod;

  bool IsLauncher() const { return !launcher_hash.empty(); }
};

std::string GetEnv(const char* name);

bool SessionActive();

enum class LatencyThread
{
  Cpu,
  Room,
  Controllers,
};
void PrioritizeThread(LatencyThread thread);

bool HarnessInputActive();
std::string HarnessReport(std::string json);
int TestPads();
bool TestSkipRender();
std::optional<int> TestNetDelayMs();
int TestNetJitterMs();
struct TestBlackout
{
  int from_s = 0;
  int to_s = 0;
  int room_before_s = 0;
  bool DirectBlocked(std::chrono::steady_clock::duration elapsed) const
  {
    return elapsed >= std::chrono::seconds(from_s) && elapsed < std::chrono::seconds(to_s);
  }
  bool RoomBlocked(std::chrono::steady_clock::duration elapsed) const
  {
    return elapsed >= std::chrono::seconds(from_s) &&
           elapsed < std::chrono::seconds(to_s - room_before_s);
  }
};
std::optional<TestBlackout> TestBlackoutConfig();
struct TestNetSpikes
{
  int gap_min_ms = 0;
  int gap_max_ms = 0;
  int length_min_ms = 0;
  int length_max_ms = 0;
  int from_s = 0;
  int to_s = 0x3fffffff;
  bool poisson = false;
};
std::optional<TestNetSpikes> TestNetSpikesConfig();
struct TestNetUplink
{
  int delay_ms = 0;
  int jitter_ms = 0;
  int from_s = 0;
  int to_s = 0x3fffffff;
};
std::optional<TestNetUplink> TestNetUplinkConfig();
int TestSnapshotEvery();
bool TestFullSnapshots();
bool TestNoAfp();
std::optional<PowerPC::CPUCore> TestCpuCore();
std::optional<int> TestShaderWaitMs();
std::string TestGateSpec();
std::string TestQueueSpec();
std::string TestQueuePickSpec();
std::string TestCharOrderSpec();
std::optional<int> ShaderWaitLimitS();
std::optional<u32> TestFxcFlags();
bool TestOverridesActive();

std::optional<Profile> LoadProfile(const std::string& game_id, std::optional<u16> revision,
                                   std::string* error);
std::optional<Profile> LoadProfileFrom(const std::string& sys_directory, const std::string& game_id,
                                       std::optional<u16> revision, std::string* error);

std::optional<Profile> LoadModProfile(const std::string& dir, std::string* error);
std::optional<Profile> LoadFlatProfile(const Mod::FlatBundle& bundle, std::string* error);
bool PrepareModBoot(Profile* profile, const std::string* executable_path,
                    const std::string& default_disc, const std::string& boot_game_id,
                    u16 boot_revision, std::string* error, bool* wrong_disc);

std::string LauncherProfileName();

std::string HashFile(const std::string& path, const std::atomic<bool>* stop = nullptr,
                     std::string* reason = nullptr);

std::string DiscOpenProblem(const std::string& disc_path);

bool PrepareLauncherBoot(Profile* profile, const std::string& executable_path,
                         const std::string& disc_path, std::string* error);

bool FrameHookLive(const Core::CPUThreadGuard& guard, const Profile& profile);

void SetActiveProfile(std::optional<Profile> profile);
const Profile* ActiveProfile();
bool JitClearDue();
u32 KeptInstruction(u32 physical_address, u32 hex);

std::unique_ptr<Config::ConfigLayerLoader> GenerateConfigLoader(const Profile& profile);

const Sram* SessionSram();

u64 HashSessionSave(u64 title_id);
void SetSeedSaveHash(u64 hash);
u64 GetSeedSaveHash();

void SetSeedSessionSave(bool seed);
bool SeedSessionSave();

std::optional<std::string> VerifyForcedSettings(const Profile& profile);

std::string DescribeForcedSettings(const Profile& profile);

bool SessionShaderModeAllowed(ShaderCompilationMode mode);
bool SessionShaderThreadsAllowed(int threads);
std::vector<Config::Location> UniversalForcedLocations();
std::string DescribeGraphics();
}
