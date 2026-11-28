// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>

#include <array>
#include <climits>
#include <cstddef>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"

namespace Orca::Net
{
using Pad = std::array<u8, 8>;
constexpr int MAX_SEATS = 4;
constexpr u8 PAD_WIRE_UNPLUGGED = 0x80;
constexpr u8 PAD_WIRE_START_HIGH = 0x10;
constexpr Pad UNPLUGGED_PAD{PAD_WIRE_UNPLUGGED, 0, 0, 0, 0, 0, 0, 0};
inline bool IsUnplugged(const Pad& pad)
{
  return (pad[0] & PAD_WIRE_UNPLUGGED) != 0;
}
constexpr int NEVER = 0x3fffffff;
constexpr int MAX_PADS_IN_PACKET = 64;
constexpr int MAX_FRAMES_AHEAD = 240;
constexpr int MAX_HISTORY_IN_PACKET = 512;
constexpr int MAX_SNAPSHOT_EVERY = 4;
constexpr int RTT_SAMPLES = 15;
constexpr size_t MAX_DELAY_NOTES = 16;
constexpr int SPARED_FRAMES = 4;
constexpr int NEED_WINDOWS = 45;
using Pads = std::array<Pad, MAX_SEATS>;

class Game
{
public:
  virtual ~Game() = default;
  virtual bool Save(int frame) = 0;
  virtual bool Load(int frame) = 0;
  virtual void SetPads(int frame, const Pads& pads) = 0;
  virtual void SetResimulating(bool resimulating) = 0;
  virtual std::optional<u64> SnapshotChecksum(int frame) = 0;
  virtual void ResetPacing() = 0;
  virtual double SaveMs() const { return 0; }
  virtual double NowMs() const;
};

struct SeatPlan
{
  int plug_from = NEVER;
  int unplug_from = NEVER;
  int live_from = NEVER;
  bool operator==(const SeatPlan&) const = default;
};

struct DepartedInputs
{
  int from = NEVER;
  int until = NEVER;
  int first_frame = 0;
  std::vector<Pad> pads;
};

struct Packet
{
  int seat = 0;
  int sequence = 0;
  int first_frame = 0;
  std::vector<Pad> pads;
  std::vector<std::pair<int, std::vector<Pad>>> co;
  std::array<int, MAX_SEATS> ack{-1, -1, -1, -1};
  int current_frame = 0;
  int resimulated = -1;
  int stalls = -1;
  int hitches = -1;
  std::optional<std::array<int, SPARED_FRAMES>> spared;
  std::optional<int> advantage;
  std::optional<int> rtt;
  std::optional<int> checksum_frame;
  u64 checksum = 0;

  std::optional<std::array<SeatPlan, MAX_SEATS>> roster;
  std::optional<std::array<int, MAX_SEATS>> groups;
  std::optional<std::pair<int, int>> start_gate;
  std::map<int, DepartedInputs> departed;
  int history_ack = -1;
  int history_seat = -1;
  int history_first = 0;
  std::vector<Pads> history;
  bool leaving = false;
  int values_ack = -1;
};

class Transport
{
public:
  virtual ~Transport() = default;
  virtual void Send(const Packet& packet) = 0;
  virtual void SendToSeats(const std::map<int, Packet>& packets)
  {
    for (const auto& [seat, packet] : packets)
      Send(packet);
  }
  virtual std::vector<Packet> Receive() = 0;
  virtual bool Connected() const = 0;
};

struct Config
{
  int seats = 2;
  bool per_peer_inputs = false;
  int local_seat = 0;
  std::vector<int> co_seats;
  int input_delay = 2;
  int max_delay = 12;
  std::optional<int> fixed_delay;
  int frame_rate = 60;
  bool time_sync = true;
  int max_rollback = 7;
  int checksum_every = 60;
  int max_pads_per_packet = 16;
  int snapshot_every = 0;

  int start_frame = 0;
  Pad start_pad{};
  std::array<Pad, MAX_SEATS> start_pads{};
  std::optional<std::array<SeatPlan, MAX_SEATS>> plan;
  int authority_seat = -1;
};

enum class StepKind
{
  Run,
  Rollback,
  Stall,
  Wait,
  Ended,
};

struct Step
{
  StepKind kind = StepKind::Run;
  int frame = 0;
};

struct Stats
{
  int rollbacks = 0;
  int resimulated_frames = 0;
  int deepest_rollback = 0;
  int saves = 0;
  int snapshot_every = 1;
  int stalls = 0;
  int checksums_matched = 0;
  int confirmed_frame = -1;
  int delay = 0;
  bool delay_fixed = false;
  int delay_raises = 0;
  int delay_lowers = 0;
  int delay_reverts = 0;
  int waits = 0;
  double frame_advantage = 0;
  int hitches = 0;
  int peer_hitches = 0;
  int load_hitches = 0;
  int counted_stalls = 0;
  int spared_stalls = 0;
  int rtt_median = -1;
  int peer_rtt_median = -1;
  int late_seats = 0;
};

class Session
{
public:
  Session(const Config& config, Game& game, Transport& transport);

  Step OnBoundary(int completed_frame, const Pad& local_pad);
  Step OnBoundary(int completed_frame, const Pad& local_pad, const Pads* co_pads);
  void OnRoundTrip(int rtt_ms);
  void SetFixedDelay(std::optional<int> frames);
  std::optional<int> FixedDelay() const { return m_fixed_delay; }
  void OnLocalHitch();
  void OnLoadHitch();

  void SetLog(int base, std::vector<Pads> log);
  std::vector<Pads> TakeLog(int* base);
  int LogEnd() const { return m_log_base + static_cast<int>(m_log.size()); }
  void AddPeer(int seat, int live_from, int history_from);
  void AddPeerBatch(const std::vector<int>& seats, int live_from, int history_from);
  void SetGroup(int lead, const std::vector<int>& co_seats);
  bool IsLocal(int seat) const;
  int LocalSeat() const { return m_config.local_seat; }
  int LeadOf(int seat) const;
  std::optional<int> PeerRoundTrip(int seat) const { return m_peer_rtt[LeadOf(seat)]; }
  bool SameMachine(int a, int b) const;
  int PeersInPlay() const;
  void DropSeat(int seat);
  void PrepareAdmission(int seat);
  void UnplugSeat(int seat);
  void RequestLeave();
  bool LeftDone() const;
  bool Idle() const;
  int PlayersInPlay() const;
  void RequireValues(int seat, int version);
  void SetValuesHeld(int version);
  std::vector<Pad> PendingLocal() const;
  std::vector<Pad> PendingLocal(int seat) const;
  bool Plugged(int seat, int frame) const;
  int AuthorityFrame() const;
  int AuthoritySeat() const { return m_config.authority_seat; }
  bool CatchingUp() const;
  bool RerunFrom(int frame);
  void DropAllPeers();
  bool Settled() const;
  const std::array<SeatPlan, MAX_SEATS>& Plan() const { return m_plan; }
  std::string Describe() const;

  std::vector<std::string> TakeDelayNotes() { return std::exchange(m_delay_notes, {}); }

  const std::string& Error() const { return m_error; }
  bool ErrorFromPeer() const { return m_error_from_peer; }
  bool LateSeat(int seat) const;
  void EndForPeer(std::string error) { FailPeer(std::move(error)); }
  const Stats& GetStats() const { return m_stats; }
  int CurrentFrame() const { return m_frame; }
  bool Resimulating() const { return m_resim_until >= 0; }
  bool InputStalled() const { return m_stalling && m_stall_ended_ms < 0; }
  std::vector<int> MissingInputSeats() const;
  int Delay() const { return m_delay; }
  int ConfirmedFrame() const;
  int FinalFrame() const { return std::min({m_frame, ConfirmedFrame() + 1, m_dirty}); }
  const std::map<int, u64>& LocalChecksums() const { return m_local_checksums; }

private:
  void AdmitSeat(int seat, int live_from, int history_from);
  void RetireSeat(int seat, bool preserve_unplug = false);
  void CloseEmptyBatch();
  void ReceiveCoPrefixReplies(const Packet& packet);
  void PrepareCoPrefixReplies(const Packet& packet);
  void ReceivePackets();
  void ReceiveOne(const Packet& packet, bool input_only = false);
  bool ApplyGroups(const Packet& packet);
  void AddCoPads(Packet* packet) const;
  void AddPeerChecksum(int seat, const Packet& packet);
  void SendPacket();
  Pads PadsFor(int frame);
  bool NeedsSnapshot(int frame) const;
  void ChooseSnapshotEvery();
  void CheckChecksums();
  void UpdateLateness();
  void ResetLateness(int seat);
  Step Fail(std::string error);
  Step FailPeer(std::string error);
  bool SaveOnce(int frame);
  void SampleLocal(const Pad& local_pad, const Pads* co_pads);
  void AdaptDelay();
  void RaiseToLink();
  void NoteDelay(int from, std::string why);
  int DelayOfInput(int frame) const;
  std::optional<int> LatencyDelay(double margin_ms) const;
  int RoundTripFrames(int seat) const;
  double FramesFromMs(double ms) const;
  void TrackAdvantage();
  int KnownThrough(int seat) const;
  int DepartedThrough(int seat) const;
  const Pad* DepartedPad(int seat, int frame) const;
  void AddDeparted(Packet* packet, int recipient) const;
  bool InPlay(int seat) const;
  int SettleFrom() const;
  int StallLimit() const;
  void AdvanceReceived(int seat);
  void ApplyRoster(const std::array<SeatPlan, MAX_SEATS>& roster);
  void ReceiveHistory(const Packet& packet);
  void AppendConfirmedToLog();
  void PlugCaughtUpPeers();
  void AddHistory(Packet* packet);
  std::set<int> m_plug_batch;
  int m_history_next = 0;
  std::optional<std::pair<int, int>> m_start_gate;
  std::vector<std::pair<int, int>> m_previous_start_gates;
  std::array<int, MAX_SEATS> m_authority_ack{-1, -1, -1, -1};
  std::array<std::array<int, MAX_SEATS>, MAX_SEATS> m_peer_ack{};
  std::map<int, DepartedInputs> m_departed;

  Config m_config;
  Game& m_game;
  Transport& m_transport;

  int m_frame = 0;
  int m_resim_until = -1;
  int m_dirty;

  std::map<int, Pad> m_local;
  std::array<std::map<int, Pad>, MAX_SEATS> m_co_local;
  std::array<int, MAX_SEATS> m_lead{0, 1, 2, 3};
  std::array<bool, MAX_SEATS> m_group_known{};
  std::array<bool, MAX_SEATS> m_local_retired{};
  std::array<unsigned, MAX_SEATS> m_retired_co_leads{};
  std::array<int, MAX_SEATS> m_departed_owner{-1, -1, -1, -1};
  std::array<int, MAX_SEATS> m_local_co_until{NEVER, NEVER, NEVER, NEVER};
  std::map<int, DepartedInputs> m_co_prefix_replies;
  std::array<std::map<int, Pad>, MAX_SEATS> m_remote;
  std::array<int, MAX_SEATS> m_received{-1, -1, -1, -1};
  std::map<int, Pads> m_used;
  std::array<int, MAX_SEATS> m_acked{-1, -1, -1, -1};
  std::array<int, MAX_SEATS> m_remote_frame{0, 0, 0, 0};

  std::map<int, u64> m_local_checksums;
  std::map<std::pair<int, int>, u64> m_peer_checksums;
  int m_last_checksum_frame = 0;
  std::optional<int> m_pending_checksum;
  int m_last_sent_newest = -1;
  bool m_stalling = false;
  int m_last_saved = -1;
  std::set<int> m_saved;
  int m_snapshot_every = 1;

  int m_delay;
  std::optional<int> m_fixed_delay;
  int m_window_start = 0;
  int m_guessed_frames = 0;
  int m_window_resimulated = 0;
  int m_last_resimulated = -1;
  int m_link_stalls = 0;
  std::array<int, SPARED_FRAMES> m_spared{};
  double m_stall_began_ms = 0;
  double m_stall_ended_ms = -1;
  std::array<int, MAX_SEATS> m_peer_stalls{0, 0, 0, 0};
  std::array<int, MAX_SEATS> m_peer_stalls_seen{0, 0, 0, 0};
  std::array<std::array<int, SPARED_FRAMES>, MAX_SEATS> m_peer_spared{};
  std::array<std::array<int, SPARED_FRAMES>, MAX_SEATS> m_peer_spared_seen{};
  std::array<bool, MAX_SEATS> m_peer_reports_spared{};
  std::array<int, MAX_SEATS> m_peer_stall_delay{INT_MAX, INT_MAX, INT_MAX, INT_MAX};
  std::deque<std::pair<int, int>> m_delay_marks;
  int m_window_peer_reach = 0;
  std::array<int, NEED_WINDOWS> m_need_history{};
  int m_evidence_windows = 0;
  std::array<int, MAX_SEATS> m_edge_frame{INT_MIN / 2, INT_MIN / 2, INT_MIN / 2, INT_MIN / 2};
  int m_last_peer_stall_frame = INT_MIN / 2;
  bool m_delay_from_link = false;
  int m_link_below_windows = 0;
  int m_quiet_windows = 0;
  int m_hold_windows;
  int m_windows_since_lower = INT_MAX / 2;
  int m_stall_frame = 0;
  bool m_stall_after_input = false;
  int m_hitches = 0;
  std::array<int, MAX_SEATS> m_peer_hitches{0, 0, 0, 0};
  int m_last_hitch_frame = INT_MIN / 2;

  std::deque<int> m_rtts;
  std::optional<int> m_typical_rtt;
  std::array<std::optional<int>, MAX_SEATS> m_peer_rtt;

  std::array<std::optional<double>, MAX_SEATS> m_advantage;
  std::array<int, MAX_SEATS> m_late_sum{};
  std::array<int, MAX_SEATS> m_late_samples{};
  std::array<int, MAX_SEATS> m_late_windows{};
  std::array<int, MAX_SEATS> m_clean_windows{};
  std::array<bool, MAX_SEATS> m_late{};
  int m_late_window_start = 0;
  std::array<std::optional<double>, MAX_SEATS> m_smoothed_advantage;
  std::array<std::optional<double>, MAX_SEATS> m_remote_advantage;
  double m_balance_sum = 0;
  int m_balance_samples = 0;
  int m_sync_start = 0;
  bool m_wait_pending = false;
  bool m_waiting = false;

  int m_send_sequence = 0;
  std::array<int, MAX_SEATS> m_peer_sequence{0, 0, 0, 0};
  std::array<int, MAX_SEATS> m_peer_checksum_frame{-1, -1, -1, -1};

  std::array<SeatPlan, MAX_SEATS> m_plan{};
  int m_log_base = 0;
  std::vector<Pads> m_log;
  std::array<int, MAX_SEATS> m_history_sent{-1, -1, -1, -1};
  std::array<int, MAX_SEATS> m_history_acked{-1, -1, -1, -1};
  std::array<int, MAX_SEATS> m_history_stuck{0, 0, 0, 0};
  std::array<int, MAX_SEATS> m_own_live_from{NEVER, NEVER, NEVER, NEVER};
  bool m_leaving = false;
  std::array<int, MAX_SEATS> m_values_required{-1, -1, -1, -1};
  std::array<int, MAX_SEATS> m_values_acked{-1, -1, -1, -1};
  int m_values_held = -1;

  std::vector<std::string> m_delay_notes;

  Stats m_stats;
  std::string m_error;
  bool m_error_from_peer = false;
};
}
