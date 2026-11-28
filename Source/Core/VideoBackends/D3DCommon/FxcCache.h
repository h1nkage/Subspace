// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Crypto/SHA1.h"

namespace D3DCommon::FxcCache
{
using Key = Common::SHA1::Digest;

// The key for one compile. nullopt when the cache is off (shader cache disabled, or the compiler
// DLL couldn't be read).
std::optional<Key> MakeKey(std::string_view hlsl, std::string_view entry, std::string_view target,
                           u32 flags, std::string_view macros);

// MakeKey with an explicit compiler digest, for tests.
Key KeyFor(const Common::SHA1::Digest& compiler, std::string_view hlsl, std::string_view entry,
           std::string_view target, u32 flags, std::string_view macros);

// Cached FXC output for this key, from this run or an earlier one.
std::optional<std::vector<u8>> Find(const Key& key);

// Caches a compile's output in memory and appends it to the file.
void Store(const Key& key, std::span<const u8> bytecode);

// Logs cache stats and closes the file.
void Close();
}  // namespace D3DCommon::FxcCache
