// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/RelayRoom.h"
#include "Core/Orca/Session/Replay.h"
#include "Core/Rollback/Rollback.h"

namespace Orca::Net
{
struct NandEntry
{
  std::string path;
  bool directory = false;
  std::vector<u8> data;
  bool reference = false;
  u64 hash = 0;
};

u64 NandHash(const std::vector<u8>& data);
std::map<std::string, u64> HashNandTree(const std::string& root);
bool ResolveNandReferences(const std::string& root, std::vector<NandEntry>* entries,
                           std::string* error);

bool ReadNandTree(const std::string& root, std::vector<NandEntry>* entries);
bool ReplaceNandTree(const std::string& root, const std::vector<NandEntry>& entries);

std::vector<u8> PackKeyframe(int frame, const ReplayArchive& replay);
bool UnpackKeyframe(const std::vector<u8>& blob, int* frame, ReplayArchive* replay);
std::string KeyframeHash(const std::vector<u8>& blob);

struct ReplayStreamInfo
{
  std::string compat;
  std::string build;
  std::string game_id;
  u64 origin_hash = 0;
  int first_frame = -1;
  u32 segment = 0;
  std::string room;
  bool seat_labels = false;
};

struct ReplayChunk
{
  int first = 0;
  int live = 0;
  std::vector<ReplayFrame> frames;
  std::vector<std::pair<int, u64>> checksums;
};

std::vector<u8> PackReplayStreamHeader(const ReplayStreamInfo& info);
std::vector<u8> PackReplayChunk(const ReplayChunk& chunk, bool history_start);

std::vector<u8> PackReplayStream(const ReplayStreamInfo& info, const ReplayArchive& replay,
                                 const std::map<int, u64>& checksums);
constexpr size_t REPLAY_PACKED_MAX = 1u << 20;
constexpr size_t REPLAY_PACKED_PIECE_MAX = 32u << 10;

class ReplayStreamReader
{
public:
  explicit ReplayStreamReader(std::string compat = {}) : m_compat(std::move(compat)) {}
  bool Feed(const u8* data, size_t size, std::string* error);
  void Resume();
  const std::optional<ReplayStreamInfo>& Info() const { return m_info; }
  ReplayArchive& Archive() { return m_archive; }
  const std::map<int, u64>& Checksums() const { return m_checksums; }
  int LiveFrame() const { return m_live; }
  u64 BytesRead() const { return m_bytes; }
  u32 Chunks() const { return m_chunks; }
  u32 Resumes() const { return m_resumes; }
  bool Complete() const { return m_info.has_value() && m_pending.empty() && !m_resuming && !m_failed; }

private:
  bool Parse(std::string* error);
  std::string m_compat;
  std::vector<u8> m_pending;
  std::optional<ReplayStreamInfo> m_info;
  bool m_resuming = false;
  int m_segment_end = -1;
  size_t m_metadata_budget = 64ull << 20;
  ReplayArchive m_archive;
  std::map<int, u64> m_checksums;
  u64 m_bytes = 0;
  u32 m_chunks = 0;
  u32 m_resumes = 0;
  int m_live = -1;
  bool m_failed = false;
};

bool EncryptKeyframe(int frame, std::vector<u8>* blob, std::string* key_hex);
bool DecryptKeyframe(int frame, const std::string& key_hex, std::vector<u8>* blob);

bool OpenKeyframe(const KeyframeInfo& info, std::vector<u8> blob, int* frame, ReplayArchive* replay,
                  std::string* error);

class KeyframeStore
{
public:
  virtual ~KeyframeStore() = default;
  virtual bool Put(const KeyframeInfo& info, const std::vector<u8>& data, std::string* error) = 0;
  virtual std::optional<std::vector<u8>> Get(const KeyframeInfo& info,
                                             const std::function<bool(u64, u64)>& progress,
                                             std::string* error) = 0;
  virtual void Delete(const std::string& id) = 0;
  virtual void Cancel() {}
};

using TicketSource = std::function<bool(bool fresh, std::string* ticket, std::string* store_url,
                                        std::string* error)>;

std::unique_ptr<KeyframeStore> MakeKeyframeStore(TicketSource tickets);

std::unique_ptr<KeyframeStore> MakeHttpKeyframeStore(TicketSource tickets);

std::string LastRefusal();
}
