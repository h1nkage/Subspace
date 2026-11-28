// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <picojson.h>

#include "Common/CommonTypes.h"
#include "Core/Orca/Session/Session.h"

namespace Orca::Engine
{
struct GameResult;
}

namespace Orca::Net
{
struct RoomOptions
{
  std::string room_code;
  std::string compatibility;
  std::string player_name = "Player";
  std::string mode = "orca-test";
  bool joining = false;
  bool join_if_taken = false;
  bool friends_room = true;
  bool recovery_enabled = true;
  std::vector<u8> controls;
  std::vector<u8> queue;
  bool ask_app_for_room = true;
};

struct KeyframeInfo
{
  int frame = -1;
  std::string id;
  u64 size = 0;
  std::string hash;
  std::string key;
  struct Name
  {
    int seat = 0;
    int from = 0;
    std::string name;
    std::vector<u8> controls;
    std::vector<u8> queue;
    bool operator==(const Name&) const = default;
  };
  std::vector<Name> names;
  int names_version = 0;
  bool shared = false;
  std::vector<u8> inline_blob;
};

constexpr size_t INLINE_KEYFRAME_MAX = 32 * 1024;
constexpr size_t INLINE_KEYFRAME_MAX_TEXT = (INLINE_KEYFRAME_MAX + 2) / 3 * 4;
bool InlineKeyframe(u64 size);
std::string EncodeInlineKeyframe(const std::vector<u8>& blob);
std::optional<std::vector<u8>> DecodeInlineKeyframe(std::string_view text, u64 size);

constexpr size_t MAX_CONTROLS = 64;
constexpr size_t MAX_QUEUE = 32;

struct PeerEvent
{
  enum class Kind
  {
    Arrived,
    Left,
    Keyframe,
    Names,
    Hold,
    CoSeatArrived,
    CoSeatLeft,
  };
  Kind kind = Kind::Arrived;
  int seat = -1;
  bool host = false;
  std::string name;
  std::vector<u8> controls;
  KeyframeInfo keyframe;
  std::string reason;
  std::vector<u8> queue;
  bool holding = false;
  bool recovering = false;
  bool doubles = false;
  std::vector<int> co_seats;
  int lead = -1;
};

struct DirectSummary
{
  int transport = 0;
  int rtt_ms = -1;
  std::array<int, MAX_SEATS> seat_rtt_ms{-1, -1, -1, -1};
  std::string line = "relay";
};

struct GameReport
{
  enum class Kind
  {
    Win,
    Draw,
    Void,
  };
  Kind kind = Kind::Void;
  int winner_seat = -1;
  std::string why;
  std::string detail;
  int start = -1;
  bool from_set_block = false;
  int number = 0;
  int set_done = 0;
  int set_winner_seat = -1;
};

GameReport FromGameResult(const Engine::GameResult& result,
                          const std::optional<std::map<int, int>>& doubles_teams = std::nullopt);

enum class RoomState
{
  Connecting,
  Waiting,
  Ended,
};

struct RoomTestStep;
struct RoomTestSnapshot;

class RelayRoom final : public Transport
{
public:
  explicit RelayRoom(RoomOptions options);
  ~RelayRoom() override;

  RoomState GetState() const;
  int Seat() const;
  std::vector<int> CoSeats() const;
  std::optional<std::vector<int>> CoSeatsForSolo() const;
  void SetSessionActive(bool active);
  bool TryStartSession(const std::vector<int>& co_seats);
  int LocalIndex(int seat, bool only_mine = false) const;
  std::vector<int> GroupOf(int seat) const;
  std::optional<bool> Joining() const;
  std::vector<PeerEvent> TakePeerEvents();
  bool ArrivalPending() const;
  bool InputHoldFor(const std::string& id) const;
  std::optional<std::chrono::steady_clock::time_point> HostAwayDeadline() const;
  void RetryPeer(int seat);
  bool HoldInputSilence(std::chrono::steady_clock::time_point since,
                        std::chrono::steady_clock::time_point now,
                        std::optional<std::vector<int>> missing = std::nullopt);
  bool RoomRecovering(std::chrono::steady_clock::time_point now) const;

  bool HostLeftPending() const;
  std::vector<int> LeftSeatsPending() const;
  void OfferKeyframe(int seat, const KeyframeInfo& info);
  void BroadcastNames(const std::vector<KeyframeInfo::Name>& names, int version);
  void DropPeer(int seat, const std::string& reason);
  void HoldJoins(bool holding, bool fight = false);
  bool FreshTicket(bool fresh, std::string* ticket, std::string* store_url, std::string* error);
  std::string Code() const;
  u64 Generation() const;
  std::string SeatName(int seat) const;
  std::vector<u8> HelloControls() const;
  std::vector<u8> HelloQueue() const;
  std::string Status() const;
  std::string ErrorCode() const;
  int RoundTripMs(u32* sequence = nullptr) const;
  std::vector<int> RecentRoundTrips(u32* sequence) const;
  DirectSummary Direct() const;
  void TestDirect(const std::string& command);

  std::string Queue() const;
  std::string PlayQueue() const;
  bool Doubles() const;
  std::map<int, int> Teams() const;
  bool RoomRecovery() const;
  bool MatchLive() const;
  bool SetOver() const;
  void SetOpponentPlugged(bool plugged);
  void ReportGame(GameReport report);
  void ReportGameStart(int start);
  int KnownSetGames() const;
  void ReportDesync();
  void ReportStall();
  void ReportFault();
  void KeepAlive();
  void SendReplayStream(const std::vector<u8>& bytes);
  void SendReplayPacked(const std::vector<std::vector<u8>>& pieces, int frames);

  void Send(const Packet& packet) override;
  void SendToSeats(const std::map<int, Packet>& packets) override;
  std::vector<Packet> Receive() override;
  bool Connected() const override;

  void Leave();
  void SetLeaveReason(std::string reason);
  void TestDropConnection(std::chrono::seconds ticket_age = {});

private:
  RelayRoom(RoomOptions options, bool start_io);
  friend std::vector<RoomTestSnapshot> TraceRoomForTest(const RoomOptions&,
                                                      const std::vector<RoomTestStep>&,
                                                      const std::function<void(RelayRoom&)>&);
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

class SetTally
{
public:
  static constexpr int SET_WINS = 2;
  static constexpr int SET_GAMES = 7;
  struct End
  {
    std::string winner;
    std::string why;
  };
  std::optional<End> Game(const std::string& winner);
  int Wins(const std::string& id) const;
  int Games() const { return m_games; }
  void Reset();

private:
  std::map<std::string, int> m_wins;
  int m_games = 0;
  bool m_ended = false;
};

bool ParseRoomJson(const std::string& text, picojson::value* value);
bool DropKeepsRoom(const std::string& queue, bool match_live, bool set_over);
constexpr std::chrono::milliseconds KEEP_ALIVE_EVERY{2000};
bool KeepAliveDue(std::chrono::steady_clock::time_point now,
                  std::optional<std::chrono::steady_clock::time_point> wanted,
                  std::optional<std::chrono::steady_clock::time_point> sent);
std::string FinishMessage(const std::string& match, const std::string& winner);
int RememberKnownSetGames(const std::string& match, int known);
std::string GameStartMessage(const std::string& queue, const std::string& match, bool decided,
                             int start);
std::string EncodePacket(const Packet& packet);
std::string EncodeDirectPacket(const Packet& packet);
bool DecodePacket(const std::string& json, Packet* packet);
picojson::value NamesToJson(const std::vector<KeyframeInfo::Name>& names);
std::vector<KeyframeInfo::Name> NamesFromJson(const picojson::object& message);
picojson::object KeyframeOfferToJson(const KeyframeInfo& info);
std::optional<KeyframeInfo> KeyframeOfferFromJson(const picojson::object& message);
bool DecodeDirectPacket(const std::string& payload, int slot, Packet* packet);
bool TicketAllowsDirect(const picojson::object& reply);
std::string RefusalReason(const std::string& connect_error);
std::string RefusalCode(const std::string& connect_error);
void CoalescePacket(std::optional<Packet>* outgoing, const Packet& packet);
void CoalesceBroadcast(std::optional<Packet>* outgoing,
                       std::map<int, std::optional<Packet>>* addressed, const Packet& packet);
void CoalesceToSeats(std::optional<Packet>* outgoing,
                     std::map<int, std::optional<Packet>>* addressed,
                     const std::map<int, Packet>& packets);
struct RoomTestStep
{
  enum class Kind
  {
    Message,
    DirectInput,
    DirectPacket,
    LinkPeer,
    Tick,
    SocketClose,
    RetryPeer,
    PollReconnect,
    ConnectFailure,
    InitialConnectFailure,
    InputSilence,
    SocketConnected,
    SendFailure,
    ReportGame,
    GameStart,
    Stall,
    PageEvent,
    SessionActive,
    Tournament,
    OfferKeyframe,
    AppCaps,
    SocketMessage,
    FlushHeld,
  };
  Kind kind;
  int milliseconds = 0;
  std::string text;
  unsigned close_code = 0;
};
struct RoomTestSnapshot
{
  RoomState state;
  std::vector<PeerEvent> events;
  std::vector<Packet> packets;
  std::optional<std::chrono::steady_clock::time_point> host_away_until;
  bool reconnecting = false;
  bool reconnect_started = false;
  std::vector<std::string> sent;
  unsigned reconnect_attempts = 0;
  std::optional<std::chrono::steady_clock::time_point> reconnect_at;
  std::vector<std::string> notices;
  std::string status;
  std::string error_code;
  bool recovery = false;
  int known_games = 0;
  std::vector<std::string> results;
  std::optional<std::chrono::steady_clock::time_point> public_deadline;
  bool session_recovering = false;
  std::optional<bool> input_held;
  int seat = -1;
  std::vector<int> co_seats;
  std::array<int, MAX_SEATS> local_indices{-1, -1, -1, -1};
  std::array<std::vector<int>, MAX_SEATS> groups;
  std::vector<int> linked_seats;
  std::optional<std::chrono::steady_clock::time_point> host_input_deadline;
  std::map<int, int> teams;
  std::optional<int> ticket_local_seats = 1;
  picojson::object mirror;
  std::string published_mirror;
  unsigned mirror_serializations = 0;
  std::vector<std::string> probe_lines;
};
std::vector<RoomTestSnapshot> TraceRoomForTest(const RoomOptions& options,
                                             const std::vector<RoomTestStep>& steps,
                                             const std::function<void(RelayRoom&)>& observe = {});
}
