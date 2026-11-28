// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Mod.h"

namespace Orca::Mod
{
inline constexpr std::string_view FLAT_SIGNED_HEADER = "orca-flat-bundle 1\n";
inline constexpr u64 MAX_SIG_BYTES = 512;
inline constexpr std::string_view SD_HASH_HEADER = "orca-chunked-sha256 1\n";
inline constexpr u64 SD_HASH_CHUNK_BYTES = u64{64} << 20;
inline constexpr u64 MIN_FLAT_SERIAL = 1;
inline constexpr size_t MAX_FOLDER_ENTRIES = 64;

using PublicKey = std::array<u8, 32>;
using Signature = std::array<u8, 64>;

struct FlatBundle
{
  std::string dir;
  Manifest manifest;
  Tree tree;
  std::vector<std::pair<std::string, std::string>> patches;
  std::string jitwarm;
  std::string sd_image_hash;
  std::optional<PublicKey> signer;
};

std::string FlatSignedMessage(std::span<const TreeFile> files);
std::string FlatTreeHash(std::span<const TreeFile> files, std::string_view jitwarm);
std::optional<std::pair<PublicKey, Signature>> ParseSignature(std::string_view text);
bool VerifySignature(const PublicKey& key, const Signature& signature, std::string_view message);
std::string ChunkedSha256File(const std::string& path, u64 bytes);

std::span<const PublicKey> TrustedKeys();
void TrustKeysForTests(std::span<const PublicKey> keys);
void ResetTrustedKeysForTests();
bool IsTestKey(const PublicKey& key);
std::string KeyHex(const PublicKey& key);

struct BootLauncher
{
  std::string name;
  std::span<const u8> bytes;
};

std::optional<FlatBundle> LoadFlatBundle(const std::string& dir, std::span<const PublicKey> keys,
                                         std::string* error, const BootLauncher* boot = nullptr,
                                         std::string_view listing = {});

void SetLocalPack(std::string folder);
bool LocalPackRequested();

std::string FlatBundleDir(const std::string* executable_path);

enum class BundleKind
{
  None,
  Tree,
  Flat,
  Refused,
};
void ResolveBundle(const std::string* executable_path, std::span<const u8> launcher_bytes = {});
void ResolveBundleForTests(const std::string* executable_path, std::span<const u8> launcher_bytes,
                          bool jailed);
std::optional<BundleKind> ResolvedKind();
std::string ResolvedDir();
const FlatBundle* ResolvedFlat();
std::string ResolvedError();
void ResetResolvedBundle();
}
