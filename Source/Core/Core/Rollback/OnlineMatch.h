// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Core/Orca/Session/Events.h"
#include "Core/Orca/Session/Session.h"
#include "Core/Orca/Session/RelayRoom.h"

namespace Core
{
class System;
}

namespace Orca::Mod
{
struct Manifest;
struct OriginDecl;
}
namespace Orca::Net
{
struct ReplayArchive;
class KeyframeStore;
}
namespace Orca::UX
{
class GuestMemory;
}

namespace Rollback::OnlineMatch
{
using FrameHook = std::function<void(int frame, bool resimulating,
                                     const std::vector<Orca::Events::PortInfo>& ports, bool alone)>;
std::optional<int> OnBoundary(Core::System& system,
                              const std::function<Orca::Net::Pad(int local_seat)>& local_pad,
                              const FrameHook& on_frame,
                              const std::function<void(int local_seat)>& local_pad_waiting = {});

constexpr int KEYFRAME_FRESH_FRAMES = 30 * 60;

class FriendsBatch
{
public:
  bool Ready(bool on_menus, int waiting, int plugging, bool recovering,
             std::chrono::steady_clock::time_point now, int local_seats = 1);
  void Clear() { m_until.reset(); }
private:
  std::optional<std::chrono::steady_clock::time_point> m_until;
};

std::optional<std::vector<int>> FriendsMissingInputs(const Orca::Net::Session& session, bool doubles);
int WaitingFriendSeat(int local_seat, const std::vector<int>& missing, bool joining,
                       int host_seat, const std::vector<int>& seated);
bool WaitForFriendsRoster(const Orca::Net::Session& session, bool joining, bool doubles);
std::optional<std::vector<int>> DropMissingFriends(Orca::Net::Session& session, bool doubles);

struct JoinCapture
{
  int frame;
  std::optional<u64> hash;
};
std::optional<JoinCapture> ChooseJoinCapture(const Orca::Net::Session* session, bool doubles,
                                            int next, int earliest);

std::string RoomReopenCode(const std::string& previous_code, const std::string& queue,
                            bool held, bool taken);
bool PumpRoomReopenForTest();
std::vector<unsigned> TraceRoomReopenForTest(const std::vector<std::chrono::seconds>& times,
                                              const std::function<void()>& after_step);
int PumpRoomReopenFailureForTest(const std::function<void()>& wait_for_failure);

struct LaunchJoinBoundaryResult
{
  bool finished;
  bool joining;
  bool has_port;
  int frame;
  int local_seat;
  bool log_empty;
  bool queued_local_empty;
};
LaunchJoinBoundaryResult PumpLaunchJoinBoundaryForTest();

struct ReseatResult
{
  int local_seat;
  int running;
  int origin_frame;
  int log_base;
  std::vector<Orca::Net::Pads> log;
  bool queued_local_empty;
  std::vector<int> co_seats;
};
ReseatResult PumpReseatForTest(int seat, const std::vector<Orca::Net::Pads>& log,
                             const std::vector<int>& co_seats = {});

void ReportJoinFailure(std::string_view code, std::string_view sentence);

struct HostRecoveryResult
{
  bool fresh_start = false;
  bool seated = false;
  int current_frame = -1;
  std::optional<int> keyframe_frame;
  std::vector<int> seats;
  std::array<Orca::Net::SeatPlan, Orca::Net::MAX_SEATS> plan{};
  std::array<int, Orca::Net::MAX_SEATS> groups{};
  std::string error;
};
HostRecoveryResult PumpRecoveryHostForTest(Core::System& system, Orca::Net::RelayRoom& room,
                                          Orca::Net::Game& game, Orca::Net::ReplayArchive replay,
                                          int current_frame, bool session_alive,
                                          std::unique_ptr<Orca::Net::KeyframeStore> store);
HostRecoveryResult PumpFriendsHostForTest(Core::System& system, Orca::Net::RelayRoom& room,
                                         Orca::Net::Game& game, Orca::Net::ReplayArchive replay,
                                         int current_frame, const std::vector<int>& seated,
                                         bool on_menus,
                                         std::unique_ptr<Orca::Net::KeyframeStore> store,
                                         bool host_requested = false,
                                         const std::function<void(const std::function<void()>&)>&
                                             after_admit = {});

bool PumpHeldFriendsEventsForTest(Orca::Net::RelayRoom& room,
    std::unique_ptr<Orca::Net::Session>& session, const std::vector<int>& seated);

int PumpFriendsHostBoundaryForTest(std::unique_ptr<Orca::Net::Session>& session, int completed,
                                   const std::vector<int>& seated,
                                   const std::function<void()>& poll,
                                   const std::function<std::vector<int>()>& left_in_play,
                                   bool joining = false);

constexpr int ORIGIN_SETTLE_FRAMES = 30;
constexpr int ORIGIN_DEADLINE = 6000;
constexpr int ORIGIN_WORDS_WINDOW = 6000;
Orca::Net::Pads CanonicalBootPads();
enum class OriginStep
{
  Wait,
  Capture,
  Fail,
};
OriginStep DecideOrigin(int frame, int menu_built_streak, std::optional<u32> jit_clear_frame);
enum class OriginRule
{
  Menu,
  Declared,
  Frame0,
  Failed,
};
OriginRule SelectOriginRule(const Orca::Mod::Manifest* manifest, bool menu_supported,
                          bool guards_failed);
OriginStep DecideDeclaredOrigin(int frame, int streak, const Orca::Mod::OriginDecl& decl);
std::string_view OriginFailureReport(const Orca::Mod::Manifest* manifest);
std::string_view RoomRefusal(std::string_view queue, bool results);
int DeclaredOriginStreak(int frame, int streak, const Orca::Mod::OriginDecl& decl,
                         const Orca::UX::GuestMemory& memory);

constexpr int FRESH_FRIENDS_AFTER = 10 * 60;
inline constexpr std::chrono::seconds FRESH_RESET_FRIENDS_AFTER{3 * 60};
inline constexpr std::chrono::seconds FRESH_RESET_JOIN_WARNING{3};
constexpr int FRESH_CSS_SETTLE = 30;
constexpr int FRESH_TAIL_LIMIT = 600;
enum class FreshKind
{
  Menu,
  Reset,
};
FreshKind FreshKindForOrigin(OriginRule rule);
bool FreshExitAllowed(FreshKind kind, u8 exit);
bool FreshTailSettled(FreshKind kind, int css_streak, int length, int frame = 0,
                      int offer_from = 0);
Orca::Net::ReplayArchive FreshReplayStart(FreshKind kind, int frame, u64 origin_hash, u8 exit,
                                        u32 seed);
bool LocalNandReady(bool is_wii, bool has_host_nand, bool temporary);
enum class FreshStep
{
  None,
  Warn,
  Now,
  Hold,
};
struct FreshInputs
{
  FreshKind kind = FreshKind::Menu;
  bool enabled = true;
  bool origin_ready = false;
  bool joining = false;
  bool session = false;
  bool seated_or_plugging = false;
  bool busy = false;
  bool queue_armed = false;
  bool in_queue_room = false;
  bool waiting = false;
  bool in_place = false;
  bool single_player = false;
  int history_frames = 0;
  std::chrono::duration<double> history_time{};
  int fresh_after = -1;
};
FreshStep DecideFreshStart(const FreshInputs& in);
struct FightHoldInputs
{
  bool recovering = false;
  bool enabled = true;
  bool fight = false;
  bool waiting = false;
  bool joining = false;
  bool session = false;
  bool seated_or_plugging = false;
  bool doubles = false;
  bool in_queue_room = false;
  bool let_in = false;
};
bool FightJoinHeld(const FightHoldInputs& in);
class HoldMessageTimer
{
public:
  using Clock = std::chrono::steady_clock;
  void Reset() { m_next.reset(); }
  bool Due(Clock::time_point now, Clock::duration lifetime)
  {
    if (m_next && now < *m_next)
      return false;
    m_next = now + lifetime;
    return true;
  }

private:
  std::optional<Clock::time_point> m_next;
};
FreshStep WarnBeforeResetJoin(FreshStep step, FreshKind kind, bool queue_armed,
                             std::optional<std::chrono::steady_clock::time_point>* warned_at,
                             std::chrono::steady_clock::time_point now);
u8 FriendsFreshExit(std::optional<bool> css_pick_ranked);

constexpr int FRESH_UNSEEN_LIMIT = 120;
bool FreshUnseen(bool queue_room, int frames);
bool HeaderFreeWhileCatchingUp(bool unseen_hold, bool in_tail, bool joining);

bool AloneAt(int frame, bool solo_idle, bool drop_in_pending, std::optional<int> stored_keyframe);

bool HeaderFreeAt(int frame, bool solo_quiet, std::optional<int> stored_keyframe);

enum class HeaderWait
{
  Ready,
  Wait,
  Fail,
};
constexpr int HEADER_WAIT_BOUNDARIES = 120;
constexpr int HEADER_FAIL_BOUNDARIES = 300;
HeaderWait StepHeaderWait(int* waited, bool in_place, bool someone_waiting, bool queue_room);
HeaderWait HeaderWaitAt(int* waited, bool in_tail, bool unseen, bool in_place, bool someone_waiting,
                        bool queue_room);

struct HomeInputs
{
  int local_seat = 0;
  bool joining = false;
  bool session = false;
  bool room_up = false;
  bool solo_idle = false;
  bool drop_in_pending = false;
  bool on_menus = false;
  bool queue_image = false;
  bool lingering = false;
};
bool MayComeHome(const HomeInputs& in);

class StallBudget
{
public:
  using Clock = std::chrono::steady_clock;
  static constexpr std::chrono::seconds STALL_WINDOW{60};
  static constexpr std::chrono::seconds STALL_BUDGET{15};
  void Add(Clock::time_point start, Clock::time_point end);
  Clock::duration Stalled(Clock::time_point now,
                          std::optional<Clock::time_point> ongoing_since = std::nullopt) const;
  bool Spent(Clock::time_point now,
             std::optional<Clock::time_point> ongoing_since = std::nullopt,
             bool ongoing_held = false) const
  {
    return Stalled(now, ongoing_held ? std::nullopt : ongoing_since) >= STALL_BUDGET;
  }
  void Clear()
  {
    m_stalls.clear();
    m_recoveries.clear();
    m_pending_stalls.clear();
    m_recovery_seen = false;
  }
  static constexpr std::chrono::minutes RECOVERY_WINDOW{10};
  static constexpr int MAX_RECOVERIES = 3;
  static constexpr std::chrono::seconds MIN_EXCUSED_STALL{2};
  bool ExcuseRecovered(Clock::time_point now);
  void ObserveRecovery(bool recovering) { m_recovery_seen |= recovering; }
  bool Poll(Clock::time_point now, Clock::time_point step_start, bool recovering);
  void FinishStep(std::optional<Clock::time_point> stall_start, Clock::time_point end,
                  bool still_stalled, bool resumed);
  int Recoveries() const { return static_cast<int>(m_recoveries.size()); }

private:
  std::vector<std::pair<Clock::time_point, Clock::time_point>> m_stalls;
  std::vector<Clock::time_point> m_recoveries;
  std::vector<std::pair<Clock::time_point, Clock::time_point>> m_pending_stalls;
  bool m_recovery_seen = false;
};

struct OwnValues
{
  std::string name;
  std::vector<u8> controls;
  std::vector<u8> queue;
};
bool OwnValuesKept(const std::vector<Orca::Net::KeyframeInfo::Name>& names, int seat, int plug_from,
                   const OwnValues& own);

struct VerdictWaitInputs
{
  bool queue_room = false;
  bool match_live = false;
  bool set_over = false;
  bool room_ended = false;
  bool from_peer = false;
  bool waited_out = false;
};
bool VoidFirst(const VerdictWaitInputs& in);

enum class LobbyPickStep
{
  Ignore,
  Announce,
  Leave,
};
struct LobbyPickInputs
{
  bool joining = false;
  int local_seat = 0;
  bool queue_room = false;
  bool host_cap = false;
  bool pick_cap = false;
  bool session = false;
  bool session_idle = false;
  bool drop_in_friends = false;
  bool arrival_pending = false;
  bool keyframe_kept = false;
};
struct LobbyPickPlan
{
  LobbyPickStep step = LobbyPickStep::Ignore;
  bool drop_keyframe = false;
  bool arm = true;
};
LobbyPickPlan DecideLobbyPick(const LobbyPickInputs& in);

bool AnnounceHomePick(bool css_pick, bool host_cap, bool queue_or_search);

enum class KeptPickStep
{
  Wait,
  Arm,
  Drop,
};
struct KeptPickInputs
{
  bool stands = false;
  bool alone = false;
  bool queue_or_search = false;
};
KeptPickStep DecideKeptPick(const KeptPickInputs& in);

struct FriendQueueInputs
{
  bool friend_coming = false;
  bool queue_or_search = false;
  int local_seat = 0;
  bool joining = false;
  bool friends_room = false;
};
bool FriendEndsQueue(const FriendQueueInputs& in);

bool JoinEndsQueue(bool queue_room, bool queue_or_search, bool queue_image);

enum class FriendsPickStep
{
  Wait,
  Arm,
  Drop,
};
struct FriendsPickInputs
{
  bool played = false;
  bool alone = false;
  bool on_pick_select = false;
  bool host_cap = false;
  bool queue_or_search = false;
  bool elsewhere = false;
};
FriendsPickStep DecideFriendsPick(const FriendsPickInputs& in);

bool Active();
int LocalSeat();
int LocalOrdinal(int seat);
int LocalSeatCount();
std::vector<Orca::Net::Pads> PumpSoloSeatsForTest(Core::System& system,
    const std::vector<int>& co_seats, int frames, bool fresh_tail,
    const std::function<Orca::Net::Pad(int)>& sample,
    std::vector<Orca::Events::PortInfo>* ports);
Orca::Net::Pads PumpSoloRoomForTest(Core::System& system, Orca::Net::RelayRoom& room);
std::optional<Orca::Net::Pad> PumpSeenLocalPadForTest(int lead,
    const std::vector<int>& co_seats, const std::function<Orca::Net::Pad(int)>& sample);
int PumpLeadSeatForTest(std::unique_ptr<Orca::Net::Session> session, int solo_seat);
std::vector<Orca::Net::Pads> PumpJoinerSoloForTest(
    std::unique_ptr<Orca::Net::Session> session, int frames,
    const std::function<Orca::Net::Pad(int)>& sample);
void LogStats(const char* when);
void End(const char* reason);

bool SoloPauseAllowed();
bool BeginPause();
void EndPause();
bool Pausing();
void WatchRoomWhilePaused();
std::chrono::nanoseconds TakePausedTime();
}
