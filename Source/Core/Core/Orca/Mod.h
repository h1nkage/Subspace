// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"

namespace Core
{
class CPUThreadGuard;
}

namespace Orca::Mod
{
inline constexpr int ENGINE_API = 1;
std::string_view EngineVersion();
bool VersionAtLeast(std::string_view version, std::string_view minimum);
inline constexpr std::string_view UPDATE_MESSAGE =
    "This pack needs Subspace {} or newer. Update the desktop app and try again.";
inline constexpr std::string_view SD_BUILDER = "fat2";
inline constexpr u64 SD_MAX_BYTES = u64{2} << 30;
inline constexpr u64 SD_MIN_BYTES = u64{64} << 20;
inline constexpr u64 SD_ALIGN = 32768;
inline constexpr size_t MAX_PATH_BYTES = 100;
inline constexpr size_t MAX_FILES = 20000;
inline constexpr u64 MAX_FILE_BYTES = (u64{4} << 30) - 1;
inline constexpr u64 MAX_MOD_JSON_BYTES = u64{64} << 10;
inline constexpr u64 MAX_TREE_JSON_BYTES = u64{16} << 20;
inline constexpr u64 MAX_PATCHES_BYTES = u64{1} << 20;
inline constexpr u64 MAX_LAUNCHER_BYTES = u64{64} << 20;
inline constexpr u64 MAX_JITWARM_BYTES = u64{16} << 20;
inline constexpr int MAX_JSON_DEPTH = 16;
inline constexpr u32 DEFAULT_FIRST_KEYFRAME_FRAME = 300;
inline constexpr size_t FLAT_MAX_FILES = 8;
inline constexpr size_t FLAT_MAX_PATCHES = 4;
inline constexpr std::string_view FLAT_MANIFEST = "mod.json";
inline constexpr std::string_view FLAT_SIGNATURE = "mod.sig";

struct TreeFile
{
  std::string path;
  std::string sha256;
  u64 bytes = 0;
  bool operator==(const TreeFile&) const = default;
};

struct Tree
{
  std::string hash;
  std::vector<TreeFile> files;

  const TreeFile* Find(std::string_view path) const;
  std::vector<const TreeFile*> Under(std::string_view folder) const;
};

std::optional<std::string> PathProblem(std::string_view path);
std::optional<std::string> FlatNameProblem(std::string_view name);
bool HostOnly(std::string_view path);
std::string TreeHash(std::span<const TreeFile> files);
std::string SdTreeHash(const Tree& tree, u64 bytes, u32 cluster_bytes = 0, u8 fats = 1);
std::string Sha256Hex(std::string_view data);
std::optional<Tree> ParseTree(std::string_view json, std::string* error);

enum class Builtin : u8
{
  None,
  BrawlRev2,
  BrawlRev2PPlus,
  BrawlRev2Results,
};
std::optional<Builtin> BuiltinFromName(std::string_view name);
std::string_view BuiltinName(Builtin builtin);

inline constexpr std::pair<u32, u32> PPLUS_CODESET_REGION{0x80550010, 0x805A0000};
struct PPlusGuardAddresses
{
  u32 rss_exdata = 0x80553594;
  u32 page_index = 0x80554320;
  u32 current_page = 0x805518D0;
  bool operator==(const PPlusGuardAddresses&) const = default;
};

struct RankedRules
{
  u8 stocks = 3;
  u8 time_minutes = 8;
  bool operator==(const RankedRules&) const = default;
};

struct KeptWord
{
  u32 address = 0;
  u32 game = 0;
  u32 loader = 0;
  bool operator==(const KeptWord&) const = default;
};

struct SdImageFile
{
  std::string file;
  u64 bytes = 0;
  std::string sha256;
  bool operator==(const SdImageFile&) const = default;
};

struct OriginWord
{
  u32 address = 0;
  u32 word = 0;
  bool operator==(const OriginWord&) const = default;
};

struct OriginDecl
{
  u32 frame = 0;
  std::vector<OriginWord> words;
  u32 settle = 30;
};

struct Manifest
{
  int format = 0;
  int engine_api = 0;
  std::string min_orca;
  std::string title;
  std::string disc_id;
  u16 disc_revision = 0;
  std::string launcher;
  std::optional<u64> sd_bytes;
  u32 sd_cluster_bytes = 0;
  u8 sd_fats = 1;
  u32 frame_hook = 0;
  u32 frame_hook_word = 0;
  std::optional<u32> boot_nand_frame;
  u32 first_keyframe_frame = DEFAULT_FIRST_KEYFRAME_FRAME;
  std::optional<u32> jit_clear_frame;
  std::optional<OriginDecl> origin;
  std::vector<KeptWord> keep_game_code;
  std::optional<u64> save_title;
  std::vector<std::pair<std::string, std::string>> forced;
  Builtin builtin = Builtin::None;
  std::optional<PPlusGuardAddresses> pplus_guards;
  std::optional<RankedRules> ranked_rules;
  std::vector<std::string> rulesets;
  std::vector<std::string> offers;
  std::string listing;
  u64 serial = 0;
  std::optional<SdImageFile> sd_image;
  std::vector<std::string> patches;
  std::string jitwarm;
  std::vector<std::string> FlatFiles() const;
};

std::span<const std::string_view> ForcedAllowlist();
std::span<const std::string_view> OfferableCaps();
u64 DiscTitleId(std::string_view disc_id);

std::optional<Manifest> ParseManifest(std::string_view json, std::string* error);

bool Requested();
std::string Dir();

struct FlatBundle;

struct ModInfo
{
  std::string dir;
  Tree tree;
  Manifest manifest;
  std::string sd_image_hash;
  bool flat = false;
  std::vector<std::pair<std::string, std::string>> flat_patches;
  std::string flat_jitwarm;

  std::string JitWarmPath() const;
  bool EngineTextures() const { return flat; }
};
const ModInfo* ActiveMod();

struct SdImage
{
  std::string path;
  std::string hash;
  bool built = false;
};
std::optional<SdImage> PrepareSdImage(const ModInfo& mod, const std::string& cache_dir,
                                      std::string* error);
struct SdSource
{
  std::string image_path;
  std::string source_path;
  u64 bytes = 0;
};
bool BuildSdImage(std::span<const SdSource> files, u64 bytes, const std::string& out_path,
                  std::string* error, u32 cluster_bytes = 0, u8 fats = 1);

struct GuardWord
{
  u32 address = 0;
  u32 word = 0;
};
std::span<const GuardWord> BuiltinGuards(Builtin builtin);
bool GuardsHold(Builtin builtin, const std::function<std::optional<u32>(u32)>& read,
                const std::optional<PPlusGuardAddresses>& pplus_guards = std::nullopt);
void RefreshGuards(const Core::CPUThreadGuard& guard, std::optional<int> frame = std::nullopt);
void ResetGuards();
void NoteGuards(bool held, std::optional<int> frame);
bool GuardsDecided();
bool BuiltinOn(Builtin builtin);
Builtin DeclaredBuiltin();
bool GuardsFailed();
bool Offers(std::string_view cap);

u32 FirstKeyframeFrame();
}
