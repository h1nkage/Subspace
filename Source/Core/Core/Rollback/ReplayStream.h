// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Network.h"
#include "Common/WorkQueueThread.h"
#include "Core/Orca/Session/Keyframe.h"
#include "Core/Rollback/OnlineMatch.h"
#include "Core/Rollback/Rollback.h"

namespace Core
{
class System;
}
namespace File
{
class IOFile;
}

namespace Rollback::ReplayStream
{
using NameMap = std::map<int, std::string>;
bool ProductMode();
using Common::OfflineReplay;
bool WatchSourceAllowed(std::string_view source, std::string_view rooms_origin, bool test_knobs,
                        std::string* error);
std::unique_ptr<File::IOFile> OpenReplayFile(const std::string& path, std::string* error);

constexpr size_t WATCH_PENDING_MAX = 8u << 20;
struct WatchPending
{
  bool Append(std::span<const u8> incoming);
  bool Reset();
  std::vector<u8> bytes;
  std::vector<size_t> resets;
};

std::optional<NameMap> ParseNames(std::string_view fields);
std::vector<Orca::Events::PortInfo> ViewPorts(const std::vector<Orca::Events::PortInfo>& ports,
                                          const NameMap& names);
bool Command(std::string_view line);

std::vector<std::vector<u8>> RepackedPieces(const std::vector<u8>& written, int* frames,
                                            size_t max_piece = Orca::Net::REPLAY_PACKED_PIECE_MAX,
                                            bool* too_big = nullptr);

class PackedSender
{
public:
  using Send =
      std::function<void(const std::vector<std::vector<u8>>& pieces, int frames, u64 generation)>;
  explicit PackedSender(Send send);
  ~PackedSender();
  void Submit(std::vector<u8> written, u64 generation, u32 history);
  bool TooBig(u32 history) const;

private:
  struct Job
  {
    std::vector<u8> written;
    u64 generation = 0;
    u32 history = 0;
    u64 sequence = 0;
  };
  Send m_send;
  std::atomic<u64> m_latest{0};
  std::atomic<u32> m_too_big{0};
  Common::WorkQueueThread<Job> m_thread;
  bool m_started = false;
};

class Writer
{
public:
  static std::unique_ptr<Writer> ForMatch();
  explicit Writer(std::string base, bool to_room = false, bool gated = false);
  ~Writer();
  bool Pump(const Orca::Net::ReplayArchive& replay, int end, int live,
            const std::map<int, u64>* checksums, bool room_ready = false);
  void SendPacked();

private:
  bool Emit(const std::vector<u8>& bytes);
  bool StartRoom(const Orca::Net::ReplayArchive& replay, int live);
  void LogTiming(bool final) const;

  struct Timing;
  std::unique_ptr<Timing> m_timing;

  std::string m_base;
  bool m_to_room = false;
  bool m_started = false;
  bool m_room_started = false;
  u64 m_room_generation = 0;
  std::map<int, u64> m_room_checksums;
  std::vector<u8> m_header;
  std::vector<u8> m_written;
  bool m_written_over = false;
  std::unique_ptr<PackedSender> m_packer;
  bool m_gated = false;
  bool m_record_wanted = false;
  int m_record_checked = -1;
  u32 m_segment = 0;
  std::unique_ptr<File::IOFile> m_file;
  int m_first = -1;
  u64 m_origin = 0;
  u32 m_seed = 0;
  int m_written_end = -1;
  std::chrono::steady_clock::time_point m_flushed{};
};

struct PlayerHost
{
  std::function<bool()> restore_origin;
  std::function<bool(MachineImage image, const std::vector<Orca::Net::NandEntry>& nand, int frame)>
      restore;
  std::function<bool(std::vector<Orca::Net::NandEntry>* nand)> read_nand;
  std::function<void(bool catching_up)> set_catching_up;
  std::function<void(int frame, const Orca::Net::Pads& pads)> set_pads;
  std::function<bool()> stopping;
  std::function<void(const std::string& message)> stop;
};

class Player
{
public:
  Player();
  ~Player();
  static std::unique_ptr<Player> FromEnv();
  std::optional<int> Step(Core::System& system, const PlayerHost& host, int* running,
                          u64 origin_hash, int origin_frame, const OnlineMatch::FrameHook& on_frame);
  bool Over() const { return m_over; }
  void Fail(const PlayerHost& host, const std::string& why);

private:
  bool Poll(std::string* error);
  void WatchThread(std::string source);
  void Finish(const PlayerHost& host, bool ok, const std::string& why);
  void PrintTime(int frame, bool force = false);

  std::string m_path;
  bool m_product = false;
  bool m_paused = false;
  std::optional<NameMap> m_names;
  std::optional<int> m_seek_target;
  int m_position = -1;
  std::chrono::steady_clock::time_point m_time_printed{};
  bool m_follow = false;
  bool m_realtime = false;
  int m_delay = 12;
  std::optional<std::pair<int, int>> m_seek;
  std::string m_repack;
  std::unique_ptr<File::IOFile> m_file;
  u64 m_offset = 0;
  struct Watch;
  std::unique_ptr<Watch> m_watch;
  bool m_source_ended = false;
  std::unique_ptr<Orca::Net::ReplayStreamReader> m_reader;
  bool m_loaded = false;
  bool m_over = false;
  bool m_catching = true;
  int m_played = 0;
  int m_checked = 0;
  int m_mismatched = 0;
  int m_first_mismatch = -1;
  std::chrono::steady_clock::time_point m_started{}, m_last_data{};
  s64 m_behind_sum = 0;
  int m_behind_samples = 0;
  int m_behind_max = 0;
  s64 m_live_sum = 0;
  int m_live_max = 0;
  double m_caught_up_ms = -1;
  int m_waits = 0;
  double m_waited_ms = 0;
  int m_last_progress = 0;
  bool m_labeled_recording = false;
  MachineImage m_seek_image;
  std::vector<Orca::Net::NandEntry> m_seek_nand;
  bool m_seek_taken = false;
  bool m_seek_done = false;
};
}
