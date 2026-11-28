// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/OnlineMatch.h"

#include <algorithm>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <picojson.h>

#ifdef __APPLE__
#include <sys/clonefile.h>
#endif

#include "Common/FileUtil.h"
#include "Common/HookableEvent.h"
#include "Common/Logging/Log.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"
#include "Common/Timer.h"
#include "AudioCommon/AudioCommon.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/SystemTimers.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/FS/HostBackend/FS.h"
#include "Core/IOS/IOS.h"
#include "Core/Orca/Engine/GuestMemory.h"
#include "Core/Orca/Mod.h"
#include "Core/Orca/Perf.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/Session/Keyframe.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Session/PadCodec.h"
#include "Core/Orca/Session/RoomResilience.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/UX/Overlay.h"
#include "Core/Orca/UX/ControllerSource.h"
#include "Core/Orca/UX/Probe.h"
#include "Core/Orca/UX/MenuEvents.h"
#include "Core/Orca/UX/QueueApp.h"
#include "Core/Orca/UX/Search.h"
#include "Core/Orca/UX/SetEnd.h"
#include "Core/Orca/UX/UX.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/Rollback/Harness.h"
#include "Core/Rollback/ReplayStream.h"
#include "Core/Rollback/Rollback.h"
#include "Core/Rollback/SessionPort.h"
#include "Core/System.h"
#include "Core/WiiRoot.h"
#include "VideoCommon/OnScreenDisplay.h"

namespace Rollback::OnlineMatch
{
namespace
{
using Orca::Net::Pad;
using Orca::Net::Pads;
using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

struct KeyframeJob
{
  std::thread thread;
  std::atomic<bool> done{false};
  bool ok = false;
  int frame = -1;
  Orca::Net::KeyframeInfo info;
  double capture_ms = 0, pack_ms = 0, put_ms = 0;
  u64 raw_size = 0;
  u64 generation = 0;
  std::string error;
  std::string refusal;
  ~KeyframeJob()
  {
    if (thread.joinable())
      thread.join();
  }
};

struct Download
{
  std::thread thread;
  std::atomic<bool> done{false};
  std::atomic<int> percent{0};
  std::atomic<bool> cancel{false};
  bool ok = false;
  std::string error;
  std::string refusal;
  Orca::Net::KeyframeInfo info;
  int frame = -1;
  Orca::Net::ReplayArchive replay;
  double transfer_ms = 0, unpack_ms = 0;
  ~Download()
  {
    cancel = true;
    if (thread.joinable())
      thread.join();
  }
};

struct Arrival
{
  std::string name;
  std::vector<u8> controls;
  std::vector<u8> queue;
  bool recovering = false;
  bool doubles = false;
  Clock::time_point arrived_at = Clock::now();
  u64 arrived_boundary = 0;
  int next_keyframe_notice = 15;
  int lead = -1;
};

struct Match
{
  bool started = false;
  bool finished = false;
  bool stale = false;
  std::unique_ptr<RingPort> port;
  std::unique_ptr<Orca::Net::Session> session;
  std::unique_ptr<Orca::Net::KeyframeStore> store;
  bool joining = false;
  bool reopen_join = false;
  int local_seat = 0;
  std::vector<int> co_seats;
  int running = -1;

  int log_base = 0;
  std::vector<Pads> log;
  std::array<std::deque<Pad>, Orca::Net::MAX_SEATS> queued_local{};
  std::unique_ptr<KeyframeJob> job;
  std::optional<Orca::Net::KeyframeInfo> keyframe;
  u64 keyframe_generation = 0;
  double keyframe_capture_ms = 0;
  bool keyframe_wanted = false;
  int recovery_keyframe_from = -1;
  int header_waited = 0;
  std::map<int, Arrival> waiting;
  std::map<int, Arrival> menu_waiting;
  u64 host_boundaries = 0;
  std::set<int> seated;
  std::set<int> plugging;
  FriendsBatch batch;
  bool doubles = false;
  std::set<int> doubles_seats;
  std::vector<Orca::Net::KeyframeInfo> shared_keyframes;
  bool holding = false;
  bool fight_holding = false;
  bool fight_let_in = false;
  bool room_lost = false;
  bool reopen_gave_up = false;
  bool unstable_reopening = false;
  std::optional<Clock::time_point> unstable_hosted_since;
  unsigned reopen_attempts = 0;
  std::optional<Clock::time_point> network_failure_since;
  Clock::time_point room_lost_at{}, reopen_at{};
  std::chrono::seconds reopen_backoff{0};
  std::string home_code;
  std::optional<Clock::time_point> reopen_pending;
  bool tell_no_room = false;

  std::unique_ptr<Download> download;
  std::unique_ptr<Download> replaying;
  MachineImage origin;
  std::vector<Orca::Net::NandEntry> origin_nand;
  int origin_frame = -1;
  u64 origin_hash = 0;
  u64 origin_ticks = 0;
  int menu_built_streak = 0;
  int origin_words_streak = 0;
  OriginRule origin_rule = OriginRule::Menu;
  bool origin_failed = false;
  Orca::Net::ReplayArchive replay;
  Clock::time_point replay_heartbeat{};
  int host_seat = 0;
  std::string host_name;
  bool host_here = false;
  Orca::Net::HostRoomWait host_room_wait;
  bool host_holding = false;
  bool host_hold_fight = false;
  HoldMessageTimer host_hold_message;
  Clock::time_point boot_time{}, offered_time{}, loaded_time{};
  double load_ms = 0;
  bool joined = false;
  int leave_at = -1;
  bool leave_requested = false;
  bool opponent_no_show = false;
  std::string join_status;

  int last_percent = -1;

  u32 round_trip_sequence = 0;
  std::optional<Clock::time_point> last_return;
  Common::EventHook on_state_changed;

  bool join_in_play = false;
  std::string join_error_code, join_error;

  Clock::time_point stats_time{};
  int stats_frame = -1;
  int stats_held = 0;
  Orca::Net::Stats stats_last{};
  int stats_rbmax = 0;
  double stats_stall_ms = 0;
  Orca::Events::InputAge stats_input{};
  StallBudget stall_budget;

  std::map<int, std::vector<Orca::Net::KeyframeInfo::Name>> port_names;
  int names_version = 0;
  std::optional<Orca::Net::KeyframeInfo> later_names;
  std::vector<Orca::Events::PortInfo> told_ports;
  std::optional<std::string> host_gone;
  int idle_frames = 0;

  std::optional<Clock::time_point> linger_until;
  bool friend_left_told = false;

  struct QueueImage
  {
    int frame = 0;
    int local_seat = 0;
    std::vector<int> co_seats;
    Clock::time_point loaded_time{};
    MachineImage image;
    std::vector<Orca::Net::NandEntry> nand;
    Orca::Net::ReplayArchive replay;
    bool armed = false;
    Clock::time_point taken{};
  };
  std::unique_ptr<QueueImage> queue_image;
  std::unique_ptr<QueueImage> before_join;
  bool want_queue_image = false;
  bool ranked_fought = false;

  std::optional<u8> fresh_pending;
  std::optional<Clock::time_point> fresh_join_warned_at;
  bool fresh_queue_armed = false;
  std::string fresh_room;
  bool friends_in_place = false;
  struct Fresh
  {
    enum class Phase
    {
      Tail,
      Hold,
      Steer,
    };
    Phase phase = Phase::Tail;
    FreshKind kind = FreshKind::Menu;
    u8 exit = 0;
    bool queue = false;
    std::string room;
    int css_streak = 0;
    Clock::time_point started{};
    bool unseen = false;
    int unseen_frames = 0;
    Clock::time_point tail_end{};
  };
  std::optional<Fresh> fresh;
  bool fast = false;
  bool fast_spent = false;

  std::optional<Orca::UX::OnlinePick> lobby_pick;
  std::optional<Orca::UX::OnlinePick> kept_pick;
  std::optional<Orca::UX::OnlinePick> friends_pick;
  bool friends_pick_played = false;
  bool set_over_left = false;
  bool set_over_hint = false;
  std::optional<Clock::time_point> set_over_hold_since;
  u16 set_over_buttons = 0xFFFF;
  std::unique_ptr<ReplayStream::Writer> replay_out;
  std::unique_ptr<ReplayStream::Player> replay_play;
};

int s_desyncs = 0;

void ReplayFrameHook(Core::System& system, Match& match, int frame, bool resimulating,
                     const std::vector<Orca::Events::PortInfo>& ports, bool read_own,
                     const FrameHook& on_frame, bool restored = false)
{
  auto& replay = match.replay;
  if (restored)
    replay.checksums.erase(replay.checksums.lower_bound(frame), replay.checksums.end());
  on_frame(frame, resimulating, ports, read_own);
  if (frame % 60 == 0 && replay.first_frame >= 0)
  {
    auto& memory = system.GetMemory();
    replay.checksums[frame] = RamChecksum(
        {memory.GetRAM(), memory.GetRamSize()},
        {memory.GetEXRAM(), memory.GetEXRAM() ? memory.GetExRamSize() : 0});
  }
}

std::optional<Orca::Net::Pad> s_local_pad;

std::atomic<bool> s_solo_idle{false};
std::atomic<bool> s_pausing{false};
std::atomic<s64> s_paused_since_ns{0};
std::atomic<s64> s_paused_ns{0};
std::atomic<s64> s_paused_total_ns{0};

s64 NowNs()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
      .count();
}

void AddName(Match& match, Orca::Net::KeyframeInfo::Name name);
std::vector<Orca::Net::KeyframeInfo::Name> AllNames(const Match& match);
void SetNames(Match& match, const Orca::Net::KeyframeInfo& from_host);
void RefreshOwnValues(Match& match, int frame);
std::vector<Orca::Events::PortInfo> PortsAt(Match& match, int frame);
void Leave(Match& match);
void JoinInPlay(Match& match, const std::string& code, bool keep_room = false);

Match& TheMatch()
{
  static Match match;
  return match;
}

int LeadSeat(const Match& match)
{
  return match.session ? match.session->LocalSeat() : match.local_seat;
}

bool IsLocal(const Match& match, int seat)
{
  return match.session ? match.session->IsLocal(seat) :
      seat == match.local_seat || std::ranges::find(match.co_seats, seat) != match.co_seats.end();
}

void ClearQueuedLocal(Match& match)
{
  for (auto& queue : match.queued_local)
    queue.clear();
}

bool SoloIdle(const Match& match)
{
  return match.started && !match.finished && match.port && !match.session && !match.joining &&
         !match.download && !match.job && !match.keyframe_wanted && match.waiting.empty() &&
         match.plugging.empty() && !match.port->CatchingUp();
}

bool Stopping(Core::System& system)
{
  return Core::GetState(system) == Core::State::Stopping ||
         system.GetCPU().GetState() != CPU::State::Running;
}

constexpr std::chrono::microseconds HITCH_TIME{33'334};
constexpr auto PEER_SILENCE_LIMIT = Orca::Net::PEER_SILENCE_LIMIT;
constexpr char PEER_SILENT_SENTENCE[] = "Opponent's game stopped responding";
std::string SilentSentence(const std::vector<std::string>& names)
{
  std::vector<std::string> known;
  for (const std::string& name : names)
  {
    if (!name.empty() && name != "your friend")
      known.push_back(name);
  }
  if (known.empty())
    return PEER_SILENT_SENTENCE;
  std::string who = known[0];
  for (std::size_t i = 1; i < known.size(); ++i)
    who += (i + 1 == known.size() ? " and " : ", ") + known[i];
  return fmt::format("{}'s {} stopped responding", who, known.size() == 1 ? "game" : "games");
}
constexpr std::chrono::seconds LEAVE_SILENCE_LIMIT{3};
constexpr int IDLE_GRACE_FRAMES = 12;
constexpr int MAX_ROLLBACK = 7;
constexpr int FIRST_KEYFRAME_FRAME = 300;
int FirstKeyframeFrame()
{
  return Orca::Mod::ActiveMod() ? static_cast<int>(Orca::Mod::FirstKeyframeFrame()) :
                                  FIRST_KEYFRAME_FRAME;
}
constexpr int CATCH_UP_BEHIND = 12;
constexpr std::chrono::seconds JOIN_LIMIT{90};
constexpr auto ROOM_HOLD = Orca::Net::ROOM_HOLD;
constexpr std::chrono::seconds VERDICT_WAIT{30};
constexpr std::chrono::seconds RANKED_STALL_LIMIT{15};
constexpr std::chrono::milliseconds SET_OVER_HOLD{1500};

constexpr int DEFAULT_DELAY = 3;

std::optional<int> ChosenDelay()
{
  if (const std::optional<int> chosen = Orca::Online::ChosenDelay())
    return chosen;
  if (const char* env = std::getenv("ORCA_INPUT_DELAY"); env && *env)
  {
    const int frames = std::atoi(env);
    if (frames >= 1)
      return std::min(frames, Orca::Net::Config{}.max_delay);
  }
  return DEFAULT_DELAY;
}

constexpr int DELAY_SWITCH_LEAD = 90;
struct PendingDelay
{
  int frames = 0;
  int at = 0;
};
std::optional<PendingDelay> s_pending_delay;

void TakeLobbyDelay(const Match& match)
{
  const int next = match.running + 1;
  if (const std::optional<Orca::Online::DelayRequest> request = Orca::Online::TakeDelayRequest())
  {
    PendingDelay pending{request->frames, request->at};
    if (request->at < 0 && !match.joining)
    {
      pending.at = next + DELAY_SWITCH_LEAD;
      std::fputs(fmt::format("orca delay {} {}\n", pending.frames, pending.at).c_str(), stdout);
      std::fflush(stdout);
    }
    else if (request->at < 0)
    {
      pending.at = next;
    }
    NOTICE_LOG_FMT(ROLLBACK, "Subspace: input delay {} from frame {} (next frame {})",
                   pending.frames, pending.at, next);
    s_pending_delay = pending;
  }
  if (s_pending_delay && next >= s_pending_delay->at)
  {
    if (next > s_pending_delay->at)
    {
      WARN_LOG_FMT(ROLLBACK, "Subspace: input delay {} heard {} frames after frame {}",
                   s_pending_delay->frames, next - s_pending_delay->at, s_pending_delay->at);
    }
    Orca::Online::SetChosenDelay(s_pending_delay->frames);
    Core::DisplayMessage(fmt::format("Buffer changed to {}", s_pending_delay->frames), 4000);
    s_pending_delay.reset();
  }
}

void TakeLobbyVolume(Core::System& system)
{
  if (const std::optional<int> volume = Orca::Online::TakeVolumeRequest())
  {
    Config::SetCurrent(Config::MAIN_AUDIO_MUTED, false);
    Config::SetCurrent(Config::MAIN_AUDIO_VOLUME, *volume);
    AudioCommon::UpdateSoundStream(system);
  }
}

bool FightHoldOn()
{
  static const bool off = Orca::GetEnv("ORCA_TEST_FIGHT_HOLD") == "off";
  return !(off && Orca::UX::TestKnobsAllowed());
}
bool JoinFastOfferOn()
{
  static const bool off = Orca::GetEnv("ORCA_TEST_JOIN_FAST_OFFER") == "off";
  return !(off && Orca::UX::TestKnobsAllowed());
}
bool FreshStartsOn()
{
  static const bool off = Orca::GetEnv("ORCA_TEST_FRESH") == "off";
  return !(off && Orca::UX::TestKnobsAllowed());
}
int FreshFriendsAfter()
{
  static const int after = [] {
    const std::string value = Orca::GetEnv("ORCA_TEST_FRESH_AFTER");
    return value.empty() ? -1 : std::max(0, std::atoi(value.c_str()));
  }();
  return after >= 0 && Orca::UX::TestKnobsAllowed() ? after : -1;
}
int FastUntil()
{
  static const int until = std::atoi(Orca::GetEnv("ORCA_TEST_FAST_UNTIL").c_str());
  return until > 0 && Orca::UX::TestKnobsAllowed() ? until : 0;
}

u32 NewHistorySeed()
{
  if (Orca::UX::TestKnobsAllowed())
  {
    static const std::string fixed = Orca::GetEnv("ORCA_TEST_RNG_SEED");
    if (!fixed.empty())
      return static_cast<u32>(std::strtoul(fixed.c_str(), nullptr, 0));
  }
  if (!Orca::Online::Enabled())
    return 0;
  std::random_device device;
  u32 seed = 0;
  while (seed == 0)
    seed = static_cast<u32>(device());
  return seed;
}

bool OriginReady(const Match& match)
{
  return match.origin_frame >= 0;
}

bool CanonicalBoot(const Match& match)
{
  return !OriginReady(match) && !match.origin_failed;
}

bool InTail(const Match& match)
{
  return match.fresh && match.fresh->phase == Match::Fresh::Phase::Tail;
}

bool Unseen(const Match& match)
{
  return match.fresh && match.fresh->unseen && !InTail(match) && match.port &&
         match.port->CatchingUp();
}

void ShowFresh(Match& match, const char* why)
{
  if (!match.fresh || !match.fresh->unseen)
    return;
  Match::Fresh& fresh = *match.fresh;
  fresh.unseen = false;
  if (!InTail(match) && match.port)
    match.port->SetCatchingUp(false);
  match.stats_frame = -1;
  NOTICE_LOG_FMT(ROLLBACK,
                 "Drop-in: the fresh start shows again at frame {} after {} unseen "
                 "frames past its tail ({:.0f} ms){}{}",
                 match.running + 1, fresh.unseen_frames, MsSince(fresh.tail_end), why ? ": " : "",
                 why ? why : "");
}

std::optional<int> NewRoundTrip(u32* last_sequence)
{
  u32 sequence = 0;
  const int round_trip_ms = Orca::Online::RoundTripMs(&sequence);
  if (round_trip_ms < 0 || sequence == *last_sequence)
    return std::nullopt;
  *last_sequence = sequence;
  return round_trip_ms;
}

void SeedRoundTrips(Match& match)
{
  for (const int round_trip_ms : Orca::Online::RecentRoundTrips(&match.round_trip_sequence))
    match.session->OnRoundTrip(round_trip_ms);
}

void ShowJoinMessage(std::string message, u32 ms)
{
  if (std::ranges::all_of(message, Common::IsPrintableCharacter))
    OSD::AddTypedMessage(OSD::MessageType::OrcaJoin, std::move(message), ms);
}

void StopEmulation(Core::System& system, const std::string& message)
{
  Core::DisplayMessage(message, 10000);
  if (!Stopping(system))
    Core::QueueHostJob([](Core::System& host_system) { Core::Stop(host_system); });
}

IOS::HLE::FS::HostFileSystem* HostNand(Core::System& system)
{
  IOS::HLE::EmulationKernel* ios = system.GetIOS();
  return ios ? static_cast<IOS::HLE::FS::HostFileSystem*>(ios->GetFS().get()) : nullptr;
}

Orca::Net::Config SessionConfig()
{
  Orca::Net::Config config;
  config.seats = Orca::Net::MAX_SEATS;
  config.per_peer_inputs = Orca::Online::RoomDoubles();
  config.max_rollback = MAX_ROLLBACK;
  config.snapshot_every = Orca::TestSnapshotEvery();
  config.fixed_delay = ChosenDelay();
  return config;
}

void StartKeyframe(Core::System& system, Match& match, int frame,
                    std::optional<u64> final_hash = std::nullopt)
{
  for (auto& [seat, arrival] : match.waiting)
    arrival.next_keyframe_notice = 0;
  const auto capture_start = Clock::now();
  auto job = std::make_unique<KeyframeJob>();
  job->frame = frame;
  const Orca::Net::ReplayFrame* boundary = match.replay.At(frame);
  if (!boundary || static_cast<size_t>(frame) > Orca::Net::MAX_REPLAY_FRAMES)
  {
    job->error = "restart the game to accept more players";
    job->refusal = "replay_limit";
    job->done = true;
    match.job = std::move(job);
    return;
  }
  Orca::Net::ReplayArchive replay = match.replay;
  replay.boundary = *boundary;
  replay.frames.resize(static_cast<size_t>(frame - replay.first_frame));
  auto& memory = system.GetMemory();
  replay.target_hash = final_hash ? *final_hash :
      RamChecksum({memory.GetRAM(), memory.GetRamSize()},
                  {memory.GetEXRAM(), memory.GetEXRAM() ? memory.GetExRamSize() : 0});
  job->capture_ms = MsSince(capture_start);
  KeyframeJob* const raw = job.get();
  Orca::Net::KeyframeStore* const store = match.store.get();
  raw->thread = std::thread([raw, store, captured_replay = std::move(replay)] {
    try
    {
      const auto pack_start = Clock::now();
      std::vector<u8> blob = Orca::Net::PackKeyframe(raw->frame, captured_replay);
      raw->raw_size = captured_replay.frames.size() * sizeof(Orca::Net::ReplayFrame);
      if (blob.empty())
      {
        raw->error = "the game's replay is too large; start a new game to accept more players";
        raw->refusal = "replay_limit";
      }
      if (raw->error.empty() && !Orca::Net::EncryptKeyframe(raw->frame, &blob, &raw->info.key))
        raw->error = "encryption failed";
      raw->pack_ms = MsSince(pack_start);
      if (raw->error.empty())
      {
        raw->info.frame = raw->frame;
        raw->info.size = blob.size();
        raw->info.hash = Orca::Net::KeyframeHash(blob);
        raw->info.id = fmt::format("replay-{}-{}", raw->frame, raw->info.hash.substr(0, 8));
        if (Orca::Net::InlineKeyframe(blob.size()))
        {
          raw->info.inline_blob = std::move(blob);
          raw->ok = true;
        }
        else
        {
          const auto put_start = Clock::now();
          raw->ok = store->Put(raw->info, blob, &raw->error);
          raw->put_ms = MsSince(put_start);
          if (!raw->ok)
            raw->refusal = Orca::Net::LastRefusal();
        }
      }
    }
    catch (const std::exception&)
    {
      raw->error = "couldn't prepare the game's replay";
    }
    raw->done = true;
  });
  match.job = std::move(job);
}

void EndFresh(Match& match, const char* why);

Pad FreshPad(Match& match, const Pad& local, int frame)
{
  if (!match.fresh)
    return local;
  Match::Fresh& fresh = *match.fresh;
  using Phase = Match::Fresh::Phase;
  if (fresh.phase != Phase::Tail && fresh.queue &&
      (Orca::Online::RoomEnded() || Orca::Online::Code() != fresh.room))
  {
    EndFresh(match, "the room is gone");
    return local;
  }
  if (fresh.phase == Phase::Hold && Orca::UX::Game().Header().HeaderInPlace())
  {
    const Orca::UX::QueueApp::Identity pick = Orca::UX::QueueApp::OwnIdentity();
    if (Orca::Status::Cap("queue2") && pick.HasPick())
    {
      fresh.phase = Phase::Steer;
      Orca::UX::Game().Css()->ArmOwnPick(pick);
    }
    else
    {
      EndFresh(match, "the room's header is in");
      return local;
    }
  }
  if (fresh.phase == Phase::Steer)
  {
    if (!Orca::UX::Game().Css()->OwnPickArmed())
    {
      NOTICE_LOG_FMT(ROLLBACK, "Drop-in: port 1's pick is back at frame {} ({:.0f} ms after the "
                               "fresh start)",
                     frame, MsSince(fresh.started));
      EndFresh(match, nullptr);
      return local;
    }
    const std::optional<GCPadStatus> steer = Orca::UX::Game().Css()->OwnPickPad();
    return steer ? Orca::Net::EncodePad(*steer) : Pad{};
  }
  return Pad{};
}

void StepTail(Match& match, int frame)
{
  if (!InTail(match))
    return;
  Match::Fresh& fresh = *match.fresh;
  fresh.css_streak = Orca::UX::Game().Read().LastSeen().css ? fresh.css_streak + 1 : 0;
  const int length = frame - match.origin_frame + 1;
  const int offer_from = !fresh.queue && JoinFastOfferOn() &&
                                 FirstKeyframeFrame() < match.origin_frame + FRESH_TAIL_LIMIT ?
                             FirstKeyframeFrame() : 0;
  if (!FreshTailSettled(fresh.kind, fresh.css_streak, length, frame, offer_from))
    return;
  if (fresh.kind == FreshKind::Menu && fresh.css_streak < FRESH_CSS_SETTLE)
  {
    WARN_LOG_FMT(ROLLBACK, "Drop-in: the fresh start's tail found no character select in {} "
                           "frames; it ends at frame {}",
                 FRESH_TAIL_LIMIT, frame);
  }
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: the fresh start's tail ran frames {}-{} in {:.0f} ms",
                 match.origin_frame, frame, MsSince(fresh.started));
  Orca::Events::NoteResync();
  match.stats_frame = -1;
  if (fresh.queue && fresh.kind == FreshKind::Menu)
  {
    fresh.phase = Match::Fresh::Phase::Hold;
    fresh.unseen = FreshUnseen(true, 0);
    fresh.unseen_frames = 0;
    fresh.tail_end = Clock::now();
    if (!fresh.unseen)
      match.port->SetCatchingUp(false);
  }
  else
  {
    match.port->SetCatchingUp(false);
    match.fresh.reset();
  }
}

void StepUnseen(Match& match)
{
  if (!match.fresh || !match.fresh->unseen || InTail(match))
    return;
  Match::Fresh& fresh = *match.fresh;
  ++fresh.unseen_frames;
  if (!FreshUnseen(fresh.queue, fresh.unseen_frames))
    ShowFresh(match, "its unseen frames ran out");
}

void RunSolo(Match& match, const std::function<Pad(int)>& local_pad)
{
  const auto sample = [&](int seat) {
    auto& queue = match.queued_local[seat];
    if (queue.empty())
      return local_pad(seat);
    const Pad pad = queue.front();
    queue.pop_front();
    return pad;
  };
  Pad pad = sample(match.local_seat);
  const bool mask_start = match.doubles && !match.waiting.empty() &&
                          Orca::UX::Game().Read().OnTheMenus();
  if (mask_start)
    pad[0] &= static_cast<u8>(~(PAD_BUTTON_START >> 8));
  const int frame = match.running + 1;
  if (const int until = FastUntil();
      until > 0 && (!match.fast_spent && frame < until) != match.fast)
  {
    match.fast = !match.fast_spent && frame < until;
    Core::SetIsThrottlerTempDisabled(match.fast);
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: frames run {} from frame {} (ORCA_TEST_FAST_UNTIL)",
                   match.fast ? "unthrottled" : "at their own pace", frame);
  }
  Pads pads;
  if (CanonicalBoot(match))
  {
    pads = CanonicalBootPads();
  }
  else
  {
    pads.fill(Orca::Net::UNPLUGGED_PAD);
    pads[match.local_seat] = FreshPad(match, pad, frame);
    for (const int seat : match.co_seats)
    {
      Pad co_pad = sample(seat);
      if (mask_start)
        co_pad[0] &= static_cast<u8>(~(PAD_BUTTON_START >> 8));
      pads[seat] = match.fresh ? Pad{} : co_pad;
    }
  }
  match.port->SetPads(frame, pads);
  match.log.push_back(pads);
  match.running = frame;
  StepUnseen(match);
  StepTail(match, frame);
  if (!match.keyframe && !match.job && match.log.size() > 4 * KEYFRAME_FRESH_FRAMES)
  {
    const int drop = static_cast<int>(match.log.size()) - KEYFRAME_FRESH_FRAMES;
    match.log.erase(match.log.begin(), match.log.begin() + drop);
    match.log_base += drop;
  }
}

void DropFriends(Match& match, const std::string& reason, bool announce = true)
{
  for (int seat = 0; seat < Orca::Net::MAX_SEATS; ++seat)
  {
    if (IsLocal(match, seat) || match.session->LeadOf(seat) != seat ||
        match.session->Plan()[seat].live_from == Orca::Net::NEVER)
      continue;
    Orca::Online::DropPeer(seat, reason);
    match.session->DropSeat(seat);
  }
  match.seated.clear();
  match.plugging.clear();
  Orca::Online::SetOpponentPlugged(false);
  if (reason == "desync")
    Orca::Online::ReportDesync();
  if (announce)
  {
    Orca::Status::State("friend-left " + reason);
    match.friend_left_told = true;
  }
}

void ConfirmResults(Match& match);

void GoSolo(Match& match, const char* why, const char* state = "playing")
{
  ConfirmResults(match);
  Orca::Events::NoteResync();
  match.log = match.session->TakeLog(&match.log_base);
  for (int seat = 0; seat < Orca::Net::MAX_SEATS; ++seat)
  {
    const auto pending = match.session->PendingLocal(seat);
    match.queued_local[seat].assign(pending.begin(), pending.end());
  }
  std::erase_if(match.co_seats, [&](int seat) { return !match.session->IsLocal(seat); });
  Orca::Net::Stats stats = match.session->GetStats();
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: back to solo play at frame {} ({}): {} rollbacks, {} checksums matched",
                 match.running + 1, why, stats.rollbacks, stats.checksums_matched);
  match.session.reset();
  Orca::Online::SetSessionActive(false);
  match.plugging.clear();
  match.idle_frames = 0;
  match.port->SetCatchingUp(false);
  match.port->SetResimulating(false);
  if (state)
    Orca::Status::State(state);
}

bool Soft()
{
  return Orca::Status::Cap("join");
}

std::unique_ptr<Orca::Net::KeyframeStore> MakeStore()
{
  return Orca::Net::MakeKeyframeStore(
      [](bool fresh, std::string* ticket, std::string* store_url, std::string* error) {
        return Orca::Online::FreshTicket(fresh, ticket, store_url, error);
      });
}

void DeleteStored(Match& match, const Orca::Net::KeyframeInfo& info)
{
  if (info.shared && match.joining)
    return;
  if (match.store && info.inline_blob.empty())
    match.store->Delete(info.id);
}

void EndFresh(Match& match, const char* why)
{
  if (!match.fresh || InTail(match))
    return;
  if (why)
  {
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: the fresh start hands port 1 back to the player at frame {} "
                             "({})",
                   match.running + 1, why);
  }
  ShowFresh(match, nullptr);
  if (match.fresh->phase == Match::Fresh::Phase::Steer)
    Orca::UX::Game().Css()->DisarmOwnPick();
  match.fresh.reset();
}

void ResetDropIn(Match& match)
{
  match.opponent_no_show = false;
  match.replaying.reset();
  match.before_join.reset();
  match.fresh_pending.reset();
  match.fresh_join_warned_at.reset();
  match.fresh_queue_armed = false;
  EndFresh(match, "the room is left");
  if (match.keyframe)
    DeleteStored(match, *match.keyframe);
  match.keyframe.reset();
  if (match.store)
    match.store->Cancel();
  match.waiting.clear();
  match.menu_waiting.clear();
  match.batch.Clear();
  match.doubles = false;
  match.doubles_seats.clear();
  for (const auto& shared : match.shared_keyframes)
    DeleteStored(match, shared);
  match.shared_keyframes.clear();
  match.seated.clear();
  match.plugging.clear();
  match.keyframe_wanted = false;
  match.header_waited = 0;
  std::thread([job = std::move(match.job), download = std::move(match.download),
               store = std::move(match.store)]() mutable {
    job.reset();
    download.reset();
    store.reset();
  }).detach();
  match.store = MakeStore();
}

Orca::Engine::Mode QueueMode(const std::string& queue)
{
  if (queue == "casual")
    return Orca::Engine::Mode::Casual;
  if (queue == "ranked")
    return Orca::Engine::Mode::Ranked;
  return Orca::Engine::Mode::None;
}

u8 RoomFlags()
{
  return Orca::Status::Cap("queue2") ? Orca::Engine::FLAG_QUEUE2 : u8{0};
}

std::pair<Orca::Engine::Mode, u8> WantedRulesMode(const Match& match)
{
  using Orca::Engine::Mode;
  namespace MB = Orca::Engine;
  if (match.joining)
    return {Mode::None, 0};
  const Mode mode =
      Orca::Online::RoomEnded() ? Mode::None : QueueMode(Orca::Online::RoomQueue());
  if (mode != Mode::None &&
      (match.local_seat == 0 ||
       ((match.linger_until || Orca::Online::SetOver()) && mode == Mode::Ranked)))
  {
    return {mode, RoomFlags()};
  }
  if (mode == Mode::None && Orca::UX::QueueApp::Active() && Orca::Status::Cap("queue2") &&
      match.local_seat == 0 && !match.session && !match.linger_until)
  {
    return {Orca::UX::QueueApp::Ranked() ? Mode::Ranked : Mode::Casual,
            static_cast<u8>(MB::FLAG_SOLO | MB::FLAG_QUEUE2)};
  }
  return {Mode::None, 0};
}

void UpdateWanted(const Match& match)
{
  const auto [mode, flags] = WantedRulesMode(match);
  const bool room = mode != Orca::Engine::Mode::None &&
                    (flags & Orca::Engine::FLAG_SOLO) == 0;
  if (Orca::UX::Game().Header().SetWanted(mode, flags, room ? Orca::Online::Code() : std::string()))
  {
    DEBUG_LOG_FMT(ROLLBACK, "Online rules: header {} flags {:#04x} wanted from frame {}",
                  static_cast<int>(mode), flags, match.running + 1);
  }
}

void DropStaleKeyframe(Match& match)
{
  if (!match.keyframe || match.session ||
      match.keyframe_generation == Orca::UX::Game().Header().WantedGeneration())
  {
    return;
  }
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: keyframe of frame {} dropped: made for another header",
                 match.keyframe->frame);
  DeleteStored(match, *match.keyframe);
  match.keyframe.reset();
  if (!match.waiting.empty())
    match.keyframe_wanted = true;
}

bool SoloQuiet(const Match& match)
{
  return match.started && !match.finished && match.port && !match.session && !match.joining &&
         !match.download && !match.job && match.plugging.empty() &&
         (!match.port->CatchingUp() ||
          HeaderFreeWhileCatchingUp(Unseen(match), InTail(match), match.joining));
}

void FailHeaderWait(Match& match)
{
  WARN_LOG_FMT(ROLLBACK,
               "Online rules: room {}: this game's header isn't the room's after {} boundaries "
               "with the opponent waiting, frame {}: leaving (mismatch)",
               Orca::Online::Code(), HEADER_FAIL_BOUNDARIES, match.running + 1);
  Orca::Online::SetLeaveReason("mismatch");
  Orca::Status::Report("mismatch", "That match couldn't start");
  Leave(match);
}

void ForgetRoom(Match& match)
{
  match.reopen_join = false;
  match.room_lost = false;
  match.reopen_gave_up = false;
  match.unstable_reopening = false;
  match.unstable_hosted_since.reset();
  match.reopen_attempts = 0;
  match.network_failure_since.reset();
  match.reopen_backoff = {};
  match.home_code.clear();
  match.reopen_pending.reset();
  match.tell_no_room = false;
}

constexpr std::chrono::seconds REOPEN_DELAY{1};

void ReopenRoom(Match& match)
{
  ForgetRoom(match);
  Orca::Online::ShutdownInBackground();
  if (match.local_seat == 0)
    match.reopen_pending = Clock::now() + REOPEN_DELAY;
  else
    match.tell_no_room = true;
}

void Reseat(Match& match)
{
  if (match.local_seat == 0)
    return;
  if (match.local_seat < 0 || match.local_seat >= Orca::Net::MAX_SEATS)
  {
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: no seat was assigned; playing on port 1 from frame {}",
                   match.running + 1);
    ClearQueuedLocal(match);
  }
  else
  {
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: port {} moves to port 1 from frame {}",
                   Orca::Online::SeatPort(match.local_seat) + 1, match.running + 1);
    auto pending = std::move(match.queued_local[match.local_seat]);
    ClearQueuedLocal(match);
    match.queued_local[0] = std::move(pending);
  }
  match.local_seat = 0;
  match.co_seats.clear();
  Orca::UX::SetLocalPadCount(1);
  match.port_names.clear();
}

void AfterFriendsGone(Match& match)
{
  Orca::Online::SetOpponentPlugged(false);
  const std::string queue = Orca::Online::RoomQueue();
  if (queue == "private" || Orca::Online::RoomEnded() || match.linger_until)
    return;
  if (!std::exchange(match.friend_left_told, true))
    Orca::Status::State("friend-left left");
  if (queue == "ranked" && Orca::Online::SetOver())
  {
    NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: the opponent left the set's room (set over): this "
                             "game stays until its player leaves");
    return;
  }
  if (queue == "ranked" && Orca::Online::MatchLive())
  {
    NOTICE_LOG_FMT(ROLLBACK,
                   "Matchmaking: the opponent went mid-set: waiting for the room's verdict");
    match.linger_until = Clock::now() + VERDICT_WAIT;
    return;
  }
  NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: the {} pairing is over: a room of this game's own", queue);
  ResetDropIn(match);
  ReopenRoom(match);
}

void KeepRoomOpen(Match& match, Clock::time_point now = Clock::now())
{
  if (match.local_seat != 0)
    return;
  if (match.reopen_pending)
  {
    if (now < *match.reopen_pending)
      return;
    match.reopen_pending.reset();
    Orca::Online::StartRoom("", false);
    return;
  }
  const auto give_up = [&] {
    match.reopen_gave_up = true;
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: room {} stopped reopening after {} attempts",
                   Orca::Online::Code(), match.reopen_attempts);
    Orca::Status::Report("network", "The room couldn't reconnect. Your game continues solo; "
                                     "start a new game to open a room.");
    Orca::Status::Event("no-room");
  };
  if (!Orca::Online::RoomEnded())
  {
    if (Orca::Online::InRoom())
    {
      if (Orca::Online::Joining().value_or(false))
      {
        if (Pausing())
          return;
        JoinInPlay(match, Orca::Online::Code(), true);
        match.reopen_join = true;
        return;
      }
      if (match.room_lost)
      {
        NOTICE_LOG_FMT(ROLLBACK, "Drop-in: hosting room {} again", Orca::Online::Code());
        if (Soft())
          Orca::Status::Event("playing");
      }
      match.room_lost = false;
      match.reopen_gave_up = false;
      if (match.unstable_reopening)
      {
        if (!match.unstable_hosted_since)
          match.unstable_hosted_since = now;
        if (now - *match.unstable_hosted_since >= std::chrono::minutes(5))
        {
          match.unstable_reopening = false;
          match.unstable_hosted_since.reset();
        }
      }
      if (!match.unstable_reopening)
      {
        match.reopen_attempts = 0;
        match.network_failure_since.reset();
      }
      match.reopen_backoff = {};
      match.home_code = Orca::Online::Code();
    }
    else if (!match.reopen_gave_up && !match.session && match.network_failure_since &&
             now - *match.network_failure_since >= std::chrono::minutes(5))
    {
      give_up();
      Orca::Online::ShutdownInBackground();
    }
    return;
  }
  match.unstable_hosted_since.reset();
  if (match.session)
    return;
  const std::string code = Orca::Online::RoomErrorCode();
  if (code.empty())
    return;
  if (code == "match-over")
  {
    NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: the room closed after its result; a room of its own");
    Orca::Status::Event("friend-left match-over");
    Orca::UX::Search::End();
    match.linger_until.reset();
    ResetDropIn(match);
    ForgetRoom(match);
    Orca::Online::ShutdownInBackground();
    Orca::Online::StartRoom("", false);
    return;
  }
  if (match.reopen_gave_up)
    return;
  if (code == "room-unstable")
    match.unstable_reopening = true;
  if (code == "network" || match.unstable_reopening)
  {
    if (!match.network_failure_since)
      match.network_failure_since = now;
  }
  else
  {
    match.network_failure_since.reset();
  }
  if (match.reopen_attempts >= 10 ||
      (match.network_failure_since && now - *match.network_failure_since >= std::chrono::minutes(5)))
  {
    give_up();
    return;
  }
  if (!match.room_lost)
  {
    match.room_lost = true;
    match.room_lost_at = now;
    match.reopen_backoff = std::chrono::seconds(2);
    const auto delay = code == "room-unstable" ? std::chrono::seconds(0) : match.reopen_backoff;
    match.reopen_at = now + delay;
    const std::string why = Orca::Online::StatusLine();
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: this game's room ended ({}: {}); another opens in {} s",
                   code, why, delay.count());
    Core::DisplayMessage(why, 6000);
    if (Soft())
    {
      if (code == "room-unstable")
        Orca::Status::Report("network", "The room keeps disconnecting. Opening a new room; "
                                         "send your friend a new invite.");
      else if (Orca::Online::RoomTaken() || code == "room_full")
        Orca::Status::Event("playing");
      else
        Orca::Status::Report("network", fmt::format("{}: your game opens its room again", why));
    }
    ResetDropIn(match);
  }
  match.home_code = RoomReopenCode(match.home_code, Orca::Online::RoomQueue(),
                                   now - match.room_lost_at < ROOM_HOLD,
                                   Orca::Online::RoomTaken() || code == "room_full" ||
                                       code == "room-unstable");
  if (now < match.reopen_at)
    return;
  const std::string reopen = match.home_code;
  ++match.reopen_attempts;
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: reopening room {} attempt={} (previous room={}, ended: {}: {})",
                 reopen.empty() ? "new" : reopen, match.reopen_attempts, Orca::Online::Code(), code,
                 Orca::Online::StatusLine());
  Orca::Online::ShutdownInBackground();
  Orca::Online::StartRoom(reopen, false, true, !reopen.empty());
  match.reopen_backoff = std::min(match.reopen_backoff * 2, std::chrono::seconds(30));
  match.reopen_at = now + match.reopen_backoff;
}

void BecomeSolo(Match& match, const char* why, const char* state, bool own_boot = false,
                bool keep_room = false)
{
  Orca::Events::NoteResync();
  if (match.session)
  {
    GoSolo(match, why, nullptr);
  }
  else
  {
    match.log.clear();
    match.log_base = match.running + 1;
    ClearQueuedLocal(match);
  }
  if (own_boot)
    Reseat(match);
  match.joining = false;
  match.join_in_play = false;
  match.joined = false;
  match.host_here = false;
  match.host_holding = false;
  match.leave_requested = false;
  match.leave_at = -1;
  match.last_percent = -1;
  match.host_room_wait.Back();
  match.later_names.reset();
  OSD::DiscardTypedMessage(OSD::MessageType::OrcaJoin);
  Orca::UX::Search::JoinOver();
  match.port->SetCatchingUp(false);
  ResetDropIn(match);
  if (!keep_room)
    ReopenRoom(match);
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: playing solo on port {} from frame {} ({})",
                 Orca::Online::SeatPort(match.local_seat) + 1, match.running + 1, why);
  if (state)
    Orca::Status::State(state);
}

bool WaitForAwayHost(Match& match)
{
  if (!match.joining || match.leave_requested || Orca::Online::RoomQueue() != "private" ||
      !match.host_room_wait.Start(Clock::now(), Orca::Online::HostAwayDeadline()))
    return false;
  const auto deadline = match.host_room_wait.until;
  BecomeSolo(match, "host room away", nullptr, false, true);
  match.host_room_wait.until = deadline;
  match.joining = true;
  match.join_in_play = true;
  match.host_gone.reset();
  match.boot_time = Clock::now();
  match.offered_time = match.boot_time;
  match.join_status.clear();
  Orca::Online::RetryPeer(match.host_seat);
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: host room away; session ended, staying in room {} for back",
                 Orca::Online::Code());
  Orca::Status::Event("room-away host ended waiting-back");
  return true;
}

bool InQueueRoom();

bool QueueOrSearch()
{
  return Orca::UX::QueueApp::Active() ||
         Orca::UX::Search::Current() != Orca::UX::Search::State::None;
}

void EndQueueForFriend(Match& match, bool arrived, bool invited)
{
  FriendQueueInputs in;
  in.friend_coming = arrived || invited;
  in.queue_or_search = QueueOrSearch();
  in.local_seat = match.local_seat;
  in.joining = match.joining;
  in.friends_room = !Orca::Online::RoomEnded() && Orca::Online::Seat() >= 0 &&
                    Orca::Online::RoomQueue() == "private";
  if (!FriendEndsQueue(in))
    return;
  NOTICE_LOG_FMT(ROLLBACK,
                 "Queue: a friend {} room {} at frame {} while this player is on the {} queue: "
                 "the queue is over (orca menu cancel), so the friend's keyframe carries no queue "
                 "header",
                 arrived ? "arrived in" : "is invited to", Orca::Online::Code(),
                 match.running + 1,
                 (Orca::UX::QueueApp::Active() ? Orca::UX::QueueApp::Ranked() :
                                              Orca::UX::Search::Ranked()) ?
                     "ranked" :
                     "casual");
  if (Orca::UX::QueueApp::Active())
  {
    match.friends_pick =
        Orca::UX::QueueApp::Ranked() ? Orca::UX::OnlinePick::Ranked : Orca::UX::OnlinePick::Casual;
    match.friends_pick_played = false;
  }
  Orca::UX::Search::End();
  Orca::UX::QueueApp::End();
  Orca::Status::Menu("cancel");
}

void RetireReassignedCoSeats(Match& match, Orca::Net::RelayRoom* test_room = nullptr)
{
  if (!match.session || match.joining)
    return;
  for (int seat = 0; seat < Orca::Net::MAX_SEATS; ++seat)
  {
    if (match.session->IsLocal(seat) || match.session->LeadOf(seat) == seat)
      continue;
    const auto group = test_room ? test_room->GroupOf(seat) : Orca::Online::GroupOf(seat);
    if (!group.empty() && group.front() != match.session->LeadOf(seat))
    {
      match.session->UnplugSeat(seat);
      match.session->PrepareAdmission(seat);
    }
  }
}

void HostEvents(Core::System& system, Match& match, Orca::Net::RelayRoom* test_room = nullptr,
                std::optional<bool> test_menus = std::nullopt)
{
  ++match.host_boundaries;
  match.doubles = test_room ? test_room->Doubles() : Orca::Online::RoomDoubles();
  bool locals_changed = false;
  if (!match.session && (test_room || Orca::Online::InRoom()))
  {
    if (const auto locals = test_room ? test_room->CoSeatsForSolo() : Orca::Online::CoSeatsForSolo())
    {
      locals_changed = *locals != match.co_seats;
      match.co_seats = *locals;
      Orca::UX::SetLocalPadCount(match.co_seats.empty() ? 1 : 2);
    }
  }
  const bool in_room = test_room ? test_room->Connected() : Orca::Online::InRoom();
  const bool in_queue_room = test_room ? QueueMode(test_room->PlayQueue()) != Orca::Engine::Mode::None :
                                         InQueueRoom();
  if (match.fresh_queue_armed && in_room && !in_queue_room)
    match.fresh_queue_armed = false;
  const bool on_menus = test_menus.value_or(Orca::UX::Game().Read().OnTheMenus());
  if (on_menus)
  {
    match.waiting.merge(match.menu_waiting);
    match.menu_waiting.clear();
  }
  bool arrived = false;
  for (const Orca::Net::PeerEvent& event :
       (test_room ? test_room->TakePeerEvents() : Orca::Online::TakePeerEvents()))
  {
    if (event.kind == Orca::Net::PeerEvent::Kind::Arrived)
    {
      if (event.seat <= 0 || event.seat >= Orca::Net::MAX_SEATS || event.host)
      {
        WARN_LOG_FMT(ROLLBACK, "Drop-in: ignoring a player at seat {}{}", event.seat + 1,
                     event.host ? " that says it hosts" : "");
        continue;
      }
      const bool capable = match.doubles && event.doubles &&
          std::ranges::all_of(match.seated, [&](int seat) {
            return seat == event.seat || match.doubles_seats.contains(seat);
          }) && std::ranges::all_of(match.waiting, [&](const auto& entry) {
            return entry.first == event.seat || entry.second.doubles;
          }) && std::ranges::all_of(match.menu_waiting, [&](const auto& entry) {
            return entry.first == event.seat || entry.second.doubles;
          });
      if ((!event.co_seats.empty() && !capable) ||
          (match.doubles && !event.doubles) || (!capable && (std::any_of(match.seated.begin(), match.seated.end(),
                      [&event](int seat) { return seat != event.seat; }) ||
          std::any_of(match.waiting.begin(), match.waiting.end(),
                      [&event](const auto& waiting) { return waiting.first != event.seat; }) ||
          std::any_of(match.menu_waiting.begin(), match.menu_waiting.end(),
                      [&event](const auto& waiting) { return waiting.first != event.seat; }))))
      {
        NOTICE_LOG_FMT(ROLLBACK,
                       "Drop-in: {} arrived for port {}: room_full",
                       event.name, Orca::Online::SeatPort(event.seat) + 1);
        if (test_room)
          test_room->DropPeer(event.seat, "room_full");
        else
          Orca::Online::DropPeer(event.seat, "room_full");
        continue;
      }
      NOTICE_LOG_FMT(ROLLBACK, "Drop-in: {} arrived for port {}", event.name, Orca::Online::SeatPort(event.seat) + 1);
      Core::DisplayMessage(fmt::format("{} is joining in Port {}", event.name, Orca::Online::SeatPort(event.seat) + 1), 4000);
      auto& arrivals = match.doubles && !on_menus && !event.recovering ?
                          match.menu_waiting : match.waiting;
      arrivals[event.seat] = {event.name, event.controls, event.queue, event.recovering,
                              event.doubles, Clock::now(), match.host_boundaries};
      arrivals[event.seat].lead = event.seat;
      for (const int co : event.co_seats)
      {
        arrivals[co] = {test_room ? test_room->SeatName(co) : Orca::Online::SeatName(co), {}, {},
                        event.recovering, event.doubles, Clock::now(), match.host_boundaries};
        arrivals[co].lead = event.seat;
      }
      if (event.recovering)
      {
        if (match.doubles && match.session)
          match.session->DropSeat(event.seat);
        const int from = match.doubles && match.session && !match.session->LocalChecksums().empty() ?
                             match.session->LocalChecksums().rbegin()->first : match.running + 1;
        match.recovery_keyframe_from = std::max(match.recovery_keyframe_from, from);
      }
      match.keyframe_wanted = true;
      arrived = true;
      Orca::Status::State("friend-joining");
    }
    else if (event.kind == Orca::Net::PeerEvent::Kind::CoSeatLeft)
    {
      match.waiting.erase(event.seat);
      match.menu_waiting.erase(event.seat);
      match.plugging.erase(event.seat);
      match.seated.erase(event.seat);
      match.doubles_seats.erase(event.seat);
      if (match.session)
        match.session->UnplugSeat(event.seat);
    }
    else if (event.kind == Orca::Net::PeerEvent::Kind::Left)
    {
      const bool was_waiting = (match.waiting.erase(event.seat) + match.menu_waiting.erase(event.seat)) > 0;
      match.plugging.erase(event.seat);
      match.doubles_seats.erase(event.seat);
      const bool was_playing = match.seated.erase(event.seat) > 0;
      if (match.session)
      {
        NOTICE_LOG_FMT(ROLLBACK, "Drop-in: port {} left the room{}{}", Orca::Online::SeatPort(event.seat) + 1,
                       event.reason.empty() ? "" : ": ", event.reason);
        match.session->DropSeat(event.seat);
      }
      if (was_playing && match.seated.empty())
        Orca::Online::SetOpponentPlugged(false);
      if (was_waiting || was_playing)
      {
        Orca::Status::State("friend-left " +
                            (event.reason.empty() ? std::string("left") : event.reason));
        match.friend_left_told = true;
      }
    }
  }
  if (match.plugging.empty())
  {
    for (const auto& shared : match.shared_keyframes)
      DeleteStored(match, shared);
    match.shared_keyframes.clear();
  }
  const bool invited = Orca::Online::TakePrepareJoin();
  if (match.origin_failed && (!match.waiting.empty() || !match.menu_waiting.empty()))
  {
    for (const auto& [seat, arrival] : match.waiting)
      Orca::Online::DropPeer(seat, "refused");
    for (const auto& [seat, arrival] : match.menu_waiting)
      Orca::Online::DropPeer(seat, "refused");
    match.waiting.clear();
    match.menu_waiting.clear();
    match.keyframe_wanted = false;
    Orca::Status::State("friend-left refused");
    match.friend_left_told = true;
  }
  EndQueueForFriend(match, arrived, invited);
  UpdateWanted(match);

  const auto now = Clock::now();
  for (auto& [seat, arrival] : match.waiting)
  {
    if (match.job)
      arrival.next_keyframe_notice = 0;
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - arrival.arrived_at);
    if (!arrival.next_keyframe_notice || elapsed.count() < arrival.next_keyframe_notice)
      continue;
    const int next = match.running + 1;
    const bool in_place = Orca::UX::Game().Header().HeaderInPlace();
    int waited = match.header_waited;
    const bool header_ready = HeaderWaitAt(&waited, InTail(match), Unseen(match), in_place,
                                           true, in_queue_room) == HeaderWait::Ready;
    const auto capture = ChooseJoinCapture(match.session.get(), match.doubles, next,
        std::max(match.replay.first_frame + 1, match.recovery_keyframe_from));
    const char* fresh_reason = !match.fresh ? "none" :
        InTail(match) ? "tail" : match.fresh->phase == Match::Fresh::Phase::Hold ?
        "queue header hold" : "restoring own pick";
    NOTICE_LOG_FMT(ROLLBACK,
                   "Drop-in: no keyframe yet {} s after {} arrived (wanted={}, header in place={}, "
                   "running frame {}, boundaries since arrival {}, store={}, job={}, header ready={}, "
                   "next={}, first keyframe frame={} (reached={}), recovery_keyframe_from={} "
                   "(reached={}), origin ready={}, replay.first_frame={} (past={}), fresh={} "
                   "(reason={}), capture present={}, fresh pending={}, queue fresh armed={}, "
                   "room ready={}, queue room={}, room queue={}, holding={})",
                   elapsed.count(), arrival.name, match.keyframe_wanted, in_place, match.running,
                   match.host_boundaries - arrival.arrived_boundary, match.store != nullptr,
                   match.job != nullptr, header_ready, next, FirstKeyframeFrame(),
                   next >= FirstKeyframeFrame(), match.recovery_keyframe_from,
                   next >= match.recovery_keyframe_from, OriginReady(match), match.replay.first_frame,
                   next > match.replay.first_frame, match.fresh.has_value(), fresh_reason,
                   capture.has_value(), match.fresh_pending.has_value(), match.fresh_queue_armed,
                   in_room, in_queue_room,
                   test_room ? test_room->Queue() : Orca::Online::RoomQueue(), match.holding);
    arrival.next_keyframe_notice = arrival.next_keyframe_notice == 15 ? 45 : 0;
  }

  if (match.job && match.job->done)
  {
    std::unique_ptr<KeyframeJob> job = std::move(match.job);
    if (job->thread.joinable())
      job->thread.join();
    if (!job->ok && !job->refusal.empty())
    {
      const std::string why = job->refusal == "signed_out" || job->refusal == "replay_limit" ?
                                  job->refusal : "refused";
      ERROR_LOG_FMT(ROLLBACK, "Drop-in: keyframe of frame {} refused ({}): {}; {} waiting told",
                    job->frame, job->refusal, job->error, match.waiting.size());
      for (const auto& [seat, arrival] : match.waiting)
        Orca::Online::DropPeer(seat, why);
      if (!match.waiting.empty())
      {
        match.waiting.clear();
        Orca::Status::State("friend-left " + why);
        match.friend_left_told = true;
      }
      match.keyframe_wanted = false;
    }
    else if (!job->ok)
    {
      ERROR_LOG_FMT(ROLLBACK, "Drop-in: keyframe of frame {} failed: {}", job->frame, job->error);
      match.keyframe_wanted = !match.waiting.empty();
    }
    else
    {
      NOTICE_LOG_FMT(ROLLBACK,
                     "Drop-in: keyframe of frame {}: {} bytes raw, {} bytes {}; capture {:.1f} ms, "
                     "pack and encrypt {:.1f} ms, store {:.0f} ms",
                     job->frame, job->raw_size, job->info.size,
                     job->info.inline_blob.empty() ? "stored" : "inline in the room's offer",
                     job->capture_ms, job->pack_ms, job->put_ms);
      if (match.keyframe)
        DeleteStored(match, *match.keyframe);
      match.keyframe = job->info;
      match.keyframe_generation = job->generation;
      if (!match.session && job->frame >= match.log_base &&
          job->frame <= match.log_base + static_cast<int>(match.log.size()))
      {
        match.log.erase(match.log.begin(), match.log.begin() + (job->frame - match.log_base));
        match.log_base = job->frame;
      }
    }
  }

  RetireReassignedCoSeats(match, test_room);

  const bool recovering = !match.waiting.empty() && std::ranges::all_of(match.waiting, [](const auto& entry) {
    return entry.second.recovering;
  });
  const bool batch_ready = !match.doubles || match.batch.Ready(
      on_menus, static_cast<int>(match.waiting.size()),
      static_cast<int>(match.plugging.size()), recovering, Clock::now(),
      1 + static_cast<int>(match.co_seats.size()));
  if (match.waiting.empty())
    match.fight_let_in = false;
  const bool let_in = Orca::Online::TakeLetIn();
  if (let_in && match.fight_holding && !match.waiting.empty())
  {
    match.fight_let_in = true;
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: let-in ends the fight hold at frame {}", match.running + 1);
  }
  FightHoldInputs fight;
  fight.enabled = FightHoldOn();
  fight.fight = Orca::UX::Game().Read().FightInProgress() &&
                !match.fresh && !match.fresh_pending;
  fight.waiting = !match.waiting.empty();
  fight.joining = match.joining;
  fight.session = match.session != nullptr;
  fight.seated_or_plugging = !match.seated.empty() || !match.plugging.empty();
  fight.doubles = match.doubles;
  fight.in_queue_room = in_queue_room || match.fresh_queue_armed;
  fight.let_in = match.fight_let_in;
  fight.recovering = std::ranges::any_of(match.waiting, [](const auto& entry) {
    return entry.second.recovering;
  });
  const bool was_fight_holding = match.fight_holding;
  match.fight_holding = FightJoinHeld(fight);
  const bool hold = !match.waiting.empty() &&
      (Orca::UX::Game().Read().DropInHeld() || !batch_ready || match.fight_holding);
  const bool notice_hold = hold || !match.menu_waiting.empty();
  if (notice_hold != match.holding ||
      (notice_hold && was_fight_holding != match.fight_holding))
  {
    match.holding = notice_hold;
    if (test_room)
      test_room->HoldJoins(notice_hold, match.fight_holding);
    else
      Orca::Online::HoldJoins(notice_hold, match.fight_holding);
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: {} at frame {}",
                   match.fight_holding ? "the join waits for this match to end" :
                   notice_hold ? "the join waits for the menus or its batch" : "the join goes on",
                   match.running + 1);
    if (match.fight_holding)
    {
      const std::string& name = match.waiting.begin()->second.name;
      Orca::UX::ShowToast(fmt::format("{} joins when this match ends",
                                     name.empty() ? "Your friend" : name));
    }
    if (notice_hold)
      Orca::Status::State("friend-holding");
    else if (!match.waiting.empty())
      Orca::Status::State("friend-joining");
  }
  if (hold || (match.waiting.empty() && !match.menu_waiting.empty()))
    return;

  if (match.waiting.empty())
    match.friends_in_place = false;
  FreshInputs start;
  start.kind = FreshKindForOrigin(match.origin_rule);
  start.enabled = FreshStartsOn() && match.origin_frame > 0;
  start.origin_ready = OriginReady(match);
  start.joining = match.joining;
  start.session = match.session != nullptr;
  start.seated_or_plugging = !match.seated.empty() || !match.plugging.empty();
  start.busy = InTail(match) || match.job != nullptr || match.fresh_pending.has_value();
  start.queue_armed = match.fresh_queue_armed;
  start.in_queue_room = in_queue_room;
  start.waiting = !match.waiting.empty();
  start.in_place = match.friends_in_place ||
                   (!start.in_queue_room && std::ranges::any_of(match.waiting, [](const auto& entry) {
                     return entry.second.recovering;
                   }));
  start.single_player = Orca::UX::Game().Read().DropInHeld();
  start.history_frames = match.running + 1 - match.replay.first_frame;
  const u64 ticks = system.GetCoreTiming().GetTicks();
  start.history_time = std::chrono::duration<double>(
      static_cast<double>(ticks >= match.origin_ticks ? ticks - match.origin_ticks : 0) /
      system.GetSystemTimers().GetTicksPerSecond());
  start.fresh_after = match.fight_let_in ? 0 : FreshFriendsAfter();
  switch (WarnBeforeResetJoin(DecideFreshStart(start), start.kind, start.queue_armed,
                             &match.fresh_join_warned_at, Clock::now()))
  {
  case FreshStep::None:
    match.friends_in_place = !match.waiting.empty();
    break;
  case FreshStep::Hold:
    return;
  case FreshStep::Warn:
    Orca::UX::ShowRestartForJoin(match.waiting.begin()->second.name);
    return;
  case FreshStep::Now:
    if (start.kind == FreshKind::Reset)
    {
      match.fresh_pending = 0;
      NOTICE_LOG_FMT(ROLLBACK, "Drop-in: a reset start at the next boundary (frame {})",
                     match.running + 2);
    }
    else if (start.queue_armed)
    {
      match.fresh_pending = Orca::Online::RoomQueue() == "ranked" ? Orca::Net::MENU_EXIT_RANKED :
                                                                    Orca::Net::MENU_EXIT_CASUAL;
      NOTICE_LOG_FMT(ROLLBACK, "Drop-in: {} room {}: a fresh start at the next boundary (frame {})",
                     Orca::Online::RoomQueue(), Orca::Online::Code(), match.running + 2);
    }
    else
    {
      const std::optional<Orca::UX::OnlinePick> pick = Orca::UX::Game().Read().CssPick();
      match.fresh_pending = FriendsFreshExit(
          pick ? std::optional(*pick == Orca::UX::OnlinePick::Ranked) : std::nullopt);
      NOTICE_LOG_FMT(ROLLBACK, "Drop-in: a friend arrives after {} frames of this game's own: a "
                               "fresh start at the next boundary (frame {}, exit {})",
                     start.history_frames, match.running + 2, *match.fresh_pending);
    }
    return;
  }

  DropStaleKeyframe(match);
  const u64 generation = Orca::UX::Game().Header().WantedGeneration();
  const bool in_place = Orca::UX::Game().Header().HeaderInPlace();
  const HeaderWait header = HeaderWaitAt(&match.header_waited, InTail(match), Unseen(match),
                                         in_place, !match.waiting.empty(), in_queue_room);
  if (header == HeaderWait::Fail)
  {
    FailHeaderWait(match);
    return;
  }
  const bool header_ready = header == HeaderWait::Ready;
  if (!in_place && match.keyframe_wanted && match.header_waited % 60 == 1)
  {
    NOTICE_LOG_FMT(ROLLBACK, "Online rules: a keyframe waits for the room's header (frame {}{})",
                   match.running + 1,
                   header_ready ? "; a friends room's, made anyway" : "");
  }

  const int next = match.running + 1;
  const bool usable = match.keyframe && match.keyframe_generation == generation;
  const bool fresh = usable && match.keyframe->frame >= match.recovery_keyframe_from &&
                     next - match.keyframe->frame <= KEYFRAME_FRESH_FRAMES;
  bool capture_failed = false;
  const auto capture = ChooseJoinCapture(match.session.get(), match.doubles, next,
      std::max(match.replay.first_frame + 1, match.recovery_keyframe_from));
  if (match.keyframe_wanted && !match.job && match.store && header_ready)
  {
    if (fresh)
      match.keyframe_wanted = false;
    else if (next >= FirstKeyframeFrame() && next >= match.recovery_keyframe_from &&
             OriginReady(match) && next > match.replay.first_frame &&
             !match.fresh && capture)
    {
      match.keyframe_wanted = false;
      StartKeyframe(system, match, capture->frame, capture->hash);
      if (match.job)
      {
        match.job->generation = generation;
      }
      else
      {
        capture_failed = true;
        match.keyframe_wanted = !match.waiting.empty();
      }
    }
  }
  if (!match.store && !match.waiting.empty())
    WARN_LOG_FMT(ROLLBACK, "Drop-in: no keyframe store: a friend can't join yet");

  if (!match.keyframe || match.waiting.empty() || match.job || s_pausing ||
      (capture_failed && !match.session) || !header_ready || !usable)
  {
    return;
  }
  if (match.keyframe->frame < match.recovery_keyframe_from)
    return;
  if (!match.session && (locals_changed ||
      (match.keyframe->frame >= match.log_base &&
       !(test_room ? test_room->TryStartSession(match.co_seats) :
                     Orca::Online::TryStartSession(match.co_seats)))))
    return;
  int live_from = next;
  if (match.doubles && match.session)
    for (const auto& [seat, arrival] : match.waiting)
      if (match.session->Plan()[seat].unplug_from != Orca::Net::NEVER)
        live_from = std::max(live_from, match.session->Plan()[seat].unplug_from);
  const bool grouped = std::ranges::any_of(match.waiting, [](const auto& entry) {
    return entry.second.lead >= 0 && entry.second.lead != entry.first;
  });
  const bool batch = match.doubles &&
                     (grouped || (!recovering && (match.waiting.size() > 1 || !match.seated.empty())));
  std::vector<int> batch_seats;
  if (batch)
  {
    for (const auto& [seat, arrival] : match.waiting)
    {
      batch_seats.push_back(seat);
      AddName(match, {seat, live_from, arrival.name, arrival.controls, arrival.queue});
    }
  }
  if (match.doubles && match.waiting.size() > 1)
  {
    match.keyframe->shared = true;
    match.shared_keyframes.push_back(*match.keyframe);
  }
  for (auto it = match.waiting.begin(); it != match.waiting.end();)
  {
    const int seat = it->first;
    const int keyframe_frame = match.keyframe->frame;
    if (!match.session)
    {
      if (keyframe_frame < match.log_base)
      {
        ++it;
        continue;
      }
      Orca::Net::Config config = SessionConfig();
      config.per_peer_inputs = match.doubles;
      config.local_seat = 0;
      config.co_seats = match.co_seats;
      config.authority_seat = 0;
      config.start_frame = next;
      config.start_pad = match.log.empty() ? Pad{} : match.log.back()[0];
      for (const int local : match.co_seats)
        config.start_pads[local] = match.log.empty() ? Pad{} : match.log.back()[local];
      std::array<Orca::Net::SeatPlan, Orca::Net::MAX_SEATS> plan{};
      plan[0] = {0, Orca::Net::NEVER, 0};
      for (const int local : match.co_seats)
        plan[local] = {0, Orca::Net::NEVER, 0};
      config.plan = plan;
      auto* transport = test_room ? test_room : Orca::Online::Transport();
      transport->Receive();
      match.session = std::make_unique<Orca::Net::Session>(config, *match.port,
                                                          *transport);
      match.stall_budget.Clear();
      SeedRoundTrips(match);
      match.session->SetLog(match.log_base, std::move(match.log));
      match.log.clear();
      ClearQueuedLocal(match);
      EndFresh(match, "a session starts");
      match.fast_spent = true;
      if (std::exchange(match.fast, false))
        Core::SetIsThrottlerTempDisabled(false);
      RefreshOwnValues(match, next + 1);
    }
    if (batch && !batch_seats.empty())
    {
      for (const auto& [lead, arrival] : match.waiting)
      {
        if (arrival.lead != lead)
          continue;
        std::vector<int> co;
        for (const auto& [member, member_arrival] : match.waiting)
          if (member != lead && member_arrival.lead == lead)
            co.push_back(member);
        if (!co.empty())
          match.session->SetGroup(lead, co);
      }
      match.session->AddPeerBatch(batch_seats, live_from, keyframe_frame);
      batch_seats.clear();
    }
    else if (!batch)
      match.session->AddPeer(seat, live_from, keyframe_frame);
    match.seated.insert(seat);
    if (it->second.doubles)
      match.doubles_seats.insert(seat);
    match.plugging.insert(seat);
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: port {} is {} from frame {}{}", Orca::Online::SeatPort(seat) + 1, it->second.name,
                   live_from, it->second.controls.empty() ? "" : ", with their own controls");
    if (!batch)
      AddName(match, {seat, live_from, it->second.name, it->second.controls, it->second.queue});
    match.session->RequireValues(seat, match.names_version);
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: {}", match.session->Describe());
    Orca::Net::KeyframeInfo offer = *match.keyframe;
    offer.names = AllNames(match);
    offer.names_version = match.names_version;
    if (it->second.lead < 0 || it->second.lead == seat)
    {
      if (test_room)
      {
        test_room->BroadcastNames(offer.names, offer.names_version);
        test_room->OfferKeyframe(seat, offer);
      }
      else
      {
        Orca::Online::BroadcastNames(offer.names, offer.names_version);
        Orca::Online::OfferKeyframe(seat, offer);
      }
    }
    NOTICE_LOG_FMT(ROLLBACK,
                   "Drop-in: port {} replays from keyframe frame {}, live from frame {}", Orca::Online::SeatPort(seat) + 1,
                   keyframe_frame, live_from);
    it = match.waiting.erase(it);
  }
  if (match.waiting.empty())
  {
    match.keyframe.reset();
    match.recovery_keyframe_from = -1;
    match.batch.Clear();
  }
}

void EndQueueForJoin(Match& match)
{
  const bool queue_or_search = QueueOrSearch();
  if (!JoinEndsQueue(InQueueRoom(), queue_or_search, match.queue_image != nullptr))
    return;
  NOTICE_LOG_FMT(ROLLBACK,
                 "Queue: joined {}'s friends room {}: this player's own queue is over{}{}",
                 match.host_name.empty() ? std::string("a friend") : match.host_name,
                 Orca::Online::Code(), queue_or_search ? " (orca menu cancel)" : "",
                 match.queue_image ? "; the game it kept for after a queue's room is dropped" :
                                     "");
  match.queue_image.reset();
  match.want_queue_image = false;
  match.ranked_fought = false;
  if (!queue_or_search)
    return;
  Orca::UX::Search::End();
  Orca::UX::QueueApp::End();
  Orca::Status::Menu("cancel");
}

void StartDownload(Match& match, const Orca::Net::KeyframeInfo& info)
{
  auto download = std::make_unique<Download>();
  download->info = info;
  Download* const raw = download.get();
  Orca::Net::KeyframeStore* const store = match.store.get();
  raw->thread = std::thread([raw, store] {
    try
    {
      const auto start = Clock::now();
      std::string error;
      std::optional<std::vector<u8>> blob;
      if (!raw->info.inline_blob.empty())
      {
        blob = raw->info.inline_blob;
        raw->percent = 100;
      }
      else
      {
        blob = store->Get(
            raw->info,
            [raw](u64 done, u64 total) {
              raw->percent = static_cast<int>(done * 100 / std::max<u64>(total, 1));
              return !raw->cancel.load();
            },
            &error);
      }
      raw->transfer_ms = MsSince(start);
      if (!blob)
      {
        raw->error = error;
        raw->refusal = Orca::Net::LastRefusal();
        raw->done = true;
        return;
      }
      const auto unpack_start = Clock::now();
      raw->ok = Orca::Net::OpenKeyframe(raw->info, std::move(*blob), &raw->frame, &raw->replay,
                                        &raw->error);
      raw->unpack_ms = MsSince(unpack_start);
    }
    catch (const std::exception&)
    {
      raw->error = "couldn't read the replay";
      raw->ok = false;
    }
    raw->done = true;
  });
  match.download = std::move(download);
}

enum class JoinPump
{
  Waiting,
  Ready,
  Failed,
};

JoinPump PumpJoin(Match& match, std::string* last_status)
{
  {
    for (const Orca::Net::PeerEvent& event : Orca::Online::TakePeerEvents())
    {
      if (event.kind == Orca::Net::PeerEvent::Kind::Arrived && event.host)
      {
        if (match.host_room_wait.until)
        {
          match.boot_time = Clock::now();
          NOTICE_LOG_FMT(ROLLBACK, "Drop-in: host back; resuming by keyframe in room {}",
                         Orca::Online::Code());
          Orca::Status::Event("room-back host ended fresh-drop-in");
          Orca::Status::State("joining 0");
        }
        match.host_here = true;
        match.host_seat = event.seat;
        match.host_name = event.name.empty() ? "your friend" : event.name;
        ShowJoinMessage(fmt::format("Joining {}'s game...", match.host_name),
                        static_cast<u32>(std::chrono::milliseconds(JOIN_LIMIT).count()));
      }
      else if (event.kind == Orca::Net::PeerEvent::Kind::Left && event.host)
      {
        if (match.host_room_wait.until && event.reason == "room-away-silent")
          continue;
        if (event.reason == "mismatch")
        {
          WARN_LOG_FMT(ROLLBACK, "Online rules: the host left room {}: its game wasn't set up for "
                       "this match (mismatch)", Orca::Online::Code());
          match.join_error_code = "mismatch";
          match.join_error = "That match couldn't start";
          return JoinPump::Failed;
        }
        match.join_error_code = "peer_left";
        match.join_error = "Your friend left before you could join";
        return JoinPump::Failed;
      }
      else if (event.kind == Orca::Net::PeerEvent::Kind::Hold && !match.download)
      {
        const bool fight = event.reason == "fight";
        if (event.holding != match.host_holding ||
            (event.holding && fight != match.host_hold_fight))
        {
          match.host_holding = event.holding;
          match.host_hold_fight = fight;
          match.host_hold_message.Reset();
          NOTICE_LOG_FMT(ROLLBACK, "Drop-in: the host {}",
                         event.holding ? (fight ? "is playing a match: the join waits" :
                                                 "is in a single-player mode: the join waits") :
                                         "is back in the menus: the join goes on");
          if (event.holding)
          {
            Orca::Status::State("friend-waiting");
          }
          else
          {
            Orca::Status::State("joining 0");
            match.last_percent = std::max(match.last_percent, 0);
            ShowJoinMessage(fmt::format("Joining {}'s game...", match.host_name.empty() ?
                                                                    "your friend" :
                                                                    match.host_name),
                            static_cast<u32>(std::chrono::milliseconds(JOIN_LIMIT).count()));
          }
        }
      }
      else if (event.kind == Orca::Net::PeerEvent::Kind::Names)
      {
        if (!match.later_names || event.keyframe.names_version > match.later_names->names_version)
          match.later_names = event.keyframe;
      }
      else if (event.kind == Orca::Net::PeerEvent::Kind::Keyframe && !match.download)
      {
        match.host_room_wait.Back();
        match.host_holding = false;
        match.offered_time = Clock::now();
        match.host_seat = event.seat;
        NOTICE_LOG_FMT(ROLLBACK, "Drop-in: the host offers keyframe {} (frame {}, {} bytes, {})",
                       event.keyframe.id, event.keyframe.frame, event.keyframe.size,
                       event.keyframe.inline_blob.empty() ? "from the store" : "inline");
        StartDownload(match, event.keyframe);
      }
    }
    if (Orca::Online::RoomEnded())
    {
      match.join_error_code.clear();
      match.join_error = fmt::format("Couldn't join: {}", Orca::Online::StatusLine());
      return JoinPump::Failed;
    }
    if (match.host_room_wait.Expired(Clock::now()))
    {
      NOTICE_LOG_FMT(ROLLBACK, "Drop-in: host back wait ended: room hold expired");
      Orca::Status::Event("room-away host ended hold-expired");
      match.join_error_code = "peer_left";
      match.join_error = "Your friend did not return to the room";
      return JoinPump::Failed;
    }
    if (std::string status = Orca::Online::StatusLine(); status != *last_status)
    {
      NOTICE_LOG_FMT(NETPLAY, "Orca: {}", status);
      *last_status = std::move(status);
    }
    if (match.download)
    {
      const int percent = match.download->percent / 2;
      if (percent > match.last_percent)
      {
        match.last_percent = percent;
        Orca::Status::State(fmt::format("joining {}", percent));
      }
      if (match.download->done)
        return JoinPump::Ready;
    }
    if (match.host_holding)
    {
      match.boot_time = Clock::now();
      if (match.host_hold_message.Due(match.boot_time, JOIN_LIMIT))
        ShowJoinMessage(
            fmt::format("{} is playing {}: you'll join when they're back in the menus",
                        match.host_name.empty() ? "Your friend" : match.host_name,
                        match.host_hold_fight ? "a match" : "a single-player mode"),
            static_cast<u32>(std::chrono::milliseconds(JOIN_LIMIT).count()));
    }
    if (!match.host_room_wait.until && Clock::now() - match.boot_time > JOIN_LIMIT)
    {
      match.join_error_code = "network";
      match.join_error = "Your friend's game never sent its state";
      return JoinPump::Failed;
    }
  }
  return JoinPump::Waiting;
}

void FailJoin(Core::System& system, Match& match)
{
  const std::string sentence =
      match.join_error.empty() ? std::string("Couldn't join your friend's game") : match.join_error;
  std::string code = match.join_error_code;
  if (code.empty())
    code = Orca::Online::RoomErrorCode();
  const bool automatic = std::exchange(match.reopen_join, false);
  if (automatic)
    Orca::Status::Event("playing");
  else
    ReportJoinFailure(code, sentence);
  match.join_error_code.clear();
  match.join_error.clear();
  if (automatic || Soft())
  {
    Core::DisplayMessage(sentence, 6000);
    BecomeSolo(match, "the join failed", "playing", match.loaded_time == Clock::time_point{});
    return;
  }
  StopEmulation(system, sentence);
  End("couldn't join");
}

enum class Load
{
  Done,
  Failed,
  Broken,
};

enum class Restore
{
  Done,
  Failed,
  Broken,
};
Restore RestoreLocal(Core::System& system, Match& match, MachineImage image,
                     const std::vector<Orca::Net::NandEntry>& nand_entries, int frame)
{
  IOS::HLE::FS::HostFileSystem* nand = system.IsWii() ? HostNand(system) : nullptr;
  if (!LocalNandReady(system.IsWii(), nand != nullptr, Core::WiiRootIsTemporary()))
    return Restore::Failed;
  if (nand)
    nand->CloseHostFiles();
  match.port.reset();
  match.port = std::make_unique<RingPort>(system, MAX_ROLLBACK);
  if (nand)
  {
    if (!Orca::Net::ReplaceNandTree(nand->HostRoot(), nand_entries))
      return Restore::Broken;
    nand->ReloadFst();
  }
  if (!match.port->LoadImage(std::move(image), frame))
    return Restore::Broken;
  Rollback::Harness::Restart();
  return Restore::Done;
}

bool CaptureOrigin(Core::System& system, Match& match, int frame)
{
  const auto start = Clock::now();
  IOS::HLE::FS::HostFileSystem* nand = system.IsWii() ? HostNand(system) : nullptr;
  if (!system.IsWii())
    match.origin_nand.clear();
  if (nand)
    nand->CloseHostFiles();
  const bool nand_ok = LocalNandReady(system.IsWii(), nand != nullptr,
                                      Core::WiiRootIsTemporary()) &&
                       (!system.IsWii() ||
                        Orca::Net::ReadNandTree(nand->HostRoot(), &match.origin_nand));
  if (nand)
    nand->ReopenHostFiles();
  if (!nand_ok || !SnapshotRing::Capture(system, &match.origin, true))
  {
    StopEmulation(system, "Couldn't prepare this game's local replay origin");
    End("replay origin");
    return false;
  }
  match.origin_frame = frame;
  match.origin_ticks = system.GetCoreTiming().GetTicks();
  match.origin_hash = RamChecksum(match.origin.mem1, match.origin.mem2);
  match.replay = {};
  match.replay.origin_hash = match.origin_hash;
  match.replay.first_frame = frame;
  if (match.origin_rule == OriginRule::Menu)
  {
    match.replay.frames.resize(1);
    match.replay.frames[0].seed = NewHistorySeed();
  }
  match.log.clear();
  match.log_base = frame;
  system.GetJitInterface().ClearSafe();
  u64 nand_bytes = 0;
  for (const Orca::Net::NandEntry& e : match.origin_nand)
    nand_bytes += e.data.size();
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: origin at frame {}: RAM {:016x}, NAND {} KB ({:.0f} ms)", frame,
                 match.origin_hash, nand_bytes / 1024, MsSince(start));
  NOTICE_LOG_FMT(ROLLBACK, "Orca origin: rule {} at frame {}: RAM {:016x}",
                 match.origin_rule == OriginRule::Declared ? "declared" :
                 match.origin_rule == OriginRule::Frame0 ? "frame0" : "menu",
                 frame, match.origin_hash);
  return true;
}

std::optional<int> FreshStart(Core::System& system, Match& match)
{
  const FreshKind kind = FreshKindForOrigin(match.origin_rule);
  const u8 exit = std::exchange(match.fresh_pending, std::nullopt).value_or(0);
  if (!OriginReady(match) || match.session || match.joining || match.replaying || match.download ||
      match.job || !match.port || !FreshExitAllowed(kind, exit))
  {
    WARN_LOG_FMT(ROLLBACK, "Drop-in: a fresh start was due at frame {}, but this game isn't solo "
                           "with an origin; skipped",
                 match.running + 1);
    return std::nullopt;
  }
  const auto start = Clock::now();
  const int from = match.running + 1;
  const int frame = match.origin_frame;
  switch (RestoreLocal(system, match, match.origin, match.origin_nand, frame))
  {
  case Restore::Done:
    break;
  case Restore::Failed:
    ERROR_LOG_FMT(ROLLBACK, "Drop-in: no session NAND for a fresh start at frame {}", from);
    return std::nullopt;
  case Restore::Broken:
    StopEmulation(system, "Couldn't restore this game's local boot");
    Orca::Status::Error("internal", "Couldn't restore this game's local boot");
    End("fresh start");
    return std::nullopt;
  }
  system.GetJitInterface().ClearSafe();
  if (match.keyframe)
    DeleteStored(match, *match.keyframe);
  match.keyframe.reset();
  match.keyframe_wanted = !match.waiting.empty();
  match.header_waited = 0;
  match.running = frame - 1;
  Orca::UX::Game().Read().ResetFightHold();
  match.fight_let_in = false;
  match.replay = FreshReplayStart(kind, frame, match.origin_hash, exit,
                                  kind == FreshKind::Menu ? NewHistorySeed() : 0);
  match.log.clear();
  match.log_base = frame;
  ClearQueuedLocal(match);
  match.port_names.clear();
  RefreshOwnValues(match, frame);
  match.stats_frame = -1;
  Match::Fresh fresh;
  fresh.kind = kind;
  fresh.exit = exit;
  fresh.queue = InQueueRoom();
  fresh.room = Orca::Online::Code();
  fresh.started = start;
  match.fresh = fresh;
  if (fresh.queue)
  {
    match.fresh_queue_armed = false;
    match.fresh_room = fresh.room;
  }
  Orca::UX::Game().Css()->DisarmOwnPick();
  match.fast_spent = true;
  if (std::exchange(match.fast, false))
    Core::SetIsThrottlerTempDisabled(false);
  match.port->SetCatchingUp(true);
  Orca::Events::NoteResync();
  Orca::UX::QueueApp::ResetTimeouts();
  Orca::UX::Game().Results().Reset();
  Orca::UX::Game().Header().MemoryReplaced();
  if (kind == FreshKind::Reset && fresh.queue)
    Orca::UX::ShowRestartForMatch();
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: fresh start at frame {}: back to the origin (frame {}) in {:.0f} "
                           "ms, leaving the main menu with exit code {}{}",
                 from, frame, MsSince(start), exit,
                 fresh.queue ? fmt::format(" for {} room {}", Orca::Online::RoomQueue(), fresh.room) :
                               std::string());
  return frame;
}

Load LoadKeyframe(Core::System& system, Match& match, int* frame_out)
{
  std::unique_ptr<Download> download = std::move(match.download);
  if (download->thread.joinable())
    download->thread.join();
  if (!download->ok)
  {
    match.join_error_code = download->refusal == "signed_out" ? "signed_out" : "network";
    match.join_error = "Couldn't download your friend's replay";
    return Load::Failed;
  }
  if (!OriginReady(match) || match.origin.state.empty())
  {
    match.join_error_code = "internal";
    match.join_error = "This game has no local origin to rebuild your friend's game from";
    return Load::Failed;
  }
  if (download->replay.origin_hash != match.origin_hash ||
      download->replay.first_frame != match.origin_frame)
  {
    WARN_LOG_FMT(ROLLBACK, "Drop-in: the host's origin is frame {} ({:016x}), this game's frame {} "
                           "({:016x})",
                 download->replay.first_frame, download->replay.origin_hash, match.origin_frame,
                 match.origin_hash);
    match.join_error_code = "room_mismatch";
    match.join_error = "Your friend's game data differs from yours";
    return Load::Failed;
  }
  if (!Orca::Net::HeadersFitRuleset(download->replay,
                                    static_cast<u8>(Orca::UX::Game().ProfileRuleset())))
  {
    WARN_LOG_FMT(ROLLBACK, "Drop-in: the host's replay sets another game's rules: refused");
    match.join_error_code = "room_mismatch";
    match.join_error = "Your friend's game data differs from yours";
    return Load::Failed;
  }
  IOS::HLE::FS::HostFileSystem* nand = system.IsWii() ? HostNand(system) : nullptr;
  if (!LocalNandReady(system.IsWii(), nand != nullptr, Core::WiiRootIsTemporary()))
  {
    match.join_error_code = "internal";
    match.join_error = "Couldn't restore this game's local boot";
    return Load::Failed;
  }
  auto before = std::make_unique<Match::QueueImage>();
  before->frame = match.running + 1;
  before->local_seat = match.local_seat;
  before->co_seats = match.co_seats;
  before->loaded_time = match.loaded_time;
  before->replay = match.replay;
  if (!SnapshotRing::Capture(system, &before->image, false))
    return Load::Failed;
  if (nand)
  {
    nand->CloseHostFiles();
    if (!Orca::Net::ReadNandTree(nand->HostRoot(), &before->nand))
    {
      nand->ReopenHostFiles();
      return Load::Failed;
    }
  }
  match.before_join = std::move(before);
  if (RestoreLocal(system, match, match.origin, match.origin_nand, match.origin_frame) !=
      Restore::Done)
  {
    StopEmulation(system, "Couldn't restore this game's local boot");
    End("replay origin");
    return Load::Broken;
  }
  system.GetJitInterface().ClearSafe();
  match.fresh.reset();
  Orca::UX::Game().Css()->DisarmOwnPick();
  match.replay = std::move(download->replay);
  match.running = match.origin_frame - 1;
  match.join_in_play = false;
  match.loaded_time = Clock::now();
  match.replay_heartbeat = {};
  match.replaying = std::move(download);
  match.port->SetCatchingUp(true);
  Orca::UX::QueueApp::ResetTimeouts();
  Orca::UX::Game().Results().Reset();
  Orca::Events::NoteResync();
  Orca::UX::Game().Header().MemoryReplaced();
  Orca::UX::Search::JoinOver();
  *frame_out = match.origin_frame;
  return Load::Done;
}

void FinishReplay(Match& match)
{
  const Download& download = *match.replaying;
  const int frame = download.frame;
  if (!download.info.shared)
    DeleteStored(match, download.info);
  match.local_seat = Orca::Online::Seat();
  match.co_seats = Orca::Online::CoSeats();
  Orca::UX::SetLocalPadCount(match.co_seats.empty() ? 1 : 2);
  if (match.later_names && match.later_names->names_version > download.info.names_version)
    SetNames(match, *match.later_names);
  else
    SetNames(match, download.info);
  match.later_names.reset();
  match.log.clear();
  match.log_base = frame;
  ClearQueuedLocal(match);
  Orca::Net::Config config = SessionConfig();
  config.local_seat = match.local_seat;
  config.co_seats = match.co_seats;
  config.authority_seat = match.host_seat;
  config.start_frame = frame;
  std::array<Orca::Net::SeatPlan, Orca::Net::MAX_SEATS> plan{};
  plan[match.host_seat] = {0, Orca::Net::NEVER, 0};
  config.plan = plan;
  Orca::Online::SetSessionActive(true);
  match.session =
      std::make_unique<Orca::Net::Session>(config, *match.port, *Orca::Online::Transport());
  match.stall_budget.Clear();
  match.session->SetValuesHeld(match.names_version);
  SeedRoundTrips(match);
  match.running = frame - 1;
  match.stats_frame = -1;
  EndQueueForJoin(match);
  Orca::UX::Game().Header().ExpectHeader(QueueMode(Orca::Online::RoomQueue()), Orca::Online::Code(),
                                RoomFlags(), Orca::Online::KnownSetGames());
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: rebuilt replay frame {} locally in {:.0f} ms", frame,
                 MsSince(match.loaded_time));
  match.replaying.reset();
  match.before_join.reset();
}

bool RestoreBeforeJoin(Core::System& system, Match& match)
{
  auto before = std::move(match.before_join);
  if (!before || RestoreLocal(system, match, std::move(before->image), before->nand,
                              before->frame) != Restore::Done)
  {
    return false;
  }
  system.GetJitInterface().ClearSafe();
  match.running = before->frame - 1;
  match.local_seat = before->local_seat;
  match.co_seats = std::move(before->co_seats);
  Orca::UX::SetLocalPadCount(match.co_seats.empty() ? 1 : 2);
  match.loaded_time = before->loaded_time;
  match.replay = std::move(before->replay);
  match.replaying.reset();
  Orca::Events::NoteResync();
  Orca::UX::QueueApp::ResetTimeouts();
  Orca::UX::Game().Results().Reset();
  Orca::UX::Game().Header().MemoryReplaced();
  return true;
}

enum class ReplayStep { Running, Done, Failed };

ReplayStep AdvanceReplay(Core::System& system, Match& match, const FrameHook& on_frame)
{
  const bool leave = Orca::Status::Cap("leave") && Orca::Online::TakeLeaveRequest();
  const auto join = Orca::Status::Cap("join") ? Orca::Online::TakeJoinRequest() : std::nullopt;
  if (leave || (join && *join != Orca::Online::Code()))
  {
    if (!RestoreBeforeJoin(system, match))
    {
      StopEmulation(system, "Couldn't return to your own game");
      End("replay cancelled");
    }
    else if (join)
      JoinInPlay(match, *join);
    else
      Leave(match);
    return ReplayStep::Failed;
  }
  const int frame = match.running + 1;
  const int target = match.replaying->frame;
  if (MsSince(match.loaded_time) > 180000 || Orca::Online::RoomEnded() ||
      Orca::Online::HostLeftPending())
  {
    if (!RestoreBeforeJoin(system, match))
    {
      StopEmulation(system, "Couldn't return to your own game");
      End("replay failed");
      return ReplayStep::Failed;
    }
    if (Orca::Online::RoomEnded())
    {
      match.join_error_code.clear();
      match.join_error = fmt::format("Couldn't join: {}", Orca::Online::StatusLine());
    }
    else if (Orca::Online::HostLeftPending())
    {
      match.join_error_code = "peer_left";
      match.join_error = "Your friend left before you could join";
    }
    else
    {
      match.join_error_code = "network";
      match.join_error = "Couldn't finish rebuilding your friend's game";
    }
    FailJoin(system, match);
    return ReplayStep::Failed;
  }
  if (Clock::now() - match.replay_heartbeat >= std::chrono::seconds(1))
  {
    Orca::Net::Packet heartbeat;
    heartbeat.seat = Orca::Online::Seat();
    heartbeat.first_frame = target;
    heartbeat.current_frame = target;
    heartbeat.history_ack = target - 1;
    if (auto* transport = Orca::Online::Transport())
      transport->Send(heartbeat);
    match.replay_heartbeat = Clock::now();
  }
  if (const Orca::Profile* profile = Orca::ActiveProfile();
      profile && profile->jit_clear_frame && frame == static_cast<int>(*profile->jit_clear_frame))
  {
    system.GetJitInterface().ClearSafe();
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: JIT cleared at replay frame {}", frame);
  }
  Orca::Net::ReplayFrame* const at = frame < target ? match.replay.At(frame) : nullptr;
  if (frame < target && !at)
  {
    ERROR_LOG_FMT(ROLLBACK, "Drop-in: the replay has no frame {} (frames {}-{})", frame,
                  match.replay.first_frame, target);
    if (!RestoreBeforeJoin(system, match))
    {
      StopEmulation(system, "Couldn't return to your own game");
      End("replay gap");
      return ReplayStep::Failed;
    }
    match.join_error_code = "room_mismatch";
    match.join_error = "Your friend's replay didn't reproduce the same game";
    FailJoin(system, match);
    return ReplayStep::Failed;
  }
  Orca::Net::ReplayFrame& record = at ? *at : match.replay.boundary;
  {
    Orca::Net::ReplayScope scope(&record, true, true);
    ReplayFrameHook(system, match, frame, true, *record.ports, false, on_frame);
  }
  if (frame < target)
  {
    match.port->SetPads(frame, record.pads);
    match.running = frame;
    const int first = match.replay.first_frame;
    if (frame % 60 == 0)
    {
      Orca::Status::State(fmt::format(
          "joining {}", 50 + (target > first ? (frame - first) * 40 / (target - first) : 40)));
    }
    return ReplayStep::Running;
  }
  auto& memory = system.GetMemory();
  const u64 hash = RamChecksum({memory.GetRAM(), memory.GetRamSize()},
                              {memory.GetEXRAM(), memory.GetEXRAM() ? memory.GetExRamSize() : 0});
  if (hash != match.replay.target_hash)
  {
    if (!RestoreBeforeJoin(system, match))
    {
      StopEmulation(system, "Couldn't return to your own game");
      End("replay mismatch");
      return ReplayStep::Failed;
    }
    match.join_error_code = "room_mismatch";
    match.join_error = "Your friend's replay didn't reproduce the same game";
    FailJoin(system, match);
    return ReplayStep::Failed;
  }
  match.replay.frames.push_back(match.replay.boundary);
  FinishReplay(match);
  return ReplayStep::Done;
}

std::optional<int> StepReplayPlayer(Core::System& system, Match& match, const FrameHook& on_frame)
{
  const auto after_restore = [&system] {
    system.GetJitInterface().ClearSafe();
    Orca::Events::NoteResync();
    Orca::UX::QueueApp::ResetTimeouts();
    Orca::UX::Game().Results().Reset();
    Orca::UX::Game().Header().MemoryReplaced();
  };
  ReplayStream::PlayerHost host;
  host.restore_origin = [&] {
    if (RestoreLocal(system, match, match.origin, match.origin_nand, match.origin_frame) !=
        Restore::Done)
      return false;
    after_restore();
    return true;
  };
  host.restore = [&](MachineImage image, const std::vector<Orca::Net::NandEntry>& nand, int frame) {
    if (RestoreLocal(system, match, std::move(image), nand, frame) != Restore::Done)
      return false;
    after_restore();
    return true;
  };
  host.read_nand = [&system](std::vector<Orca::Net::NandEntry>* nand) {
    IOS::HLE::FS::HostFileSystem* fs = HostNand(system);
    if (!fs)
      return false;
    fs->CloseHostFiles();
    const bool ok = Orca::Net::ReadNandTree(fs->HostRoot(), nand);
    fs->ReopenHostFiles();
    return ok;
  };
  host.set_catching_up = [&match](bool catching) { match.port->SetCatchingUp(catching); };
  host.set_pads = [&match](int frame, const Orca::Net::Pads& pads) {
    match.port->SetPads(frame, pads);
  };
  host.stopping = [&system] { return Stopping(system); };
  host.stop = [&system](const std::string& message) { StopEmulation(system, message); };
  const std::optional<int> rewound = match.replay_play->Step(
      system, host, &match.running, match.origin_hash, match.origin_frame, on_frame);
  if (match.replay_play->Over())
  {
    match.replay_play.reset();
    End("replay over");
  }
  return rewound;
}

std::optional<int> JoinHost(Core::System& system, Match& match)
{
  JoinPump pump = JoinPump::Waiting;
  while (!Stopping(system))
  {
    if (Orca::Status::Cap("leave") && Orca::Online::TakeLeaveRequest())
    {
      NOTICE_LOG_FMT(ROLLBACK, "Drop-in: left before joining, at the app's request");
      Leave(match);
      return std::nullopt;
    }
    if (Orca::Status::Cap("join"))
    {
      if (const auto code = Orca::Online::TakeJoinRequest(); code && *code != Orca::Online::Code())
      {
        NOTICE_LOG_FMT(ROLLBACK, "Drop-in: joining room {} instead", *code);
        OSD::DiscardTypedMessage(OSD::MessageType::OrcaJoin);
        ResetDropIn(match);
        Orca::Online::ShutdownInBackground();
        Orca::Online::StartRoom(*code, true, !QueueOrSearch());
        match.boot_time = Clock::now();
        match.offered_time = match.boot_time;
        match.host_here = false;
        match.host_holding = false;
        match.last_percent = -1;
        match.join_status.clear();
        match.later_names.reset();
        Orca::Status::State("joining 0");
      }
    }
    pump = PumpJoin(match, &match.join_status);
    if (pump != JoinPump::Waiting)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (pump == JoinPump::Waiting)
    return std::nullopt;
  if (pump == JoinPump::Failed)
  {
    FailJoin(system, match);
    return std::nullopt;
  }
  int frame = -1;
  switch (LoadKeyframe(system, match, &frame))
  {
  case Load::Done:
    return frame;
  case Load::Failed:
    FailJoin(system, match);
    return std::nullopt;
  case Load::Broken:
    return std::nullopt;
  }
  return std::nullopt;
}

void Leave(Match& match)
{
  if (std::exchange(match.opponent_no_show, false) &&
      VoidFirst({.queue_room = Orca::Online::RoomQueue() != "private",
                 .match_live = Orca::Online::MatchLive(),
                 .set_over = Orca::Online::SetOver(),
                 .room_ended = Orca::Online::RoomEnded(),
                 .from_peer = true}))
  {
    NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: leaving after the opponent's no-show: reporting the "
                             "match void first");
    Orca::Online::ReportPeerFault();
  }
  if (Orca::Online::RoomQueue() == "ranked" && Orca::Online::MatchLive())
    Orca::UX::SetEnd::Current().SelfLeft(false, Orca::UX::SetEnd::NowMs());
  Orca::UX::Search::End();
  match.linger_until.reset();
  if (match.joining && match.session && match.session->CatchingUp())
  {
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: left while catching up, frame {}", match.running);
    BecomeSolo(match, "left while catching up", nullptr);
    Orca::Status::Event("left");
    return;
  }
  if (match.joining && match.session)
  {
    if (!match.leave_requested)
    {
      match.leave_requested = true;
      NOTICE_LOG_FMT(ROLLBACK, "Drop-in: leaving at the app's request, frame {}", match.running);
      match.session->RequestLeave();
    }
    return;
  }
  if (match.joining)
  {
    BecomeSolo(match, "left before joining", nullptr, match.loaded_time == Clock::time_point{});
  }
  else
  {
    if (match.session)
      GoSolo(match, "left", nullptr);
    ResetDropIn(match);
    ReopenRoom(match);
  }
  Orca::Status::Event("left");
}

void JoinInPlay(Match& match, const std::string& code, bool keep_room)
{
  if (code == Orca::Online::Code() && match.joining)
    return;
  if (match.session)
    GoSolo(match, "joining another game", nullptr);
  ResetDropIn(match);
  ForgetRoom(match);
  match.linger_until.reset();
  const bool queued = QueueOrSearch();
  Orca::UX::Search::Matched(false);
  if (queued)
    Orca::UX::Search::JoinStarted();
  if (!keep_room)
  {
    Orca::Online::ShutdownInBackground();
    Orca::Online::StartRoom(code, true, !queued);
  }
  match.reopen_join = false;
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: joining room {} during play, frame {}", code, match.running + 1);
  match.joining = true;
  match.join_in_play = true;
  match.later_names.reset();
  match.boot_time = Clock::now();
  match.offered_time = match.boot_time;
  match.host_here = false;
  match.host_holding = false;
  match.joined = false;
  match.leave_requested = false;
  match.last_percent = -1;
  match.join_status.clear();
  Orca::Status::State("joining 0");
}

void HostInPlay(Match& match, const std::string& code)
{
  const bool alone = match.port && !match.session && !match.joining && !match.download &&
                     !match.job && !match.keyframe_wanted && match.waiting.empty() &&
                     match.plugging.empty() && match.seated.empty() && !match.port->CatchingUp() &&
                     !Orca::Online::DropInPending();
  if (!alone)
  {
    WARN_LOG_FMT(ROLLBACK, "Matchmaking: can't host room {}: this game isn't alone", code);
    Orca::Status::Line("unsupported host");
    return;
  }
  Reseat(match);
  ResetDropIn(match);
  ForgetRoom(match);
  match.linger_until.reset();
  Orca::Online::ShutdownInBackground();
  Orca::Online::StartRoom(code, false, false);
  Orca::UX::Search::Matched(true);
  match.fresh_queue_armed = code != match.fresh_room;
  NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: hosting room {} from frame {}", code, match.running + 1);
}

bool MaybeRestoreQueueImage(Core::System& system, Match& match);

void TakeCommands(Core::System& system, Match& match)
{
  if (!Orca::Status::Cap("join") && !Orca::Status::Cap("leave") && !Orca::Status::Cap("host"))
    return;
  if (CanonicalBoot(match) || InTail(match))
    return;
  if (match.session &&
      (match.session->Resimulating() || match.session->CurrentFrame() != match.running + 1))
  {
    return;
  }
  if (match.joining && !match.join_in_play && !match.session)
    return;
  if (Orca::Status::Cap("leave") && Orca::Online::TakeLeaveRequest())
    Leave(match);
  const bool restored = MaybeRestoreQueueImage(system, match);
  if (match.finished || !match.port)
    return;
  const bool queue_css =
      Orca::Status::Cap("queue2") && (restored || Orca::UX::QueueApp::OnSoloCss());
  if (Orca::Status::Cap("join"))
  {
    if (const auto code = Orca::Online::TakeJoinRequest())
    {
      match.want_queue_image = queue_css && !match.session && !match.joining;
      JoinInPlay(match, *code);
    }
  }
  if (Orca::Status::Cap("host"))
  {
    if (const auto code = Orca::Online::TakeHostRequest())
    {
      match.want_queue_image = queue_css && !match.session && !match.joining;
      HostInPlay(match, *code);
    }
  }
}

void CaptureQueueImage(Core::System& system, Match& match)
{
  match.want_queue_image = false;
  match.ranked_fought = false;
  auto qi = std::make_unique<Match::QueueImage>();
  qi->frame = match.running + 1;
  qi->replay = match.replay;
  const auto start = Clock::now();
  if (!SnapshotRing::Capture(system, &qi->image, false))
  {
    ERROR_LOG_FMT(ROLLBACK, "Queue: capturing this player's own game failed");
    return;
  }
  if (IOS::HLE::FS::HostFileSystem* nand = HostNand(system))
  {
    nand->CloseHostFiles();
    const bool ok = Orca::Net::ReadNandTree(nand->HostRoot(), &qi->nand);
    nand->ReopenHostFiles();
    if (!ok)
    {
      ERROR_LOG_FMT(ROLLBACK, "Queue: reading the NAND for this player's own game failed");
      return;
    }
  }
  qi->taken = Clock::now();
  u64 nand_bytes = 0;
  for (const Orca::Net::NandEntry& e : qi->nand)
    nand_bytes += e.data.size();
  NOTICE_LOG_FMT(ROLLBACK, "Queue: this player's own game kept at frame {} ({:.0f} ms, NAND {} "
                 "KB)",
                 match.running + 1, MsSince(start), nand_bytes / 1024);
  match.queue_image = std::move(qi);
}

bool InQueueRoom()
{
  return !Orca::Online::RoomEnded() && QueueMode(Orca::Online::RoomQueue()) !=
                                           Orca::Engine::Mode::None;
}

bool MaybeRestoreQueueImage(Core::System& system, Match& match)
{
  if (!match.queue_image)
    return false;
  Match::QueueImage& qi = *match.queue_image;
  if (!qi.armed && InQueueRoom() &&
      ((match.local_seat == 0 && !match.joining) || (match.joining && match.session)))
  {
    qi.armed = true;
    NOTICE_LOG_FMT(ROLLBACK, "Queue: in room {} ({})", Orca::Online::Code(),
                   Orca::Online::RoomQueue());
  }
  if (!Orca::UX::QueueApp::Active())
  {
    match.queue_image.reset();
    return false;
  }
  if (!qi.armed || match.session || match.joining || match.download || match.job ||
      match.linger_until || !match.waiting.empty() || !match.plugging.empty() || InQueueRoom() ||
      !match.port || match.port->CatchingUp())
  {
    return false;
  }
  std::unique_ptr<Match::QueueImage> image = std::move(match.queue_image);
  const auto start = Clock::now();
  const int frame = image->frame;
  switch (RestoreLocal(system, match, std::move(image->image), image->nand, frame))
  {
  case Restore::Done:
    break;
  case Restore::Failed:
    ERROR_LOG_FMT(ROLLBACK, "Queue: no session NAND to put this player's own game back into");
    return false;
  case Restore::Broken:
    StopEmulation(system, "Couldn't go back to your own game");
    Orca::Status::Error("internal", "Couldn't go back to your own game");
    End("queue image");
    return false;
  }
  match.fresh.reset();
  Orca::UX::Game().Css()->DisarmOwnPick();
  Orca::Events::NoteResync();
  Orca::UX::Game().Header().MemoryReplaced();
  match.log.clear();
  match.log_base = frame;
  match.running = frame - 1;
  match.replay = std::move(image->replay);
  ClearQueuedLocal(match);
  match.keyframe.reset();
  Reseat(match);
  if (match.tell_no_room)
  {
    match.tell_no_room = false;
    match.reopen_pending = Clock::now() + REOPEN_DELAY;
  }
  const bool mine = Orca::UX::QueueApp::TakeTimeoutWasMine();
  const bool fought = std::exchange(match.ranked_fought, false);
  const char* why = mine ? "the ready timer ran out on us" : fought ? "a ranked set was played" : "";
  Orca::UX::QueueApp::AfterRestore(!mine && !fought, why);
  NOTICE_LOG_FMT(ROLLBACK, "Queue: back on this player's own character select at frame {} in {:.0f} "
                 "ms ({:.1f} s after the room's game was kept){}",
                 frame, MsSince(start),
                 std::chrono::duration<double>(start - image->taken).count(),
                 *why ? fmt::format("; not ready ({})", why) : std::string());
  return true;
}

void TakeLobbyPickNow(Match& match)
{
  auto pick = Orca::UX::Game().Read().TakeLobbyPick();
  if (pick)
    match.lobby_pick.reset();
  else if (match.lobby_pick)
  {
    pick = std::exchange(match.lobby_pick, std::nullopt);
    if (!Orca::UX::Game().Read().PickStands(*pick))
    {
      NOTICE_LOG_FMT(ROLLBACK, "Online menu: the {} pick that waited for a boundary is stale by "
                               "frame {} (the game went on from it): dropped",
                     Orca::UX::MenuEventText(*pick, true), match.running + 1);
      return;
    }
  }
  if (!pick)
    return;
  const char* const queue = *pick == Orca::UX::OnlinePick::Ranked ? "ranked" : "casual";
  if (match.session &&
      (match.session->Resimulating() || match.session->CurrentFrame() != match.running + 1))
  {
    NOTICE_LOG_FMT(ROLLBACK, "Online menu: {} picked mid re-run, frame {}: at the next boundary",
                   queue, match.running + 1);
    match.lobby_pick = pick;
    return;
  }
  LobbyPickInputs in;
  in.joining = match.joining;
  in.local_seat = match.local_seat;
  in.queue_room = InQueueRoom();
  in.host_cap = Orca::Status::Cap("host");
  in.pick_cap = Orca::UX::PickSearchable(*pick);
  in.session = match.session != nullptr;
  in.session_idle = match.session && match.session->Idle();
  in.drop_in_friends =
      !match.waiting.empty() || !match.plugging.empty() || !match.seated.empty();
  in.arrival_pending = Orca::Online::ArrivalPending();
  in.keyframe_kept = match.keyframe || match.job || match.keyframe_wanted;
  const LobbyPickPlan plan = DecideLobbyPick(in);
  match.kept_pick.reset();
  match.friends_pick.reset();
  if (plan.drop_keyframe)
    ResetDropIn(match);
  switch (plan.step)
  {
  case LobbyPickStep::Ignore:
    NOTICE_LOG_FMT(ROLLBACK, "Online menu: {} picked in a game that isn't this player's alone "
                             "({}): nothing to do here",
                   queue, match.joining ? "its host's" : "with friends");
    return;
  case LobbyPickStep::Announce:
    if (!plan.arm)
    {
      NOTICE_LOG_FMT(ROLLBACK, "Online menu: {} picked with friends in room {}, which the page "
                               "can't search now (no pick-{}): staying with them, frame {}",
                     queue, Orca::Online::Code(), queue, match.running + 1);
      match.kept_pick = pick;
    }
    break;
  case LobbyPickStep::Leave:
    NOTICE_LOG_FMT(ROLLBACK, "Online menu: {} picked with friends in room {}: leaving it for the "
                             "queue, frame {}{}",
                   queue, Orca::Online::Code(), match.running + 1,
                   in.arrival_pending ? " (a friend arriving)" : "");
    match.linger_until.reset();
    if (match.session)
      GoSolo(match, "left for the queue", nullptr);
    ResetDropIn(match);
    ReopenRoom(match);
    Core::DisplayMessage(*pick == Orca::UX::OnlinePick::Ranked ? "Left the lobby for Ranked" :
                                                                 "Left the lobby for Casual",
                         4000);
    Orca::Status::Event("left lobby");
    break;
  }
  NOTICE_LOG_FMT(ROLLBACK, "Online menu: orca menu {}{}",
                 Orca::UX::MenuEventText(*pick, Orca::Status::Cap("host")),
                 plan.arm ? "" : " (unarmed)");
  Orca::UX::AnnounceOnlinePick(*pick, plan.arm);
}

void ComeHome(Match& match)
{
  if (match.local_seat == 0 || match.joining || match.session)
    return;
  HomeInputs in;
  in.local_seat = match.local_seat;
  in.joining = match.joining;
  in.session = match.session != nullptr;
  in.room_up = !Orca::Online::RoomEnded();
  in.solo_idle = SoloIdle(match);
  in.drop_in_pending = Orca::Online::DropInPending();
  in.on_menus = Orca::UX::Game().Read().OnTheMenus();
  in.queue_image = match.queue_image != nullptr;
  in.lingering = match.linger_until.has_value();
  if (!MayComeHome(in))
    return;
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: back to a game of its own on the menus (frame {}): a room of "
                           "its own opens",
                 match.running + 1);
  Reseat(match);
  ReopenRoom(match);
  const std::optional<Orca::UX::OnlinePick> pick = Orca::UX::Game().Read().CssPick();
  if (AnnounceHomePick(pick.has_value(), Orca::Status::Cap("host"),
                       Orca::UX::QueueApp::Active() ||
                           Orca::UX::Search::Current() != Orca::UX::Search::State::None))
  {
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: home on the {} character select: orca menu {}",
                   *pick == Orca::UX::OnlinePick::Ranked ? "ranked" : "casual",
                   Orca::UX::MenuEventText(*pick, true));
    Orca::UX::AnnounceOnlinePick(*pick);
  }
}

void ArmKeptPick(Match& match)
{
  if (!match.kept_pick || (match.session && match.session->Resimulating()))
    return;
  KeptPickInputs in;
  in.stands = Orca::UX::Game().Read().PickStands(*match.kept_pick);
  in.alone = match.local_seat == 0 && !match.joining && match.seated.empty() &&
             AloneAt(match.running + 1, SoloIdle(match), Orca::Online::DropInPending(),
                     match.keyframe ? std::optional(match.keyframe->frame) : std::nullopt);
  in.queue_or_search = Orca::UX::QueueApp::Active() ||
                       Orca::UX::Search::Current() != Orca::UX::Search::State::None;
  const Orca::UX::OnlinePick pick = *match.kept_pick;
  const char* const queue = pick == Orca::UX::OnlinePick::Ranked ? "ranked" : "casual";
  switch (DecideKeptPick(in))
  {
  case KeptPickStep::Wait:
    return;
  case KeptPickStep::Drop:
    match.kept_pick.reset();
    NOTICE_LOG_FMT(ROLLBACK, "Online menu: the {} pick kept with friends is over by frame {}: "
                             "dropped",
                   queue, match.running + 1);
    return;
  case KeptPickStep::Arm:
    match.kept_pick.reset();
    NOTICE_LOG_FMT(ROLLBACK, "Online menu: the {} pick kept with friends, alone now (frame {}): "
                             "orca menu {}",
                   queue, match.running + 1,
                   Orca::UX::MenuEventText(pick, Orca::Status::Cap("host")));
    Orca::UX::AnnounceOnlinePick(pick);
    return;
  }
}

void RearmFriendsPick(Match& match)
{
  if (!match.friends_pick || (match.session && match.session->Resimulating()))
    return;
  if (!match.seated.empty())
    match.friends_pick_played = true;
  FriendsPickInputs in;
  in.played = match.friends_pick_played;
  in.alone = match.local_seat == 0 && !match.joining && match.seated.empty() &&
             AloneAt(match.running + 1, SoloIdle(match), Orca::Online::DropInPending(),
                     match.keyframe ? std::optional(match.keyframe->frame) : std::nullopt);
  in.on_pick_select = Orca::UX::Game().Read().CssPick() == match.friends_pick;
  in.host_cap = Orca::Status::Cap("host") && Orca::UX::PickSearchable(*match.friends_pick);
  in.queue_or_search = QueueOrSearch();
  in.elsewhere = match.joining || match.local_seat != 0;
  const Orca::UX::OnlinePick pick = *match.friends_pick;
  const char* const queue = pick == Orca::UX::OnlinePick::Ranked ? "ranked" : "casual";
  switch (DecideFriendsPick(in))
  {
  case FriendsPickStep::Wait:
    return;
  case FriendsPickStep::Drop:
    match.friends_pick.reset();
    return;
  case FriendsPickStep::Arm:
    match.friends_pick.reset();
    NOTICE_LOG_FMT(ROLLBACK, "Queue: the friends are gone and this game is alone again on the {} "
                             "character select (frame {}): orca menu {}",
                   queue, match.running + 1,
                   Orca::UX::MenuEventText(pick, Orca::Status::Cap("host")));
    Orca::UX::AnnounceOnlinePick(pick);
    return;
  }
}

void TellNoRoom(Match& match)
{
  if (!match.tell_no_room || Orca::Online::RoomsLeaving())
    return;
  match.tell_no_room = false;
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: no room of its own on port {}", Orca::Online::SeatPort(match.local_seat) + 1);
  Orca::Status::State("no-room");
}

void LeaveOnMismatch(Match& match)
{
  if (match.session && match.session->Resimulating())
    return;
  const auto why = Orca::UX::Game().Header().TakeMismatch();
  if (!why)
    return;
  WARN_LOG_FMT(ROLLBACK, "Online rules: leaving room {}: {}", Orca::Online::Code(), *why);
  Orca::Online::SetLeaveReason(*why);
  Orca::Status::Report(*why, "That match couldn't start");
  Leave(match);
}

void AddName(Match& match, Orca::Net::KeyframeInfo::Name name)
{
  auto& history = match.port_names[name.seat];
  if (!history.empty() && history.back().from >= name.from)
    history.pop_back();
  history.push_back(std::move(name));
  if (history.size() > 4)
    history.erase(history.begin());
  ++match.names_version;
}

std::vector<Orca::Net::KeyframeInfo::Name> AllNames(const Match& match)
{
  std::vector<Orca::Net::KeyframeInfo::Name> all;
  for (const auto& [seat, history] : match.port_names)
    all.insert(all.end(), history.begin(), history.end());
  return all;
}

void SetNames(Match& match, const Orca::Net::KeyframeInfo& from_host)
{
  match.port_names.clear();
  std::vector<Orca::Net::KeyframeInfo::Name> sorted = from_host.names;
  std::stable_sort(sorted.begin(), sorted.end(),
                   [](const auto& a, const auto& b) { return a.from < b.from; });
  for (auto& name : sorted)
  {
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: port {} is {} from frame {}{}", Orca::Online::SeatPort(name.seat) + 1, name.name,
                   name.from, name.controls.empty() ? "" : ", with their own controls");
    match.port_names[name.seat].push_back(std::move(name));
  }
  match.names_version = from_host.names_version;
  if (match.session)
    match.session->SetValuesHeld(match.names_version);
}

const Orca::Net::KeyframeInfo::Name* EntryAt(const Match& match, int seat, int frame)
{
  const auto it = match.port_names.find(seat);
  if (it == match.port_names.end())
    return nullptr;
  for (auto entry = it->second.rbegin(); entry != it->second.rend(); ++entry)
  {
    if (frame >= entry->from)
      return &*entry;
  }
  return nullptr;
}

std::string SeatShownName(const Match& match, int seat)
{
  if (const Orca::Net::KeyframeInfo::Name* entry = EntryAt(match, seat, match.running + 1);
      entry && !entry->name.empty())
  {
    return entry->name;
  }
  return Orca::Online::SeatName(seat);
}

std::string HostShownName(const Match& match)
{
  if (!match.host_name.empty() && match.host_name != "your friend")
    return match.host_name;
  return SeatShownName(match, match.host_seat);
}

void RefreshCoValues(Match& match, int frame)
{
  for (const int seat : match.co_seats)
  {
    if (!IsLocal(match, seat))
      continue;
    const auto* now = EntryAt(match, seat, frame);
    std::string name = Orca::Online::SeatName(seat);
    if (name.empty() && now)
      name = now->name;
    if (now ? now->name != name || !now->controls.empty() || !now->queue.empty() : !name.empty())
      AddName(match, {seat, frame, std::move(name), {}, {}});
  }
}

void RefreshOwnValues(Match& match, int frame)
{
  RefreshCoValues(match, frame);
  const Orca::Net::KeyframeInfo::Name* now = EntryAt(match, match.local_seat, frame);
  std::string own = Orca::Online::Seat() == match.local_seat ?
                        Orca::Online::SeatName(match.local_seat) :
                        std::string();
  if (own.empty() && now)
    own = now->name;
  std::vector<u8> controls = Orca::Events::OwnControls();
  std::vector<u8> queue = Orca::Events::OwnQueue();
  if (now ? now->name == own && now->controls == controls && now->queue == queue :
            own.empty() && controls.empty() && queue.empty())
  {
    return;
  }
  NOTICE_LOG_FMT(ROLLBACK, "Drop-in: port {} is {} from frame {}{}", Orca::Online::SeatPort(match.local_seat) + 1, own,
                 frame, controls.empty() ? "" : ", with their own controls");
  AddName(match, {match.local_seat, frame, std::move(own), std::move(controls), std::move(queue)});
}

std::vector<Orca::Events::PortInfo> PortsAt(Match& match, int frame)
{
  if (CanonicalBoot(match))
    return {Orca::Events::PortInfo{Orca::Online::SeatPort(0), "", false, {}, {}}};
  if (!match.joining && !match.session && Orca::Online::Seat() == match.local_seat)
  {
    const Orca::Net::KeyframeInfo::Name* now = EntryAt(match, match.local_seat, frame);
    std::string own = Orca::Online::SeatName(match.local_seat);
    if (own.empty() && now)
      own = now->name;
    std::vector<u8> controls = Orca::Events::OwnControls();
    if (now ? now->name != own || now->controls != controls : !own.empty() || !controls.empty())
    {
      std::vector<u8> queue = now ? now->queue : std::vector<u8>{};
      AddName(match,
              {match.local_seat, frame, std::move(own), std::move(controls), std::move(queue)});
    }
  }
  if (!match.joining && !match.session)
    RefreshCoValues(match, frame);
  const auto port = [&](int seat) {
    Orca::Events::PortInfo info{Orca::Online::SeatPort(seat), "", !IsLocal(match, seat), {}};
    if (const Orca::Net::KeyframeInfo::Name* entry = EntryAt(match, seat, frame))
    {
      info.name = entry->name;
      info.controls = entry->controls;
      info.queue = entry->queue;
    }
    return info;
  };
  std::vector<Orca::Events::PortInfo> ports;
  if (!match.session)
  {
    ports.push_back(port(match.local_seat));
    for (const int seat : match.co_seats)
      ports.push_back(port(seat));
    return ports;
  }
  for (int seat = 0; seat < Orca::Net::MAX_SEATS; ++seat)
  {
    if (match.session->Plugged(seat, frame))
      ports.push_back(port(seat));
  }
  return ports;
}

void PerfBoundary(const Match& match)
{
  Orca::Perf::Boundary boundary;
  boundary.first_run = !Rollback::IsResimulating();
  boundary.excluded = !match.started || match.finished || match.stale ||
                      match.replaying != nullptr || CanonicalBoot(match) || InTail(match) ||
                      Unseen(match) || (match.port && match.port->CatchingUp()) ||
                      Core::GetIsThrottlerTempDisabled();
  boundary.hidden = !Orca::UX::HostVisible();
  boundary.held = LastFrameHeldMs() > 0;
  boundary.held_frames = LastFrameHeldFrames();
  if (match.session)
  {
    for (int seat = 0; seat < Orca::Net::MAX_SEATS; ++seat)
    {
      if (!IsLocal(match, seat) && match.session->LeadOf(seat) == seat &&
          match.session->Plugged(seat, match.running))
        ++boundary.peers;
    }
    boundary.rerun_total = static_cast<u64>(match.session->GetStats().resimulated_frames);
  }
  boundary.paused_total = std::chrono::nanoseconds(s_paused_total_ns.load());
  Orca::Perf::OnBoundary(boundary);
}

void PublishStats(Match& match)
{
  if (match.session && match.session->Resimulating())
    return;
  const Orca::Net::Stats stats = match.session ? match.session->GetStats() : Orca::Net::Stats{};
  int peers = 0;
  if (match.session)
  {
    for (int seat = 0; seat < Orca::Net::MAX_SEATS; ++seat)
    {
      if (!IsLocal(match, seat) && match.session->LeadOf(seat) == seat &&
          match.session->Plugged(seat, match.running))
        ++peers;
    }
  }
  Orca::Events::LinkStats link;
  link.online = peers > 0;
  link.delay = stats.delay;
  link.deepest_rollback = match.stats_rbmax;
  link.stalls = stats.stalls;
  link.hitches = stats.hitches;
  link.peer_hitches = stats.peer_hitches;
  link.round_trip_ms = Orca::Online::RoundTripMs();
  link.confirmed_frame = stats.confirmed_frame;
  const Orca::Net::DirectSummary direct = Orca::Online::Direct();
  link.transport = direct.transport;
  link.link_rtt_ms = direct.rtt_ms;
  if (match.session)
  {
    const int mine = stats.rtt_median >= 0 ? stats.rtt_median : link.round_trip_ms;
    for (int seat = 0; seat < Orca::Net::MAX_SEATS && seat < 4; ++seat)
    {
      const int lead = match.session->LeadOf(seat);
      if (IsLocal(match, seat) || lead < 0 || lead >= Orca::Net::MAX_SEATS ||
          !match.session->Plugged(lead, match.running))
        continue;
      const std::optional<int> theirs = match.session->PeerRoundTrip(lead);
      int via_room = -1;
      if (mine >= 0)
        via_room = mine + (theirs ? *theirs : 0);
      else if (theirs)
        via_room = *theirs;
      const int direct_ms = direct.seat_rtt_ms[lead];
      const int ms = direct_ms >= 0 && via_room >= 0 ? std::min(direct_ms, via_room) :
                                                       std::max(direct_ms, via_room);
      link.seat_lamp_ms[seat] = std::max(ms, 0);
      link.lamp_ms = std::max(link.lamp_ms, ms);
    }
  }

  const auto now = Clock::now();
  match.stats_time += TakePausedTime();
  if (match.stats_frame < 0)
  {
    match.stats_time = now;
    match.stats_frame = match.running;
    match.stats_held = 0;
    match.stats_last = stats;
    match.stats_input = Orca::Events::GetInputAge();
  }
  const double seconds = std::chrono::duration<double>(now - match.stats_time).count();
  const bool reset = stats.rollbacks < match.stats_last.rollbacks ||
                     stats.stalls < match.stats_last.stalls;
  const Orca::Net::Stats& last = reset ? Orca::Net::Stats{} : match.stats_last;
  if (seconds > 0)
    link.rollbacks_per_second = (stats.rollbacks - last.rollbacks) / seconds;
  Orca::Events::PublishLinkStats(link);
  if (seconds < 1.0)
    return;
  picojson::object o;
  const auto num = [&o](const char* key, double value) { o[key] = picojson::value(value); };
  num("f", match.running);
  num("seat", match.local_seat);
  num("peers", peers);
  num("ping", link.round_trip_ms);
  num("pmed", stats.rtt_median);
  num("fpmed", stats.peer_rtt_median);
  num("delay", stats.delay);
  num("rb", stats.rollbacks - last.rollbacks);
  num("rbf", stats.resimulated_frames - last.resimulated_frames);
  num("rbmax", match.stats_rbmax);
  num("st", stats.stalls - last.stalls);
  num("stms", std::round(match.stats_stall_ms));
  num("ds", s_desyncs);
  num("late", stats.late_seats);
  num("fps",
      std::round((match.running - match.stats_frame + match.stats_held) / seconds * 10) / 10);
  num("hi", stats.hitches);
  num("phi", stats.peer_hitches);
  num("hil", stats.load_hitches);
  num("tx", link.transport);
  num("lrtt", link.link_rtt_ms);
  const Orca::Events::InputAge input = Orca::Events::GetInputAge();
  const Orca::Events::InputAge& input_last =
      input.frames < match.stats_input.frames ? Orca::Events::InputAge{} : match.stats_input;
  const u64 input_changes = input.changes - input_last.changes;
  const u64 input_frames = input.frames - input_last.frames;
  num("pa", input_changes > 0 ?
                std::round((input.age_ms - input_last.age_ms) / input_changes * 10) / 10 :
                -1);
  num("rpf", input_frames > 0 ?
                 std::round(double(input.reports - input_last.reports) / input_frames * 10) / 10 :
                 -1);
  if (!match.co_seats.empty())
    num("local", 1 + std::ranges::count_if(match.co_seats,
        [&](int seat) { return IsLocal(match, seat); }));
  Orca::Status::Stats(picojson::value(o).serialize());
  match.stats_time = now;
  match.stats_frame = match.running;
  match.stats_held = 0;
  match.stats_last = stats;
  match.stats_input = input;
  match.stats_rbmax = 0;
  match.stats_stall_ms = 0;
}

void Linger(Match& match)
{
  if (!match.linger_until)
    return;
  if (Orca::Online::SetOver())
  {
    NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: the verdict came (set over): done waiting");
    match.linger_until.reset();
    return;
  }
  const bool over = Orca::Online::RoomEnded();
  if (!over && Clock::now() < *match.linger_until)
  {
    Orca::Online::KeepAlive();
    return;
  }
  match.linger_until.reset();
  if (VoidFirst({.queue_room = Orca::Online::RoomQueue() != "private",
                 .match_live = Orca::Online::MatchLive(),
                 .set_over = Orca::Online::SetOver(),
                 .room_ended = over,
                 .waited_out = true}))
  {
    NOTICE_LOG_FMT(ROLLBACK,
                   "Matchmaking: no verdict in {} s with the set still open: reporting "
                   "it void before leaving",
                   VERDICT_WAIT.count());
    Orca::Online::ReportPeerFault();
  }
  Orca::UX::SetEnd::Current().NoVerdict(Orca::UX::SetEnd::NowMs());
  if (match.local_seat != 0)
  {
    NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: done waiting in the room ({})",
                   over ? Orca::Online::RoomErrorCode() : "no verdict in time");
    if (over && Orca::Online::RoomErrorCode() == "match-over")
      Orca::Status::Event("host-left match-over");
    Reseat(match);
    ReopenRoom(match);
    return;
  }
  if (!over)
  {
    NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: no verdict in {} s: a room of this game's own",
                   VERDICT_WAIT.count());
    ResetDropIn(match);
    ReopenRoom(match);
  }
}

void ConfirmResults(Match& match)
{
  if (!match.started || match.finished || !match.port)
    return;
  int confirmed = match.running;
  int plug = INT32_MAX;
  if (match.session)
  {
    if (match.session->Resimulating())
      return;
    confirmed = std::min(confirmed, match.session->ConfirmedFrame() + 1);
    int plugged = 0, latest = -1;
    for (int seat = 0; seat < Orca::Net::MAX_SEATS; ++seat)
    {
      const Orca::Net::SeatPlan& plan = match.session->Plan()[seat];
      if (plan.plug_from == Orca::Net::NEVER || plan.unplug_from != Orca::Net::NEVER)
        continue;
      ++plugged;
      latest = std::max(latest, plan.plug_from);
    }
    if (plugged >= (Orca::Online::RoomDoubles() ? Orca::Net::MAX_SEATS : 2))
      plug = latest;
  }
  Orca::UX::QueueApp::ConfirmTimeout(confirmed, match.local_seat);
  if (const auto no_show = Orca::UX::Game().Results().ConfirmNoShow(confirmed))
  {
    const bool mine = *no_show == match.local_seat;
    NOTICE_LOG_FMT(ROLLBACK, "Online rules: port {} didn't pick a character in time{}",
                   *no_show + 1, mine ? ": leaving the set" : "");
    if (mine && Orca::Online::RoomQueue() == "ranked" && plug != INT32_MAX)
    {
      Orca::Online::SetLeaveReason("no-show");
      Orca::UX::SetEnd::Current().SelfLeft(true, Orca::UX::SetEnd::NowMs());
      Orca::Online::RequestLeave();
    }
    else if (!mine)
    {
      match.opponent_no_show = Orca::Online::RoomQueue() == "ranked";
      Core::DisplayMessage("Your opponent didn't pick a character in time", 6000);
    }
  }
  std::vector<int> starts;
  const std::vector<Orca::Engine::GameResult> results =
      Orca::UX::Game().Results().Confirm(confirmed, plug, &starts);
  if (match.joining && match.leave_requested)
    starts.clear();
  for (const int start : starts)
  {
    NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: a game began (the fight from frame {})", start);
    Orca::Online::ReportGameStart(start);
  }
  if (!starts.empty() || !results.empty())
    Orca::UX::SetEnd::Current().GameBegan(Orca::UX::SetEnd::NowMs());
  for (const Orca::Engine::GameResult& r : results)
  {
    const auto report = Orca::Net::FromGameResult(r, Orca::Online::RoomDoubles() ?
        std::optional(Orca::Online::RoomTeams()) : std::nullopt);
    if (r.set_done)
    {
      NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: the set ended with game {}: {}", r.number,
                     r.set_done == 1 ? fmt::format("port {} won", r.set_winner + 1) :
                                       std::string("void"));
    }
    NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: a game ended at frame {}: {}", r.frame,
                   report.kind == Orca::Net::GameReport::Kind::Win ?
                       fmt::format("port {} won, {}", r.winner_port + 1, report.detail) :
                   report.kind == Orca::Net::GameReport::Kind::Draw ?
                       fmt::format("a draw, {}", report.detail) :
                       fmt::format("void ({})", report.why));
    Orca::Online::ReportGame(report);
    if (match.replay_out)
      match.replay_out->SendPacked();
  }
}

void PublishStall(Match& match, double stalled_ms)
{
  Orca::Events::LinkStats link = Orca::Events::GetLinkStats();
  link.stalled_ms = static_cast<int>(stalled_ms);
  link.waiting_seat = -1;
  if (stalled_ms > 0)
  {
    const auto missing = match.session && Orca::Online::RoomDoubles() ?
                             match.session->MissingInputSeats() : std::vector<int>{};
    link.waiting_seat = WaitingFriendSeat(match.local_seat, missing, match.joining, match.host_seat,
                                         {match.seated.begin(), match.seated.end()});
  }
  Orca::Events::PublishLinkStats(link);
}
}

namespace
{
void JoinerEvents(Match& match)
{
  for (const Orca::Net::PeerEvent& event : Orca::Online::TakePeerEvents())
  {
    if (event.kind == Orca::Net::PeerEvent::Kind::Names)
    {
      if (event.keyframe.names_version < match.names_version)
        continue;
      const std::vector<Orca::Net::KeyframeInfo::Name> before = AllNames(match);
      int earliest = INT32_MAX;
      for (const auto& name : event.keyframe.names)
      {
        if (std::find(before.begin(), before.end(), name) != before.end())
          continue;
        const Orca::Net::SeatPlan& plan = match.session->Plan()[name.seat];
        const int from = std::max(name.from, plan.plug_from);
        if (plan.plug_from == Orca::Net::NEVER || from > match.running || from >= plan.unplug_from)
          continue;
        earliest = std::min(earliest, from);
      }
      SetNames(match, event.keyframe);
      if (earliest <= match.running)
      {
        if (match.session->RerunFrom(earliest))
        {
          NOTICE_LOG_FMT(ROLLBACK, "Drop-in: a name for frame {} arrived at frame {}: re-running",
                         earliest, match.running + 1);
        }
        else
        {
          WARN_LOG_FMT(ROLLBACK,
                       "Drop-in: a name for frame {} arrived at frame {}, too late to re-run",
                       earliest, match.running + 1);
        }
      }
    }
    else if (event.kind == Orca::Net::PeerEvent::Kind::Left && event.host)
      match.host_gone = event.reason;
  }
}

void CheckOwnValues(Match& match)
{
  if (!match.session || !match.session->Error().empty())
    return;
  OwnValues own;
  if (Orca::Online::Seat() == match.local_seat)
    own.name = Orca::Online::SeatName(match.local_seat);
  own.controls = Orca::Online::HelloControls();
  own.queue = Orca::Online::HelloQueue();
  const int plug_from = match.session->Plan()[match.local_seat].plug_from;
  if (OwnValuesKept(AllNames(match), match.local_seat, plug_from, own))
    return;
  WARN_LOG_FMT(ROLLBACK,
               "Drop-in: the host's values for port {} aren't this player's own: the "
               "session ends",
               Orca::Online::SeatPort(match.local_seat) + 1);
  match.session->EndForPeer("The host changed this player's own port values");
}

void FinishLeave(Match& match)
{
  LogStats("at leave");
  if (Orca::Online::RoomQueue() != "ranked")
    Core::DisplayMessage(fmt::format("You left {}'s game", match.host_name), 4000);
  BecomeSolo(match, "left", nullptr);
  Orca::Status::Event("left");
}

bool LeavingPastUnplug(const Match& match)
{
  if (!match.joining || !match.leave_requested || !match.session)
    return false;
  const int unplug = match.session->Plan()[match.local_seat].unplug_from;
  return unplug != Orca::Net::NEVER && match.running >= unplug;
}

void HostGone(Core::System& system, Match& match, const std::function<Pad(int)>& local_pad)
{
  Orca::Net::PeerEvent event;
  event.reason = *std::exchange(match.host_gone, std::nullopt);
  if (Soft() && match.leave_requested && event.reason != "desync")
  {
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: the host left the room as this player left, frame {}",
                   match.running + 1);
    FinishLeave(match);
    RunSolo(match, local_pad);
    return;
  }
  LogStats("host left");
  if ((event.reason == "room-away-silent" || event.reason == "room-resume") &&
      WaitForAwayHost(match))
  {
    RunSolo(match, local_pad);
    return;
  }
  if (event.reason == "desync")
    ++s_desyncs;
  if (Soft())
  {
    if (event.reason == "desync")
    {
      Orca::Online::ReportDesync();
      Orca::Status::Report("desync", "Your games went out of sync, so the match ended");
    }
    if (Orca::Online::RoomQueue() == "private")
      Core::DisplayMessage(fmt::format("{} left: you play on", match.host_name), 6000);
    const bool linger = Orca::Online::RoomQueue() == "ranked" && Orca::Online::MatchLive() &&
                        !Orca::Online::RoomEnded();
    const bool stay = Orca::Online::RoomQueue() == "ranked" && Orca::Online::SetOver();
    if (stay)
    {
      NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: the host left the set's room (set over): this game "
                               "stays until its player leaves");
    }
    BecomeSolo(match, "host left", "host-left", false, linger || stay);
    if (linger)
    {
      NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: the host went mid-set: waiting for the verdict");
      match.linger_until = Clock::now() + VERDICT_WAIT;
    }
    RunSolo(match, local_pad);
    return;
  }
  if (event.reason == "desync")
    Orca::Status::Error("desync", "Your games went out of sync, so the match ended");
  else
    Orca::Status::Error("peer_left", "Your friend's game ended");
  End("host left");
  StopEmulation(system, event.reason == "desync" ?
                            "Your games went out of sync, so the match ended" :
                            "Your friend's game ended");
}

std::vector<int> LeftSeatsInPlay(const Match& match)
{
  std::vector<int> seats;
  if (!match.session || (Orca::Online::RoomQueue() == "private" && !Orca::Online::RoomDoubles()))
    return seats;
  for (const int seat : Orca::Online::LeftSeatsPending())
  {
    if (seat >= 0 && seat < Orca::Net::MAX_SEATS && seat != match.local_seat &&
        match.session->Plan()[seat].live_from != Orca::Net::NEVER)
    {
      seats.push_back(seat);
    }
  }
  return seats;
}

static std::optional<int> SessionBoundary(
    Core::System& system, Match& match,
    const std::function<Orca::Net::Pad(int)>& local_pad, const FrameHook& on_frame,
    const std::function<void(int)>& local_pad_waiting, std::optional<int> rewound_to,
    bool doubles, const std::function<bool()>& stopping, std::chrono::milliseconds silence_limit,
    const std::function<std::vector<int>()>& left_in_play);

std::optional<int> Boundary(Core::System& system,
                            const std::function<Orca::Net::Pad(int local_seat)>& local_pad,
                            const FrameHook& on_frame,
                            const std::function<void(int local_seat)>& local_pad_waiting)
{
  Match& match = TheMatch();
  if (match.stale)
  {
    if (match.store)
      match.store->Cancel();
    match.session.reset();
    Orca::Online::SetSessionActive(false);
    match.port.reset();
    match.job.reset();
    match.download.reset();
    match = {};
    Orca::UX::Game().Css()->DisarmOwnPick();
    Orca::Perf::NewBoot();
  }
  if (match.finished)
    return std::nullopt;
  match.stats_held += LastFrameHeldFrames();

  std::optional<int> rewound_to;
  if (!match.started)
  {
    match.started = true;
    if (!match.on_state_changed)
    {
      match.on_state_changed = Core::AddOnStateChangedCallback([](Core::State state) {
        if (state != Core::State::Uninitialized)
          return;
        End("emulation stopped");
        TheMatch().stale = true;
      });
    }
    match.store = MakeStore();
    if (const std::string leave = Orca::GetEnv("ORCA_TEST_LEAVE_AT"); !leave.empty())
      match.leave_at = std::atoi(leave.c_str());
    Orca::UX::Game().LoadOwnControls();
    if (ReplayStream::ProductMode())
      match.replay_play = ReplayStream::Player::FromEnv();
    if (!match.replay_play)
      Orca::Online::Start();
    std::optional<bool> joining;
    while (!match.replay_play && !(joining = Orca::Online::Joining()) &&
           !Orca::Online::RoomEnded() && !Stopping(system))
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    match.joining = joining.value_or(false);
    if (match.joining)
    {
      while (Orca::Online::Seat() < 0 && !Orca::Online::RoomEnded() && !Stopping(system))
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      match.local_seat = Orca::Online::Seat();
      if (!match.store)
      {
        StopEmulation(system,
                      "Couldn't join: this Subspace has nowhere to get your friend's game from");
        End("no keyframe store");
        return std::nullopt;
      }
      match.boot_time = Clock::now();
      Orca::Status::State("joining 0");
      match.port = std::make_unique<RingPort>(system, MAX_ROLLBACK);
      match.port->SetCatchingUp(true);
    }
    else
    {
      match.port = std::make_unique<RingPort>(system, MAX_ROLLBACK);
      Orca::Status::State("playing");
      NOTICE_LOG_FMT(ROLLBACK, "Drop-in: playing solo on port 1; room {}", Orca::Online::StatusLine());
    }
    if (!match.replay_play)
    {
      match.replay_out = ReplayStream::Writer::ForMatch();
      match.replay_play = match.joining ? nullptr : ReplayStream::Player::FromEnv();
    }
    if (match.replay_play)
      match.port->SetCatchingUp(true);
    system.GetJitInterface().ClearSafe();
    const auto* mod = Orca::Mod::ActiveMod();
    match.origin_rule = SelectOriginRule(mod ? &mod->manifest : nullptr,
                                        Orca::UX::Game().Read().FreshSupported(),
                                        Orca::Mod::GuardsFailed());
    if (match.origin_rule == OriginRule::Frame0)
    {
      NOTICE_LOG_FMT(ROLLBACK, "Drop-in: this game's main menu isn't read: its origin is its first "
                               "frame, with no fresh starts");
      if (!CaptureOrigin(system, match, 0))
        return std::nullopt;
    }
  }

  if (!Orca::Online::RoomEnded())
  {
    if (const auto reason = RoomRefusal(Orca::Online::RoomQueue(),
                                       Orca::UX::Game().Describe().results);
        !reason.empty())
    {
      Orca::UX::Search::End();
      Orca::UX::QueueApp::End();
      BecomeSolo(match, "ranked unsupported", "playing", true);
      Orca::Status::Report("ranked_unsupported", reason);
      Orca::UX::ShowToast(std::string(reason));
    }
  }

  if (CanonicalBoot(match))
  {
    const int frame = match.running + 1;
    const Orca::Profile* profile = Orca::ActiveProfile();
    const auto* mod = Orca::Mod::ActiveMod();
    if (mod && mod->manifest.builtin != Orca::Mod::Builtin::None)
    {
      const Core::CPUThreadGuard guard(system);
      Orca::Mod::RefreshGuards(guard, frame);
      if (match.origin_rule == OriginRule::Menu && Orca::Mod::GuardsFailed())
        match.origin_rule = SelectOriginRule(&mod->manifest, true, true);
    }
    OriginStep step;
    if (match.origin_rule == OriginRule::Declared && mod && mod->manifest.origin)
    {
      const Core::CPUThreadGuard guard(system);
      const Orca::UX::GuardMemory memory(guard);
      match.origin_words_streak = DeclaredOriginStreak(
          frame, match.origin_words_streak, *mod->manifest.origin, memory);
      step = DecideDeclaredOrigin(frame, match.origin_words_streak, *mod->manifest.origin);
    }
    else if (match.origin_rule == OriginRule::Failed)
      step = OriginStep::Fail;
    else
      step = DecideOrigin(frame, match.menu_built_streak,
                          profile ? profile->jit_clear_frame : std::nullopt);
    switch (step)
    {
    case OriginStep::Wait:
      break;
    case OriginStep::Capture:
      if (!CaptureOrigin(system, match, frame))
        return std::nullopt;
      break;
    case OriginStep::Fail:
      match.origin_failed = true;
      match.origin_rule = OriginRule::Failed;
      auto& memory = system.GetMemory();
      const u64 ram = RamChecksum({memory.GetRAM(), memory.GetRamSize()},
                                  {memory.GetEXRAM(), memory.GetEXRAM() ? memory.GetExRamSize() : 0});
      NOTICE_LOG_FMT(ROLLBACK, "Orca origin: rule failed at frame {}: RAM {:016x}", frame, ram);
      if (const auto sentence = OriginFailureReport(mod ? &mod->manifest : nullptr);
          !sentence.empty())
        Orca::Status::Report("no_origin", sentence);
      Orca::Status::WithdrawOnline();
      ERROR_LOG_FMT(ROLLBACK, "Drop-in: no built main menu by frame {}: this game has no origin, so "
                              "it can't take or join players",
                    frame);
      if (match.replay_play)
      {
        ReplayStream::PlayerHost host;
        host.stop = [&system](const std::string& message) { StopEmulation(system, message); };
        match.replay_play->Fail(host, "this game couldn't produce the replay's local origin");
        match.replay_play.reset();
        End("replay origin failed");
        return std::nullopt;
      }
      break;
    }
  }

  if (match.replay_play && OriginReady(match))
    return StepReplayPlayer(system, match, on_frame);
  if (match.replay_out && OriginReady(match) && !match.replaying)
  {
    const int final_end = match.session ? std::min(match.running + 1, match.session->FinalFrame()) :
                                          match.running + 1;
    if (!match.replay_out->Pump(match.replay, final_end, match.running + 1,
                                &match.replay.checksums,
                                match.session != nullptr))
      match.replay_out.reset();
  }

  Orca::Net::ReplayRecordingScope recording(&match.replay);
  bool replay_boundary = false;
  if (match.replaying)
  {
    switch (AdvanceReplay(system, match, on_frame))
    {
    case ReplayStep::Running:
      return std::nullopt;
    case ReplayStep::Failed:
      if (match.finished)
        return std::nullopt;
      rewound_to = match.running + 1;
      RunSolo(match, local_pad);
      return rewound_to;
    case ReplayStep::Done:
      replay_boundary = true;
      rewound_to = match.running + 1;
      break;
    }
  }

  if (match.fresh_pending)
  {
    if (const std::optional<int> origin = FreshStart(system, match))
    {
      rewound_to = origin;
    }
    else if (!match.finished)
    {
      match.friends_in_place = !match.waiting.empty();
      if (std::exchange(match.fresh_queue_armed, false))
        match.fresh_room = Orca::Online::Code();
    }
    if (match.finished)
      return std::nullopt;
  }

  if (MaybeRestoreQueueImage(system, match))
    rewound_to = match.running + 1;
  if (match.finished || !match.port)
    return std::nullopt;

  UpdateWanted(match);
  DropStaleKeyframe(match);
  Orca::UX::Game().Header().SetHeaderFree(
      !CanonicalBoot(match) &&
      HeaderFreeAt(match.running + 1, SoloQuiet(match),
                   match.keyframe ? std::optional(match.keyframe->frame) : std::nullopt));

  if (!replay_boundary)
  {
    const int frame = match.running + 1;
    const bool resimulating = (match.session && match.session->Resimulating()) || InTail(match);
    const std::vector<Orca::Events::PortInfo> ports = PortsAt(match, frame);
    auto* record = Orca::Net::ReplayRecordingScope::Frame(frame);
    if (record)
    {
      auto canonical = Orca::Net::SyncedPorts(ports);
      const Orca::Net::ReplayFrame* previous = match.replay.At(frame - 1);
      if (previous && previous->ports && *previous->ports == canonical)
        record->ports = previous->ports;
      else
        record->ports = std::make_shared<const std::vector<Orca::Events::PortInfo>>(std::move(canonical));
    }
    Orca::Net::ReplayScope replay_scope(record, resimulating);
    ReplayFrameHook(system, match, frame, resimulating, ports,
             !resimulating && !CanonicalBoot(match) &&
                 AloneAt(frame, SoloIdle(match), Orca::Online::DropInPending(),
                         match.keyframe ? std::optional(match.keyframe->frame) : std::nullopt),
             on_frame);
    if (!resimulating && ports != match.told_ports)
    {
      match.told_ports = ports;
      Orca::Events::NotifyPlugIn(frame, ports);
    }
    if (CanonicalBoot(match) && match.origin_rule == OriginRule::Menu)
    {
      match.menu_built_streak =
          Orca::UX::Game().Read().LastSeen().menu_built ? match.menu_built_streak + 1 : 0;
    }
  }

  TakeCommands(system, match);
  if (match.finished || !match.port)
    return std::nullopt;
  LeaveOnMismatch(match);
  if (match.want_queue_image)
    CaptureQueueImage(system, match);
  else
  {
    if (MaybeRestoreQueueImage(system, match))
      rewound_to = match.running + 1;
    if (match.finished || !match.port)
      return std::nullopt;
  }
  TakeLobbyPickNow(match);
  ArmKeptPick(match);
  RearmFriendsPick(match);
  ComeHome(match);
  TellNoRoom(match);

  if (match.joining && !match.session && match.join_in_play)
  {
    int frame = -1;
    switch (PumpJoin(match, &match.join_status))
    {
    case JoinPump::Waiting:
      RunSolo(match, local_pad);
      PublishStats(match);
      return std::nullopt;
    case JoinPump::Failed:
      FailJoin(system, match);
      break;
    case JoinPump::Ready:
      switch (LoadKeyframe(system, match, &frame))
      {
      case Load::Done:
        rewound_to = frame;
        break;
      case Load::Failed:
        FailJoin(system, match);
        break;
      case Load::Broken:
        return std::nullopt;
      }
      break;
    }
    if (match.finished)
      return std::nullopt;
  }
  else if (match.joining && !match.session && CanonicalBoot(match))
  {
    if (PumpJoin(match, &match.join_status) == JoinPump::Failed)
      FailJoin(system, match);
    if (match.finished || Stopping(system) || !match.port)
      return std::nullopt;
    if (match.joining)
    {
      RunSolo(match, local_pad);
      return std::nullopt;
    }
  }
  else if (match.joining && !match.session)
  {
    if (match.origin_failed)
    {
      match.join_error_code = "internal";
      match.join_error = "This game can't rebuild your friend's game";
      FailJoin(system, match);
    }
    else
    {
      rewound_to = JoinHost(system, match);
    }
    if (!rewound_to && (match.joining || match.finished || Stopping(system)))
      return std::nullopt;
  }

  if (match.replaying)
  {
    const auto step = AdvanceReplay(system, match, on_frame);
    if (step == ReplayStep::Failed && !match.finished)
    {
      rewound_to = match.running + 1;
      RunSolo(match, local_pad);
    }
    if (step != ReplayStep::Done)
      return rewound_to;
  }

  TakeLobbyDelay(match);
  TakeLobbyVolume(system);

  if (!match.joining)
  {

    Linger(match);
    if (!Stopping(system))
      KeepRoomOpen(match);
    if (!match.joining && !Orca::Online::RoomEnded() && !match.linger_until)
      HostEvents(system, match);
    if (!match.session)
    {
      RunSolo(match, local_pad);
      if (!InTail(match) && !Unseen(match))
        PublishStats(match);
      return rewound_to;
    }
  }
  if (!match.session)
    return std::nullopt;

  if (const auto round_trip_ms = NewRoundTrip(&match.round_trip_sequence))
    match.session->OnRoundTrip(*round_trip_ms);
  match.session->SetFixedDelay(ChosenDelay());
  const auto now = Clock::now();
  const auto held = std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<double, std::milli>(LastFrameHeldMs()));
  if (match.last_return && now - *match.last_return > HITCH_TIME && !match.port->CatchingUp())
  {
    if (now - *match.last_return - held > HITCH_TIME)
      match.session->OnLocalHitch();
    else
      match.session->OnLoadHitch();
  }

  if (match.joining)
  {
    JoinerEvents(match);
    CheckOwnValues(match);
    if (match.host_gone && !match.session->Resimulating())
    {
      HostGone(system, match, local_pad);
      return rewound_to;
    }
  }

  if (match.joining && match.leave_at >= 0 && match.running >= match.leave_at &&
      !match.leave_requested)
  {
    match.leave_requested = true;
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: leaving at frame {}", match.running);
    match.session->RequestLeave();
  }

  return SessionBoundary(system, match, local_pad, on_frame, local_pad_waiting, rewound_to,
                         Orca::Online::RoomDoubles(), [&] { return Stopping(system); },
                         LeavingPastUnplug(match) ? LEAVE_SILENCE_LIMIT : PEER_SILENCE_LIMIT,
                         [&] { return LeftSeatsInPlay(match); });
}

static std::optional<int> SessionBoundary(
    Core::System& system, Match& match,
    const std::function<Orca::Net::Pad(int)>& local_pad, const FrameHook& on_frame,
    const std::function<void(int)>& local_pad_waiting, std::optional<int> rewound_to,
    bool doubles, const std::function<bool()>& stopping, std::chrono::milliseconds silence_limit,
    const std::function<std::vector<int>()>& left_in_play)
{
  for (int attempt = 2; attempt > 0;)
  {
    const int players_before = match.session->PlayersInPlay();
    const Common::ScopeGuard spend_attempt([&] {
      if (match.session && match.session->PlayersInPlay() >= players_before)
        --attempt;
    });
    bool peer_silent = false;
    bool host_left = false;
    std::vector<int> left_seats;
    const auto step_start = Clock::now();
    auto stall_published = step_start;
    bool stall_shown = false;
    bool stall_claimed = false;
    const bool catching_up = match.port->CatchingUp() || match.session->CatchingUp();
    const int stalls_before = match.session->GetStats().stalls;
    const int running_before = match.running;
    std::chrono::nanoseconds waited{};
    std::optional<Orca::Perf::StepScope> perf_scope;
    perf_scope.emplace();
    const Orca::Net::Step step = StepSession(
        *match.session, *match.port, match.running, local_pad,
        [&] {
          if (local_pad_waiting)
            local_pad_waiting(match.local_seat);
          RetireReassignedCoSeats(match);
          const auto poll_time = Clock::now();
          if (poll_time - stall_published > std::chrono::milliseconds(10))
          {
            stall_published = poll_time;
            stall_shown = true;
            PublishStall(match, MsSince(step_start));
            Orca::Events::NotifyStall(static_cast<int>(MsSince(step_start)));
            if (Orca::Online::RoomQueue() == "private" || !Orca::Online::RoomRecovery())
              Orca::UX::SetEnd::Current().Stall(static_cast<int>(MsSince(step_start)),
                                                Orca::UX::SetEnd::NowMs());
            if (!match.joining)
              left_seats = left_in_play();
            if (!stall_claimed && poll_time - step_start >= RANKED_STALL_LIMIT &&
                Orca::Online::RoomQueue() == "ranked" && Orca::Online::MatchLive() &&
                !Orca::Online::RoomRecovery())
            {
              stall_claimed = true;
              WARN_LOG_FMT(ROLLBACK, "Matchmaking: the opponent's inputs stopped for {} s: "
                                     "claiming the set",
                           RANKED_STALL_LIMIT.count());
              Orca::Online::ReportStall();
              Core::DisplayMessage("Your opponent stopped playing: the set is yours", 6000);
            }
          }
          if (match.joining && Orca::Online::HostLeftPending())
            host_left = true;
          return host_left || !left_seats.empty() || stopping();
        },
        std::chrono::milliseconds{1},
        silence_limit, &peer_silent, &waited,
        [&](Clock::time_point poll_time) {
          const auto missing = FriendsMissingInputs(*match.session, doubles);
          return !LeavingPastUnplug(match) &&
                 (WaitForFriendsRoster(*match.session, match.joining, doubles) ||
                  Orca::Online::HoldInputSilence(step_start, poll_time, missing));
        },
        catching_up ? nullptr : &match.stall_budget,
        [&](Clock::time_point poll_time) {
          return WaitForFriendsRoster(*match.session, match.joining, doubles) ||
                 Orca::Online::RoomRecovering(poll_time);
        });
    perf_scope.reset();
    Orca::Perf::AddWait(waited);
    match.last_return = Clock::now();
    if (stall_shown)
    {
      PublishStall(match, 0);
      Orca::UX::SetEnd::Current().StallOver(step.kind == Orca::Net::StepKind::Run ||
                                                step.kind == Orca::Net::StepKind::Rollback,
                                            Orca::UX::SetEnd::NowMs());
    }
    if (match.session)
    {
      for (const std::string& note : match.session->TakeDelayNotes())
        NOTICE_LOG_FMT(ROLLBACK, "Online match: {}", note);
    }
    if (match.session && match.session->GetStats().stalls > stalls_before)
      match.stats_stall_ms += MsSince(step_start);
    if (step.kind == Orca::Net::StepKind::Rollback)
    {
      Orca::Perf::NoteRollback(running_before - step.frame + 1);
      match.stats_rbmax = std::max(match.stats_rbmax, running_before - step.frame + 1);
      auto* record = Orca::Net::ReplayRecordingScope::Frame(step.frame);
      auto ports = PortsAt(match, step.frame);
      auto canonical = Orca::Net::SyncedPorts(ports);
      if (record)
        record->ports = std::make_shared<const std::vector<Orca::Events::PortInfo>>(std::move(canonical));
      Orca::Net::ReplayScope replay_scope(record, true);
      ReplayFrameHook(system, match, step.frame, true, ports, false, on_frame, true);
    }
    if (step.kind == Orca::Net::StepKind::Run || step.kind == Orca::Net::StepKind::Rollback)
    {
      match.running = step.frame;
      PublishStats(match);
      if (step.kind == Orca::Net::StepKind::Run && !match.session->Resimulating() &&
          step.frame % 1800 == 0 && !match.port->CatchingUp())
      {
        LogStats(fmt::format("at frame {}", step.frame).c_str());
        NOTICE_LOG_FMT(ROLLBACK, "Drop-in: {}", match.session->Describe());
      }
      if (match.joining)
      {
        const int behind = match.session->AuthorityFrame() - step.frame;
        match.port->SetCatchingUp(match.session->CatchingUp() || behind > CATCH_UP_BEHIND);
        if (match.session->CatchingUp())
        {
          const int percent = 50 + std::clamp(50 - behind / 4, 0, 49);
          if (percent > match.last_percent)
          {
            match.last_percent = percent;
            Orca::Status::State(fmt::format("joining {}", percent));
          }
        }
        else if (!match.joined && match.session->Plugged(match.local_seat, step.frame))
        {
          match.joined = true;
          match.reopen_join = false;
          Orca::Status::State("playing");
          const double catch_up = MsSince(match.loaded_time);
          NOTICE_LOG_FMT(ROLLBACK,
                         "Drop-in: plugged into port {} at frame {}: catch-up {:.0f} ms, {:.0f} ms "
                         "since the keyframe was offered, {:.0f} ms since this Orca's first frame",
                         Orca::Online::SeatPort(match.local_seat) + 1, step.frame, catch_up, MsSince(match.offered_time),
                         MsSince(match.boot_time));
          ShowJoinMessage(
              fmt::format("Joined {}'s game in Port {}", match.host_name, Orca::Online::SeatPort(match.local_seat) + 1),
              4000);
        }
        if (match.session->LeftDone() && step.kind == Orca::Net::StepKind::Run &&
            !match.session->Resimulating())
        {
          NOTICE_LOG_FMT(ROLLBACK, "Drop-in: unplugged at frame {}",
                         match.session->Plan()[match.local_seat].unplug_from);
          if (Soft())
          {
            FinishLeave(match);
            return rewound_to;
          }
          LogStats("at leave");
          End("left");
          StopEmulation(system, "You left your friend's game");
          return rewound_to;
        }
      }
      else
      {
        if (step.kind == Orca::Net::StepKind::Run && !match.session->Resimulating() &&
            std::erase_if(match.plugging,
                          [&](int seat) { return match.session->Plugged(seat, step.frame); }) > 0 &&
            match.plugging.empty() && match.waiting.empty())
        {
          Orca::Status::State("friend-joined");
          match.friend_left_told = false;
          bool all_seats = true;
          if (match.doubles)
            for (int seat = 0; seat < Orca::Net::MAX_SEATS; ++seat)
              all_seats &= match.session->Plugged(seat, step.frame);
          Orca::Online::SetOpponentPlugged(all_seats);
          Orca::UX::Search::End();
          for (const auto& shared : match.shared_keyframes)
            DeleteStored(match, shared);
          match.shared_keyframes.clear();
        }
        if (match.session->Idle() && match.waiting.empty() && match.menu_waiting.empty())
        {
          if (++match.idle_frames >= IDLE_GRACE_FRAMES)
          {
            GoSolo(match, "no friends left", match.friend_left_told ? nullptr : "playing");
            AfterFriendsGone(match);
          }
        }
        else
        {
          match.idle_frames = 0;
        }
      }
      if (step.kind == Orca::Net::StepKind::Rollback)
        return step.frame;
      return rewound_to;
    }
    if (step.kind != Orca::Net::StepKind::Ended)
    {
      ERROR_LOG_FMT(ROLLBACK, "Online: unexpected session step {} at frame {}",
                    static_cast<int>(step.kind), step.frame);
      return rewound_to;
    }
    if (stopping())
      return rewound_to;
    const std::string error = match.session->Error();
    if ((!match.joining || Soft()) && Orca::Online::RoomEnded() &&
        Orca::Online::RoomErrorCode() == "match-over" && !Orca::Online::IsDesync(error))
    {
      if (match.joining)
        BecomeSolo(match, "match over", "host-left match-over", true);
      else
      {
        DropFriends(match, "network", false);
        GoSolo(match, "match over", nullptr);
      }
      RunSolo(match, local_pad);
      return rewound_to;
    }
    if (host_left && error.empty() && !match.session->Resimulating())
    {
      JoinerEvents(match);
      if (!match.host_gone)
        match.host_gone = "";
      HostGone(system, match, local_pad);
      return rewound_to;
    }
    if (!left_seats.empty() && error.empty() && !match.session->Resimulating())
    {
      for (const int seat : left_seats)
      {
        NOTICE_LOG_FMT(ROLLBACK, "Drop-in: port {} left the room while this game waited on it: "
                                 "unplugged at once, frame {}",
                       Orca::Online::SeatPort(seat) + 1, match.running + 1);
        match.session->DropSeat(seat);
      }
      continue;
    }
    if (!match.joining && match.session->CurrentFrame() == match.running + 1 &&
        (attempt == 2 || !error.empty()))
    {
      const bool room_gone = Orca::Online::RoomEnded();
      WARN_LOG_FMT(ROLLBACK, "Drop-in: unplugging every friend at frame {}: {}", match.running + 1,
                   peer_silent ? "no word from them" : error);
      const bool ranked_stall = error.empty() && Orca::Online::RoomQueue() == "ranked" &&
                                Orca::Online::MatchLive();
      const bool set_over = error.empty() && Orca::Online::SetOver();
      if (set_over)
        Orca::UX::SetEnd::Current().OpponentLeftAfterSet(Orca::UX::SetEnd::NowMs());
      if (!room_gone && !ranked_stall && !set_over)
      {
        std::vector<std::string> silent;
        if (peer_silent)
        {
          for (const int seat : match.session->MissingInputSeats())
          {
            if (seat != match.local_seat)
              silent.push_back(SeatShownName(match, seat));
          }
        }
        Core::DisplayMessage(peer_silent ? SilentSentence(silent) :
                                           fmt::format("Your friend was disconnected: {}", error),
                             6000);
      }
      if (error.empty())
      {
        std::array<int, Orca::Net::MAX_SEATS> leads{};
        for (int seat = 0; seat < Orca::Net::MAX_SEATS; ++seat)
          leads[seat] = match.session->LeadOf(seat);
        const auto missing = DropMissingFriends(*match.session, doubles);
        if (missing)
        {
          for (const int seat : *missing)
          {
            match.seated.erase(seat);
            match.plugging.erase(seat);
            match.doubles_seats.erase(seat);
            if (leads[seat] == seat)
              Orca::Online::DropPeer(seat, "stalled");
          }
          match.friend_left_told = true;
          Orca::Status::State("friend-left stalled");
          if (match.seated.empty())
            Orca::Online::SetOpponentPlugged(false);
          continue;
        }
        if (ranked_stall)
          Orca::UX::SetEnd::Current().OpponentDropped(Orca::UX::SetEnd::NowMs());
        Orca::Online::ReportStall();
        DropFriends(match, "stalled");
        continue;
      }
      if (Orca::Online::IsDesync(error))
        ++s_desyncs;
      if (VoidFirst({.queue_room = Orca::Online::RoomQueue() != "private",
                     .match_live = Orca::Online::MatchLive(),
                     .set_over = Orca::Online::SetOver(),
                     .room_ended = room_gone,
                     .from_peer = match.session->ErrorFromPeer()}))
      {
        Orca::Online::ReportPeerFault();
      }
      DropFriends(match, Orca::Online::IsDesync(error) ? "desync" : "network",
                  !room_gone && attempt == 2);
      GoSolo(match, "the session failed");
      if (!room_gone)
        AfterFriendsGone(match);
      RunSolo(match, local_pad);
      return rewound_to;
    }
    if (Soft() && match.joining && peer_silent && error.empty() && LeavingPastUnplug(match))
    {
      NOTICE_LOG_FMT(ROLLBACK, "Drop-in: no word from the host for {} s after leaving, frame {}",
                     LEAVE_SILENCE_LIMIT.count(), match.running + 1);
      FinishLeave(match);
      RunSolo(match, local_pad);
      return rewound_to;
    }
    WARN_LOG_FMT(ROLLBACK, "Online: session ended at frame {}: {}", match.running + 1,
                 error.empty() ? (peer_silent ? "no word from the host" : "no error") : error);
    NOTICE_LOG_FMT(ROLLBACK, "Drop-in: session state at the end: {}", match.session->Describe());
    if (match.joining && (error.empty() || error == "Connection lost") && WaitForAwayHost(match))
    {
      RunSolo(match, local_pad);
      return rewound_to;
    }
    if (peer_silent)
    {
      WARN_LOG_FMT(ROLLBACK, "Online: no word from the host for {} s at frame {}",
                   PEER_SILENCE_LIMIT.count(), match.running);
      Orca::Online::ReportPeerStalled(SilentSentence({HostShownName(match)}));
    }
    const bool survivable = Orca::Online::ReportSessionEnd(error, match.session->ErrorFromPeer());
    if (Orca::Online::IsDesync(error))
      ++s_desyncs;
    if (Soft() && survivable && match.joining && Orca::Online::RoomQueue() == "ranked" &&
        Orca::Online::MatchLive() && !Orca::Online::RoomEnded())
    {
      NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: the host went quiet mid-set: waiting for the verdict");
      if (peer_silent)
        Orca::Online::ReportStall();
      Orca::UX::SetEnd::Current().OpponentDropped(Orca::UX::SetEnd::NowMs());
      BecomeSolo(match, "the session ended", "host-left", false, true);
      match.linger_until = Clock::now() + VERDICT_WAIT;
      RunSolo(match, local_pad);
      return rewound_to;
    }
    if (Soft() && survivable && match.joining && Orca::Online::RoomQueue() == "ranked" &&
        Orca::Online::SetOver())
    {
      NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: the host's game ended after the set (set over): "
                               "this game stays until its player leaves");
      Orca::UX::SetEnd::Current().OpponentLeftAfterSet(Orca::UX::SetEnd::NowMs());
      BecomeSolo(match, "the session ended", "host-left", false, true);
      RunSolo(match, local_pad);
      return rewound_to;
    }
    if (Soft() && survivable && match.joining)
    {
      Core::DisplayMessage(peer_silent ? SilentSentence({HostShownName(match)}) :
                                         "The connection to your friend's game ended: you play on",
                           6000);
      BecomeSolo(match, "the session ended", "host-left");
      RunSolo(match, local_pad);
      return rewound_to;
    }
    End("session ended");
    StopEmulation(system, peer_silent ? SilentSentence({HostShownName(match)}) :
                                        fmt::format("Online match ended: {}",
                                                    error.empty() ? "stopped" : error));
    return rewound_to;
  }
  return rewound_to;
}

void NoteRankedFight(Match& match)
{
  if (match.ranked_fought || !match.queue_image || Orca::Online::RoomEnded() ||
      Orca::Online::RoomQueue() != "ranked")
  {
    return;
  }
  if (Orca::UX::Game().Results().LatestScene() == Orca::Engine::SceneKind::Fight)
  {
    match.ranked_fought = true;
    NOTICE_LOG_FMT(ROLLBACK, "Queue: a ranked fight in room {} (frame {}): back not ready after it",
                   Orca::Online::Code(), match.running + 1);
  }
}

void SetOverLeave(Match& match)
{
  Orca::UX::SetEnd::Model& set_end = Orca::UX::SetEnd::Current();
  if (!Orca::Online::SetOver() || match.finished)
  {
    if (std::exchange(match.set_over_hint, false))
      set_end.LeaveHint(false, false, 0);
    match.set_over_left = false;
    match.set_over_hold_since.reset();
    match.set_over_buttons = 0xFFFF;
    return;
  }
  if (match.set_over_left)
    return;
  using Scene = Orca::Engine::SceneKind;
  const std::optional<Scene> final_scene = Orca::UX::Game().Results().FinalScene();
  const bool on_results = Orca::UX::Game().Results().LatestScene() == Scene::Results;
  const bool alone = !match.session;
  const u16 buttons = s_local_pad ? Orca::Net::DecodePad(*s_local_pad).button : u16{0};
  const u16 pressed = static_cast<u16>(buttons & ~match.set_over_buttons);
  match.set_over_buttons = buttons;
  const auto now = Clock::now();
  if (on_results && (buttons & PAD_BUTTON_B))
  {
    if (!match.set_over_hold_since)
      match.set_over_hold_since = now;
  }
  else
  {
    match.set_over_hold_since.reset();
  }
  const float held =
      match.set_over_hold_since ?
          std::min(1.0f, static_cast<float>(std::chrono::duration<double>(
                                                now - *match.set_over_hold_since) /
                                            SET_OVER_HOLD)) :
          0.0f;
  const char* why = nullptr;
  if (final_scene && *final_scene != Scene::Results)
    why = "the game is off the results screen";
  else if (match.set_over_hold_since && now - *match.set_over_hold_since >= SET_OVER_HOLD)
    why = "B held";
  else if (alone && on_results && (pressed & (PAD_BUTTON_START | PAD_BUTTON_A)))
    why = "the opponent is gone and the player pressed on";
  match.set_over_hint = on_results && !why;
  set_end.LeaveHint(match.set_over_hint, alone, held);
  if (!why)
    return;
  match.set_over_left = true;
  match.set_over_hold_since.reset();
  set_end.LeavingSetRoom(Orca::UX::SetEnd::NowMs());
  NOTICE_LOG_FMT(ROLLBACK, "Matchmaking: set over: leaving room {} ({}), frame {}",
                 Orca::Online::Code(), why, match.running + 1);
  Orca::Online::RequestLeave();
}
}

namespace
{
Pad SeenLocalPad(const Match& match, const std::function<Pad(int)>& sample, int seat)
{
  const Pad pad = sample(seat);
  if (seat == LeadSeat(match))
    s_local_pad = pad;
  return pad;
}
}

std::optional<int> OnBoundary(Core::System& system,
                              const std::function<Orca::Net::Pad(int local_seat)>& local_pad,
                              const FrameHook& on_frame,
                              const std::function<void(int local_seat)>& local_pad_waiting)
{
  s_solo_idle = false;
  PerfBoundary(TheMatch());
  const bool was_replaying = TheMatch().replaying != nullptr;
  const std::function<Orca::Net::Pad(int)> seen = [&local_pad](int local_seat) {
    return SeenLocalPad(TheMatch(), local_pad, local_seat);
  };
  const std::optional<int> rewound_to = Boundary(system, seen, on_frame, local_pad_waiting);
  if (TheMatch().replaying || (was_replaying && !TheMatch().session))
  {
    s_solo_idle = SoloIdle(TheMatch());
    return rewound_to;
  }
  ConfirmResults(TheMatch());
  NoteRankedFight(TheMatch());
  SetOverLeave(TheMatch());
  Orca::UX::Game().SetLocalPort(Orca::Online::SeatPort(TheMatch().local_seat));
  s_solo_idle = SoloIdle(TheMatch());
  return rewound_to;
}

void LogStats(const char* when)
{
  const Match& match = TheMatch();
  if (!match.session)
    return;
  const Orca::Net::Stats& stats = match.session->GetStats();
  NOTICE_LOG_FMT(
      ROLLBACK,
      "Online match {}: frame {}, {} rollbacks, {} re-run frames, deepest {}, {} stalls ({} "
      "counted), {} waits, {} hitches here ({} more in game loads), {} there, frame advantage "
      "{:.2f}, input delay {}{} "
      "(+{} -{} reverted {}), {} counted stalls a frame more would have spared, {} checksums "
      "matched, confirmed frame {}, round trip {} ms (median {} ms here, {} ms there), link {}, "
      "{} snapshots (every {}, {:.2f} ms each), {} found late{}{}",
      when, match.session->CurrentFrame(), stats.rollbacks, stats.resimulated_frames,
      stats.deepest_rollback, stats.stalls, stats.counted_stalls, stats.waits, stats.hitches,
      stats.load_hitches, stats.peer_hitches, stats.frame_advantage, stats.delay,
      stats.delay_fixed ? " (chosen)" : "", stats.delay_raises, stats.delay_lowers,
      stats.delay_reverts, stats.spared_stalls, stats.checksums_matched, stats.confirmed_frame,
      Orca::Online::RoundTripMs(), stats.rtt_median, stats.peer_rtt_median,
      Orca::Online::Direct().line, stats.saves, stats.snapshot_every,
      match.port ? match.port->SaveMs() : 0.0, stats.late_seats,
      match.session->Error().empty() ? "" : "; ", match.session->Error());
}

void ReportJoinFailure(std::string_view code, std::string_view sentence)
{
  if (code == "host_gone")
    Orca::Status::Event("host-left host_gone");
  else if (code == "match-over")
    Orca::Status::Event("host-left match-over");
  else
    Orca::Status::Report(code.empty() || code == "room-unstable" ? "network" : code, sentence);
}

int LocalSeat()
{
  return LeadSeat(TheMatch());
}

int LocalSeatCount()
{
  const Match& match = TheMatch();
  if (!match.started || match.finished)
    return 1;
  return 1 + static_cast<int>(std::ranges::count_if(match.co_seats,
      [&](int seat) { return IsLocal(match, seat); }));
}

int LocalOrdinal(int seat)
{
  const Match& match = TheMatch();
  if (seat == match.local_seat)
    return 0;
  const auto co = std::ranges::find(match.co_seats, seat);
  return co == match.co_seats.end() ? -1 : static_cast<int>(co - match.co_seats.begin()) + 1;
}

bool Active()
{
  const Match& match = TheMatch();
  return match.started && !match.finished;
}

void End(const char* reason)
{
  Match& match = TheMatch();
  if (!match.started || match.finished)
    return;
  match.finished = true;
  Orca::UX::SetLocalPadCount(1);
  s_solo_idle = false;
  LogStats(fmt::format("ended ({})", reason).c_str());
  if (match.store)
    match.store->Cancel();
  match.session.reset();
  Orca::Online::SetSessionActive(false);
  match.port.reset();
  match.download.reset();
  match.replaying.reset();
  match.job.reset();
  if (match.keyframe)
    DeleteStored(match, *match.keyframe);
  for (const auto& shared : match.shared_keyframes)
    DeleteStored(match, shared);
  Orca::Online::Shutdown();
}

bool AloneAt(int frame, bool solo_idle, bool drop_in_pending, std::optional<int> stored_keyframe)
{
  return solo_idle && !drop_in_pending &&
         !(stored_keyframe && frame - *stored_keyframe <= KEYFRAME_FRESH_FRAMES);
}

bool HeaderFreeAt(int frame, bool solo_quiet, std::optional<int> stored_keyframe)
{
  return solo_quiet && !(stored_keyframe && frame - *stored_keyframe <= KEYFRAME_FRESH_FRAMES);
}

bool OwnValuesKept(const std::vector<Orca::Net::KeyframeInfo::Name>& names, int seat, int plug_from,
                   const OwnValues& own)
{
  std::vector<const Orca::Net::KeyframeInfo::Name*> mine;
  for (const auto& name : names)
  {
    if (name.seat == seat)
      mine.push_back(&name);
  }
  std::stable_sort(mine.begin(), mine.end(),
                   [](const auto* a, const auto* b) { return a->from < b->from; });
  const auto kept = [&own](const Orca::Net::KeyframeInfo::Name* entry) {
    if (!entry)
      return own.controls.empty() && own.queue.empty();
    return entry->controls == own.controls && entry->queue == own.queue &&
           (own.name.empty() || entry->name == own.name);
  };
  if (plug_from == Orca::Net::NEVER)
    return kept(mine.empty() ? nullptr : mine.back());
  const Orca::Net::KeyframeInfo::Name* in_force = nullptr;
  for (const auto* entry : mine)
  {
    if (entry->from <= plug_from)
      in_force = entry;
    else if (!kept(entry))
      return false;
  }
  return kept(in_force);
}

void StallBudget::Add(Clock::time_point start, Clock::time_point end)
{
  if (end <= start)
    return;
  std::erase_if(m_stalls, [&](const auto& stall) { return stall.second <= end - STALL_WINDOW; });
  m_stalls.emplace_back(start, end);
}

bool StallBudget::ExcuseRecovered(Clock::time_point now)
{
  std::erase_if(m_recoveries, [&](Clock::time_point at) { return at <= now - RECOVERY_WINDOW; });
  if (static_cast<int>(m_recoveries.size()) >= MAX_RECOVERIES)
    return false;
  m_recoveries.push_back(now);
  return true;
}

bool StallBudget::Poll(Clock::time_point now, Clock::time_point step_start, bool recovering)
{
  ObserveRecovery(recovering);
  auto total = Stalled(now);
  if (!m_recovery_seen)
  {
    for (const auto& [start, end] : m_pending_stalls)
      if (end > now - STALL_WINDOW)
        total += end - std::max(start, now - STALL_WINDOW);
    total += now - std::max(step_start, now - STALL_WINDOW);
  }
  return total >= STALL_BUDGET;
}

void StallBudget::FinishStep(std::optional<Clock::time_point> stall_start, Clock::time_point end,
                              bool still_stalled, bool resumed)
{
  if (stall_start && end > *stall_start)
    m_pending_stalls.emplace_back(*stall_start, end);
  if (still_stalled && resumed)
    return;
  bool excused = false;
  if (!m_pending_stalls.empty() && m_recovery_seen && resumed)
  {
    Clock::duration duration{};
    for (const auto& [start, until] : m_pending_stalls)
      duration += until - start;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    if (duration >= MIN_EXCUSED_STALL)
    {
      excused = ExcuseRecovered(end);
      if (excused)
        NOTICE_LOG_FMT(ROLLBACK,
                       "Online: stall of {} ms excused: recovered outage ({} of {} in {} min)",
                       ms, Recoveries(), MAX_RECOVERIES, RECOVERY_WINDOW.count());
      else
        NOTICE_LOG_FMT(ROLLBACK,
                       "Online: stall of {} ms not excused: recovered outage cap reached "
                       "({} of {} in {} min)",
                       ms, Recoveries(), MAX_RECOVERIES, RECOVERY_WINDOW.count());
    }
  }
  if (!excused)
    for (const auto& [start, until] : m_pending_stalls)
      Add(start, until);
  m_pending_stalls.clear();
  m_recovery_seen = false;
}

StallBudget::Clock::duration
StallBudget::Stalled(Clock::time_point now, std::optional<Clock::time_point> ongoing_since) const
{
  const Clock::time_point from = now - STALL_WINDOW;
  Clock::duration total{};
  const auto add = [&](Clock::time_point start, Clock::time_point end) {
    start = std::max(start, from);
    end = std::min(end, now);
    if (end > start)
      total += end - start;
  };
  for (const auto& [start, end] : m_stalls)
    add(start, end);
  if (ongoing_since)
    add(*ongoing_since, now);
  return total;
}

bool VoidFirst(const VerdictWaitInputs& in)
{
  return in.queue_room && in.match_live && !in.set_over && !in.room_ended &&
         (in.from_peer || in.waited_out);
}

bool MayComeHome(const HomeInputs& in)
{
  return in.local_seat != 0 && !in.joining && !in.session && !in.room_up && in.solo_idle &&
         !in.drop_in_pending && in.on_menus && !in.queue_image && !in.lingering;
}

LobbyPickPlan DecideLobbyPick(const LobbyPickInputs& in)
{
  LobbyPickPlan plan;
  if (in.joining || in.queue_room)
    return plan;
  const bool friends =
      (in.session && !in.session_idle) || in.drop_in_friends || in.arrival_pending;
  if (!friends)
  {
    plan.step = LobbyPickStep::Announce;
    plan.drop_keyframe = in.keyframe_kept && !in.session;
    return plan;
  }
  if (!in.host_cap || in.local_seat != 0)
    return plan;
  if (in.pick_cap)
  {
    plan.step = LobbyPickStep::Leave;
    return plan;
  }
  plan.step = LobbyPickStep::Announce;
  plan.arm = false;
  return plan;
}

bool AnnounceHomePick(bool css_pick, bool host_cap, bool queue_or_search)
{
  return css_pick && host_cap && !queue_or_search;
}

bool FriendEndsQueue(const FriendQueueInputs& in)
{
  return in.friend_coming && in.queue_or_search && in.local_seat == 0 && !in.joining &&
         in.friends_room;
}

bool JoinEndsQueue(bool queue_room, bool queue_or_search, bool queue_image)
{
  return !queue_room && (queue_or_search || queue_image);
}

FriendsPickStep DecideFriendsPick(const FriendsPickInputs& in)
{
  if (in.queue_or_search || in.elsewhere || !in.host_cap)
    return FriendsPickStep::Drop;
  return in.played && in.alone && in.on_pick_select ? FriendsPickStep::Arm : FriendsPickStep::Wait;
}

KeptPickStep DecideKeptPick(const KeptPickInputs& in)
{
  if (!in.stands || in.queue_or_search)
    return KeptPickStep::Drop;
  return in.alone ? KeptPickStep::Arm : KeptPickStep::Wait;
}

Orca::Net::Pads CanonicalBootPads()
{
  Orca::Net::Pads pads;
  pads.fill(Orca::Net::UNPLUGGED_PAD);
  pads[0] = Orca::Net::Pad{};
  return pads;
}

OriginStep DecideOrigin(int frame, int menu_built_streak, std::optional<u32> jit_clear_frame)
{
  if (menu_built_streak >= ORIGIN_SETTLE_FRAMES &&
      (!jit_clear_frame || frame > static_cast<int>(*jit_clear_frame)))
  {
    return OriginStep::Capture;
  }
  return frame >= ORIGIN_DEADLINE ? OriginStep::Fail : OriginStep::Wait;
}

OriginRule SelectOriginRule(const Orca::Mod::Manifest* manifest, bool menu_supported,
                            bool guards_failed)
{
  if (!manifest)
    return menu_supported ? OriginRule::Menu : OriginRule::Frame0;
  if (manifest->builtin != Orca::Mod::Builtin::None && !guards_failed)
    return OriginRule::Menu;
  if (manifest->origin)
    return OriginRule::Declared;
  return manifest->builtin == Orca::Mod::Builtin::None ? OriginRule::Frame0 : OriginRule::Failed;
}

OriginStep DecideDeclaredOrigin(int frame, int streak, const Orca::Mod::OriginDecl& decl)
{
  if (frame < static_cast<int>(decl.frame))
    return OriginStep::Wait;
  if (decl.words.empty())
    return frame == static_cast<int>(decl.frame) ? OriginStep::Capture : OriginStep::Fail;
  if (streak >= static_cast<int>(decl.settle))
    return OriginStep::Capture;
  return frame >= static_cast<int>(decl.frame) + ORIGIN_WORDS_WINDOW ? OriginStep::Fail :
                                                                    OriginStep::Wait;
}

std::string_view OriginFailureReport(const Orca::Mod::Manifest* manifest)
{
  return manifest ? "This pack's game didn't reach the start point its pack names, so friends "
                    "can't join." : "";
}

std::string_view RoomRefusal(std::string_view queue, bool results)
{
  return queue == "ranked" && !results ?
             "This game doesn't report match results, so it can't play Ranked. Try Casual or "
             "With Friends." : "";
}

int DeclaredOriginStreak(int frame, int streak, const Orca::Mod::OriginDecl& decl,
                         const Orca::UX::GuestMemory& memory)
{
  if (frame < static_cast<int>(decl.frame) || decl.words.empty())
    return 0;
  for (const auto& word : decl.words)
  {
    if (!memory.Valid(word.address) || memory.Read32(word.address) != word.word)
      return 0;
  }
  return std::min(streak + 1, static_cast<int>(decl.settle));
}

FreshKind FreshKindForOrigin(OriginRule rule)
{
  return rule == OriginRule::Declared ? FreshKind::Reset : FreshKind::Menu;
}

bool FreshExitAllowed(FreshKind kind, u8 exit)
{
  return kind == FreshKind::Reset ? exit == 0 : Orca::Net::ValidMenuExit(exit);
}

bool FreshTailSettled(FreshKind kind, int css_streak, int length, int frame, int offer_from)
{
  return kind == FreshKind::Reset ||
         ((css_streak >= FRESH_CSS_SETTLE || length >= FRESH_TAIL_LIMIT) && frame >= offer_from);
}

Orca::Net::ReplayArchive FreshReplayStart(FreshKind kind, int frame, u64 origin_hash, u8 exit,
                                         u32 seed)
{
  Orca::Net::ReplayArchive replay;
  replay.origin_hash = origin_hash;
  replay.first_frame = frame;
  if (kind == FreshKind::Menu)
  {
    replay.frames.resize(1);
    replay.frames[0].menu_exit = exit;
    replay.frames[0].seed = seed;
  }
  return replay;
}

bool LocalNandReady(bool is_wii, bool has_host_nand, bool temporary)
{
  return !is_wii || (has_host_nand && temporary);
}

std::string RoomReopenCode(const std::string& previous_code, const std::string& queue,
                            bool held, bool taken)
{
  return held && !taken && QueueMode(queue) == Orca::Engine::Mode::None ? previous_code :
                                                                       std::string();
}

bool PumpRoomReopenForTest()
{
  Match match;
  KeepRoomOpen(match);
  return match.joining && match.reopen_join;
}

std::vector<unsigned> TraceRoomReopenForTest(const std::vector<std::chrono::seconds>& times,
                                              const std::function<void()>& after_step)
{
  Match match;
  std::vector<unsigned> attempts;
  for (const auto time : times)
  {
    KeepRoomOpen(match, Clock::time_point{} + time);
    after_step();
    attempts.push_back(match.reopen_attempts);
  }
  return attempts;
}

std::vector<Pads> PumpSoloSeatsForTest(Core::System& system, const std::vector<int>& co_seats,
    int frames, bool fresh_tail, const std::function<Pad(int)>& sample,
    std::vector<Orca::Events::PortInfo>* ports)
{
  Match match;
  match.co_seats = co_seats;
  match.origin_frame = 0;
  match.port = std::make_unique<RingPort>(system, MAX_ROLLBACK);
  if (fresh_tail)
    match.fresh.emplace();
  for (int frame = 0; frame < frames; ++frame)
    RunSolo(match, sample);
  if (ports)
    *ports = PortsAt(match, frames);
  return match.log;
}

Pads PumpSoloRoomForTest(Core::System& system, Orca::Net::RelayRoom& room)
{
  const Common::ScopeGuard reset_count([] { Orca::UX::SetLocalPadCount(1); });
  Match match;
  match.port = std::make_unique<RingPort>(system, MAX_ROLLBACK);
  match.origin_frame = 0;
  HostEvents(system, match, &room, true);
  RunSolo(match, [](int) { return Pad{}; });
  return match.log.back();
}

std::optional<Pad> PumpSeenLocalPadForTest(int lead, const std::vector<int>& co_seats,
    const std::function<Pad(int)>& sample)
{
  const auto previous = std::exchange(s_local_pad, std::nullopt);
  const Common::ScopeGuard restore([&] { s_local_pad = previous; });
  Match match;
  match.local_seat = lead;
  SeenLocalPad(match, sample, lead);
  for (const int seat : co_seats)
    SeenLocalPad(match, sample, seat);
  return s_local_pad;
}

int PumpLeadSeatForTest(std::unique_ptr<Orca::Net::Session> session, int solo_seat)
{
  Match match;
  match.local_seat = solo_seat;
  match.session = std::move(session);
  return LeadSeat(match);
}

std::vector<Pads> PumpJoinerSoloForTest(std::unique_ptr<Orca::Net::Session> session,
    int frames, const std::function<Pad(int)>& sample)
{
  Match match;
  match.local_seat = session->LocalSeat();
  match.origin_frame = 0;
  match.running = session->CurrentFrame() - 1;
  match.port = std::make_unique<RingPort>(Core::System::GetInstance(), MAX_ROLLBACK);
  match.session = std::move(session);
  GoSolo(match, "test match over", nullptr);
  Reseat(match);
  for (int frame = 0; frame < frames; ++frame)
    RunSolo(match, sample);
  return match.log;
}

LaunchJoinBoundaryResult PumpLaunchJoinBoundaryForTest()
{
  Core::System& system = Core::System::GetInstance();
  Match& match = TheMatch();
  match = {};
  const Common::ScopeGuard cleanup([&] { match = {}; });
  match.started = true;
  match.joining = true;
  match.local_seat = Orca::Online::Seat();
  match.boot_time = Clock::now();
  match.port = std::make_unique<RingPort>(system, MAX_ROLLBACK, true);
  match.port->SetCatchingUp(true);
  Boundary(system, [](int) { return Pad{}; }, [](int, bool, const auto&, bool) {}, {});
  return {match.finished, match.joining, match.port != nullptr, match.running, match.local_seat,
          match.log.empty(), std::ranges::all_of(match.queued_local,
                                               [](const auto& queue) { return queue.empty(); })};
}

ReseatResult PumpReseatForTest(int seat, const std::vector<Pads>& log,
                             const std::vector<int>& co_seats)
{
  Match match;
  match.local_seat = seat;
  match.co_seats = co_seats;
  match.log = log;
  match.log_base = 17;
  for (auto& queue : match.queued_local)
    queue.push_back(Pad{});
  Reseat(match);
  return {match.local_seat, match.running, match.origin_frame, match.log_base, match.log,
          std::ranges::all_of(match.queued_local,
                              [](const auto& queue) { return queue.empty(); }), match.co_seats};
}

int PumpRoomReopenFailureForTest(const std::function<void()>& wait_for_failure)
{
  Core::System& system = Core::System::GetInstance();
  Match match;
  match.port = std::make_unique<RingPort>(system, MAX_ROLLBACK, true);
  match.running = 0;
  KeepRoomOpen(match);
  if (!match.joining || !match.reopen_join)
    return -1;
  wait_for_failure();
  if (PumpJoin(match, &match.join_status) != JoinPump::Failed)
    return -1;
  FailJoin(system, match);
  if (match.joining || match.finished || !match.reopen_pending)
    return -1;
  RunSolo(match, [](int) { return Pad{}; });
  std::this_thread::sleep_until(*match.reopen_pending);
  KeepRoomOpen(match);
  return match.running;
}

static HostRecoveryResult PumpHostForTest(Core::System& system, Orca::Net::RelayRoom& room,
                                          Orca::Net::Game& game, Orca::Net::ReplayArchive replay,
                                          int current_frame, bool session_alive,
                                          const std::vector<int>& seated, std::optional<bool> menus,
                                          std::unique_ptr<Orca::Net::KeyframeStore> store,
                                          bool host_requested = false,
                                          const std::function<void(const std::function<void()>&)>&
                                              after_admit = {})
{
  const Common::ScopeGuard reset_count([] { Orca::UX::SetLocalPadCount(1); });
  Match match;
  match.port = std::make_unique<RingPort>(system, MAX_ROLLBACK);
  match.store = std::move(store);
  match.origin_frame = replay.first_frame;
  match.origin_rule = OriginRule::Menu;
  match.running = current_frame - 1;
  match.replay = std::move(replay);
  match.fresh_queue_armed = host_requested;
  if (!match.replay.frames.empty() && match.replay.frames.back().ports)
  {
    for (const auto& port : *match.replay.frames.back().ports)
      AddName(match, {port.port, match.origin_frame, port.name, port.controls, port.queue});
  }
  match.log_base = match.origin_frame;
  match.log.resize(current_frame - match.log_base);
  match.seated.insert(seated.begin(), seated.end());
  match.co_seats = room.CoSeats();
  if (room.Doubles())
    match.doubles_seats = match.seated;
  if (session_alive)
  {
    Orca::Net::Config config;
    config.seats = Orca::Net::MAX_SEATS;
    config.per_peer_inputs = room.Doubles();
    config.local_seat = 0;
    config.co_seats = match.co_seats;
    config.authority_seat = 0;
    config.start_frame = current_frame;
    std::array<Orca::Net::SeatPlan, Orca::Net::MAX_SEATS> plan{};
    plan[0] = {0, Orca::Net::NEVER, 0};
    for (const int local : match.co_seats)
      plan[local] = {0, Orca::Net::NEVER, 0};
    for (const int seat : seated)
      plan[seat] = {0, Orca::Net::NEVER, 0};
    config.plan = plan;
    match.session = std::make_unique<Orca::Net::Session>(config, game, room);
    match.session->SetLog(match.log_base, match.log);
  }
  HostEvents(system, match, &room, menus);
  HostRecoveryResult result;
  if (match.job)
  {
    const auto until = Clock::now() + std::chrono::seconds(5);
    while (!match.job->done && Clock::now() < until)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (match.job->done && match.job->ok)
      result.keyframe_frame = match.job->info.frame;
    HostEvents(system, match, &room, menus);
  }
  if (after_admit)
    after_admit([&] { HostEvents(system, match, &room, menus); });
  if (match.session)
  {
    result.plan = match.session->Plan();
    for (int seat = 0; seat < Orca::Net::MAX_SEATS; ++seat)
      result.groups[seat] = match.session->LeadOf(seat);
    result.error = match.session->Error();
  }
  result.fresh_start = match.fresh_pending.has_value();
  result.seated = match.waiting.empty() && match.seated.contains(1);
  result.current_frame = match.running + 1;
  result.seats.assign(match.seated.begin(), match.seated.end());
  return result;
}

HostRecoveryResult PumpRecoveryHostForTest(Core::System& system, Orca::Net::RelayRoom& room,
                                          Orca::Net::Game& game, Orca::Net::ReplayArchive replay,
                                          int current_frame, bool session_alive,
                                          std::unique_ptr<Orca::Net::KeyframeStore> store)
{
  return PumpHostForTest(system, room, game, std::move(replay), current_frame, session_alive,
                         {1}, std::nullopt, std::move(store));
}

HostRecoveryResult PumpFriendsHostForTest(Core::System& system, Orca::Net::RelayRoom& room,
                                         Orca::Net::Game& game, Orca::Net::ReplayArchive replay,
                                         int current_frame, const std::vector<int>& seated,
                                         bool on_menus,
                                         std::unique_ptr<Orca::Net::KeyframeStore> store,
                                         bool host_requested,
                                         const std::function<void(const std::function<void()>&)>&
                                             after_admit)
{
  return PumpHostForTest(system, room, game, std::move(replay), current_frame, !seated.empty(),
                         seated, on_menus, std::move(store), host_requested, after_admit);
}

bool PumpHeldFriendsEventsForTest(Orca::Net::RelayRoom& room,
    std::unique_ptr<Orca::Net::Session>& session, const std::vector<int>& seated)
{
  Match match;
  match.running = session->CurrentFrame() - 1;
  match.origin_frame = 0;
  match.session = std::move(session);
  match.seated.insert(seated.begin(), seated.end());
  match.doubles_seats = match.seated;
  HostEvents(Core::System::GetInstance(), match, &room, false);
  session = std::move(match.session);
  return match.holding;
}

int PumpFriendsHostBoundaryForTest(std::unique_ptr<Orca::Net::Session>& session, int completed,
                                   const std::vector<int>& seated,
                                   const std::function<void()>& poll,
                                   const std::function<std::vector<int>()>& left_in_play,
                                   bool joining)
{
  Core::System& system = Core::System::GetInstance();
  Match match;
  match.port = std::make_unique<RingPort>(system, MAX_ROLLBACK, true);
  match.joining = joining;
  match.local_seat = joining ? 1 : 0;
  match.running = completed;
  match.seated.insert(seated.begin(), seated.end());
  match.session = std::move(session);
  SessionBoundary(system, match, [](int) { return Pad{}; }, [](int, bool, const auto&, bool) {},
                  [&](int) { poll(); }, std::nullopt, true, [] { return false; },
                  std::chrono::milliseconds(20), left_in_play);
  session = std::move(match.session);
  return match.running;
}

std::optional<std::vector<int>> FriendsMissingInputs(const Orca::Net::Session& session, bool doubles)
{
  if (!doubles)
    return std::nullopt;
  auto seats = session.MissingInputSeats();
  return seats.empty() ? std::nullopt : std::optional(std::move(seats));
}

std::optional<std::vector<int>> DropMissingFriends(Orca::Net::Session& session, bool doubles)
{
  auto seats = FriendsMissingInputs(session, doubles);
  if (!seats)
    return seats;
  bool grouped = false;
  for (int seat = 0; seat < Orca::Net::MAX_SEATS; ++seat)
    grouped |= session.LeadOf(seat) != seat;
  if (!grouped)
  {
    for (const int seat : *seats)
      session.DropSeat(seat);
    return seats;
  }
  std::set<int> leads;
  for (const int seat : *seats)
    if (!session.IsLocal(seat))
      leads.insert(session.LeadOf(seat));
  std::vector<int> group_seats;
  for (int seat = 0; seat < Orca::Net::MAX_SEATS; ++seat)
    if (!session.IsLocal(seat) && leads.contains(session.LeadOf(seat)) &&
        session.Plan()[seat].live_from != Orca::Net::NEVER)
      group_seats.push_back(seat);
  for (const int lead : leads)
    session.DropSeat(lead);
  return group_seats;
}

int WaitingFriendSeat(int local_seat, const std::vector<int>& missing, bool joining,
                       int host_seat, const std::vector<int>& seated)
{
  const auto other = [local_seat](int seat) { return seat >= 0 && seat != local_seat; };
  if (const auto seat = std::ranges::find_if(missing, other); seat != missing.end())
    return *seat;
  if (joining && other(host_seat))
    return host_seat;
  if (const auto seat = std::ranges::find_if(seated, other); seat != seated.end())
    return *seat;
  return -1;
}

bool WaitForFriendsRoster(const Orca::Net::Session& session, bool joining, bool doubles)
{
  const auto seats = FriendsMissingInputs(session, doubles);
  return joining && seats && std::ranges::find(*seats, session.AuthoritySeat()) == seats->end();
}

bool FriendsBatch::Ready(bool on_menus, int waiting, int plugging, bool recovering,
                         std::chrono::steady_clock::time_point now, int local_seats)
{
  if (waiting == 0)
  {
    Clear();
    return true;
  }
  if (recovering)
    return true;
  if (!on_menus || plugging != 0)
  {
    Clear();
    return false;
  }
  if (!m_until)
    m_until = now + std::chrono::seconds(2);
  return waiting >= Orca::Net::MAX_SEATS - local_seats || now >= *m_until;
}

std::optional<JoinCapture> ChooseJoinCapture(const Orca::Net::Session* session, bool doubles,
                                            int next, int earliest)
{
  if (next < earliest)
    return std::nullopt;
  if (!session || session->Settled())
    return JoinCapture{next, std::nullopt};
  if (doubles)
  {
    for (auto entry = session->LocalChecksums().rbegin(); entry != session->LocalChecksums().rend();
         ++entry)
    {
      if (entry->first >= earliest && entry->first <= next)
        return JoinCapture{entry->first, entry->second};
    }
  }
  return std::nullopt;
}

bool FightJoinHeld(const FightHoldInputs& in)
{
  return in.enabled && in.fight && in.waiting && !in.joining && !in.session &&
         !in.seated_or_plugging && !in.doubles && !in.in_queue_room && !in.let_in && !in.recovering;
}

FreshStep DecideFreshStart(const FreshInputs& in)
{
  if (!in.enabled || in.joining || in.session || in.seated_or_plugging)
    return FreshStep::None;
  if (in.queue_armed)
  {
    if (!in.origin_ready || in.busy || !in.in_queue_room)
      return FreshStep::Hold;
    return FreshStep::Now;
  }
  if (in.in_queue_room || !in.waiting || in.in_place)
    return FreshStep::None;
  if (!in.origin_ready || in.busy || in.single_player)
    return FreshStep::Hold;
  if (in.fresh_after >= 0)
    return in.history_frames > in.fresh_after ? FreshStep::Now : FreshStep::None;
  const bool due = in.kind == FreshKind::Reset ? in.history_time > FRESH_RESET_FRIENDS_AFTER :
                                                in.history_frames > FRESH_FRIENDS_AFTER;
  return due ? FreshStep::Now : FreshStep::None;
}

FreshStep WarnBeforeResetJoin(FreshStep step, FreshKind kind, bool queue_armed,
                             std::optional<Clock::time_point>* warned_at, Clock::time_point now)
{
  if (step != FreshStep::Now || kind != FreshKind::Reset || queue_armed)
  {
    warned_at->reset();
    return step;
  }
  if (!*warned_at)
  {
    *warned_at = now;
    return FreshStep::Warn;
  }
  return now - **warned_at < FRESH_RESET_JOIN_WARNING ? FreshStep::Hold : FreshStep::Now;
}

u8 FriendsFreshExit(std::optional<bool> css_pick_ranked)
{
  if (!css_pick_ranked)
    return Orca::Net::MENU_EXIT_FRIENDS;
  return *css_pick_ranked ? Orca::Net::MENU_EXIT_RANKED : Orca::Net::MENU_EXIT_CASUAL;
}

bool FreshUnseen(bool queue_room, int frames)
{
  return queue_room && frames < FRESH_UNSEEN_LIMIT;
}

bool HeaderFreeWhileCatchingUp(bool unseen_hold, bool in_tail, bool joining)
{
  return unseen_hold && !in_tail && !joining;
}

HeaderWait HeaderWaitAt(int* waited, bool in_tail, bool unseen, bool in_place, bool someone_waiting,
                        bool queue_room)
{
  if (in_tail || (unseen && !in_place))
    return HeaderWait::Wait;
  return StepHeaderWait(waited, in_place, someone_waiting, queue_room);
}

HeaderWait StepHeaderWait(int* waited, bool in_place, bool someone_waiting, bool queue_room)
{
  if (in_place || !someone_waiting)
  {
    *waited = 0;
    return in_place ? HeaderWait::Ready : HeaderWait::Wait;
  }
  ++*waited;
  if (queue_room)
  {
    if (*waited <= HEADER_FAIL_BOUNDARIES)
      return HeaderWait::Wait;
    *waited = 0;
    return HeaderWait::Fail;
  }
  return *waited > HEADER_WAIT_BOUNDARIES ? HeaderWait::Ready : HeaderWait::Wait;
}

bool SoloPauseAllowed()
{
  if (!Orca::Online::Enabled())
    return true;
  return s_solo_idle && !Orca::Online::DropInPending();
}

bool BeginPause()
{
  const bool was_pausing = s_pausing.exchange(true);
  if (!SoloPauseAllowed())
  {
    if (!was_pausing)
      s_pausing = false;
    return false;
  }
  s64 none = 0;
  s_paused_since_ns.compare_exchange_strong(none, NowNs());
  return true;
}

void EndPause()
{
  s_pausing = false;
  if (const s64 since = s_paused_since_ns.exchange(0); since != 0)
  {
    const s64 paused = NowNs() - since;
    s_paused_ns += paused;
    s_paused_total_ns += paused;
  }
}

bool Pausing()
{
  return s_pausing;
}

void WatchRoomWhilePaused()
{
  Match& match = TheMatch();
  if (!s_pausing || !match.started || match.finished || match.joining || match.session)
    return;
  KeepRoomOpen(match);
}

std::chrono::nanoseconds TakePausedTime()
{
  return std::chrono::nanoseconds(s_paused_ns.exchange(0));
}
}
