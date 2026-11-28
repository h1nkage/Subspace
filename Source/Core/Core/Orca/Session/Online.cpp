// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Session/Online.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <sstream>
#include <string>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <xxh3.h>

#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Common/Version.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/ConfigLoaders/GameConfigLoader.h"
#include "Core/ConfigManager.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/Config/SessionSettings.h"
#include "Core/Orca/BootAudit.h"
#include "Core/Orca/Bridge.h"
#include "Core/Orca/Bundle.h"
#include "Core/Orca/Engine/Module.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Status.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/Orca/Session/Events.h"
#include "Core/Orca/Session/RelayRoom.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#include "Core/Orca/UX/ControllerSource.h"
#include "Core/Orca/UX/Probe.h"
#include "Core/Rollback/ReplayStream.h"

namespace Orca::Online
{
namespace
{
constexpr auto TEARDOWN_WAIT = std::chrono::seconds(2);
std::mutex s_mutex;
std::shared_ptr<Net::RelayRoom> s_room;

std::string Env(const char* name)
{
  return GetEnv(name);
}

std::shared_ptr<Net::RelayRoom> Room()
{
  std::lock_guard lock(s_mutex);
  return s_room;
}
}

bool LocalRoom()
{
  return Env("ORCA_LOCAL_ROOM") == "1";
}

bool Enabled()
{
  if (Mod::LocalPackRequested())
    return false;
  if (!SessionActive())
    return false;
  if (Rollback::ReplayStream::ProductMode())
    return true;
  return Bridge::Ready() || (!Env("SUBSPACE_BRIDGE").empty() && !Env("SUBSPACE_TOKEN").empty()) ||
         !Env("ORCA_TEST_DEV_GAME").empty() || LocalRoom();
}

namespace
{
std::string RunningUxKeyPart()
{
  Engine::Module* module = Engine::ActiveModule();
  return UxKeyPart(Events::UXCompatVersion(), module ? module->Describe().compat : 0);
}

std::string UniversalKeyParts()
{
  return fmt::format(
      "thread={}|fma={}|rtc={}:{}|si={},{},{},{}|efb={}|xfb={}|access={}|bbox={}|save={:016x}|test={}"
      "|sdcard={}:{}",
      Config::Get(Config::MAIN_CPU_THREAD), Config::Get(Config::SESSION_USE_FMA),
      Config::Get(Config::MAIN_CUSTOM_RTC_ENABLE), Config::Get(Config::MAIN_CUSTOM_RTC_VALUE),
      static_cast<int>(Config::Get(Config::GetInfoForSIDevice(0))),
      static_cast<int>(Config::Get(Config::GetInfoForSIDevice(1))),
      static_cast<int>(Config::Get(Config::GetInfoForSIDevice(2))),
      static_cast<int>(Config::Get(Config::GetInfoForSIDevice(3))),
      Config::Get(Config::GFX_HACK_SKIP_EFB_COPY_TO_RAM),
      Config::Get(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM),
      Config::Get(Config::GFX_HACK_EFB_ACCESS_ENABLE), Config::Get(Config::GFX_HACK_BBOX_ENABLE),
      GetSeedSaveHash(), TestOverridesActive(), Config::Get(Config::MAIN_WII_SD_CARD),
      Config::Get(Config::MAIN_ALLOW_SD_WRITES));
}

std::string ModKeyParts(const Profile& profile)
{
  const Mod::ModInfo& mod = *profile.mod;
  const std::string disc = Env("ORCA_DISC_SHA1");
  std::string parts = fmt::format("orca2|{}|api={}|tree={}|sd={}|disc={}|sys=",
                                  Common::GetScmRevGitStr(), Mod::ENGINE_API, mod.tree.hash,
                                  mod.sd_image_hash.empty() ? "-" : mod.sd_image_hash,
                                  disc.empty() ? "-" : disc);
  std::vector<std::pair<std::string, u16>> ini_games{{profile.game_id, profile.revision}};
  if (profile.IsLauncher())
  {
    const SConfig& running = SConfig::GetInstance();
    ini_games = {{profile.boot_game_id, 0},
                 {profile.disc, profile.revision},
                 {running.GetGameID(), running.GetRevision()}};
  }
  for (const auto& [ini_game, ini_revision] : ini_games)
  {
    for (const std::string& name : ConfigLoaders::GetGameIniFilenames(ini_game, ini_revision))
    {
      std::string contents;
      if (File::ReadFileToString(File::GetSysDirectory() + GAMESETTINGS_DIR DIR_SEP + name,
                                 contents))
      {
        parts += fmt::format("{:016x},", XXH3_64bits(contents.data(), contents.size()));
      }
      else
      {
        parts += "-,";
      }
    }
  }
  parts += '|';
  for (const auto& [location, value] : profile.forced)
    parts += fmt::format("{}.{}.{}={}|", static_cast<int>(location.system), location.section,
                         location.key, value);
  parts += UniversalKeyParts();
  parts += RunningUxKeyPart();
  parts += fmt::format("|hostport={}", HostPort());
  return parts;
}
}

std::string UxKeyPart(int engine, int game)
{
  return game > 0 ? fmt::format("|ux={}.{}", engine, game) : fmt::format("|ux={}", engine);
}

std::string CompatibilityKeyParts()
{
  if (const Profile* profile = ActiveProfile(); profile && profile->mod)
    return ModKeyParts(*profile);
  std::string parts = fmt::format("orca|{}|", Common::GetScmRevGitStr());
  if (const Profile* profile = ActiveProfile())
  {
    parts += fmt::format("{}|{}|", profile->game_id, profile->revision);
    const std::string sys = File::GetSysDirectory() + "Subspace/";
    std::vector<std::string> files{sys + profile->game_id + ".ini",
                                   sys + profile->game_id + ".patches"};
    std::vector<std::pair<std::string, u16>> ini_games{{profile->game_id, profile->revision}};
    if (profile->IsLauncher())
    {
      const SConfig& running = SConfig::GetInstance();
      ini_games = {{profile->boot_game_id, 0},
                   {profile->disc, profile->revision},
                   {running.GetGameID(), running.GetRevision()}};
    }
    for (const auto& [ini_game, ini_revision] : ini_games)
    {
      for (const std::string& name : ConfigLoaders::GetGameIniFilenames(ini_game, ini_revision))
      {
        files.push_back(File::GetSysDirectory() + GAMESETTINGS_DIR DIR_SEP + name);
        files.push_back(File::GetUserPath(D_GAMESETTINGS_IDX) + name);
      }
    }
    if (profile->IsLauncher())
    {
      parts += fmt::format("launcher={}|disc={}|sd={}|", profile->launcher_hash, profile->disc,
                           profile->sd_image_hash);
    }
    for (const std::string& path : files)
    {
      std::string contents;
      if (File::ReadFileToString(path, contents))
        parts += fmt::format("{:016x}|", XXH3_64bits(contents.data(), contents.size()));
      else
        parts += "-|";
    }
    for (const auto& [location, value] : profile->forced)
      parts += fmt::format("{}.{}.{}={}|", static_cast<int>(location.system), location.section,
                           location.key, value);
  }
  parts += UniversalKeyParts();
  parts += "|disc=" + Env("ORCA_DISC_SHA1");
  parts += RunningUxKeyPart();
  parts += fmt::format("|hostport={}", HostPort());
  return parts;
}

std::string CompatibilityKey()
{
  const std::string parts = CompatibilityKeyParts();
  const std::string key = fmt::format("orca2:{:016x}", XXH3_64bits(parts.data(), parts.size()));
  {
    static std::mutex logged_mutex;
    static std::string logged;
    std::lock_guard lock(logged_mutex);
    if (parts != logged)
    {
      logged = parts;
      NOTICE_LOG_FMT(NETPLAY, "Orca: compatibility {} from {}", key, parts);
    }
  }
  return key;
}

namespace
{
void StartLocked(Net::RoomOptions options)
{
  if (Mod::LocalPackRequested())
  {
    NOTICE_LOG_FMT(NETPLAY, "Orca: local packs are offline only; room refused");
    Status::Line("orca pack refused online (local packs are offline only)");
    return;
  }
  if (Rollback::ReplayStream::ProductMode())
    return;
  options.compatibility = CompatibilityKey();
  if (const Profile* profile = ActiveProfile())
  {
    std::string game = profile->game_id;
    Common::ToLower(&game);
    options.mode = "orca-" + game;
  }
  if (!Env("ORCA_NAME").empty())
    options.player_name = Env("ORCA_NAME").substr(0, 32);
  if (options.joining || options.join_if_taken)
  {
    options.controls = Events::OwnControls();
    options.queue = Events::OwnQueue();
  }
  NOTICE_LOG_FMT(NETPLAY, "Orca: joining an online room ({}), compatibility {}",
                 options.joining ? "to join a friend's game" : "as the host",
                 options.compatibility);
  s_room = std::make_shared<Net::RelayRoom>(std::move(options));
}
}

void Start()
{
  if (!BootAudit::WaitForOnline(BootAudit::SessionAudit()))
    return;
  std::lock_guard lock(s_mutex);
  if (s_room)
    return;
  Net::RoomOptions options;
  options.room_code = Env("ORCA_ROOM");
  options.joining = Env("ORCA_JOIN") == "1";
  StartLocked(std::move(options));
}

void StartRoom(const std::string& code, bool joining, bool friends_room, bool join_if_taken)
{
  if (!BootAudit::WaitForOnline(BootAudit::SessionAudit()))
    return;
  std::lock_guard lock(s_mutex);
  if (s_room)
    return;
  Net::RoomOptions options;
  options.room_code = code;
  options.joining = joining;
  options.friends_room = friends_room;
  options.join_if_taken = join_if_taken;
  options.ask_app_for_room = false;
  StartLocked(std::move(options));
}

std::vector<u8> HelloControls()
{
  const auto room = Room();
  return room ? room->HelloControls() : std::vector<u8>{};
}

std::vector<u8> HelloQueue()
{
  const auto room = Room();
  return room ? room->HelloQueue() : std::vector<u8>{};
}

std::string SeatName(int seat)
{
  const auto room = Room();
  return room ? room->SeatName(seat) : "";
}

std::string Code()
{
  const auto room = Room();
  return room ? room->Code() : "";
}

namespace
{
std::mutex s_request_mutex;
std::optional<std::string> s_join_request;
std::optional<std::string> s_host_request;
bool s_leave_request = false;
std::optional<int> s_chosen_delay;
std::atomic<bool> s_direct_wanted{true};
std::atomic<RecordChoice> s_record_choice{RecordChoice::Auto};
}

void RequestJoin(const std::string& code)
{
  std::lock_guard lock(s_request_mutex);
  s_join_request = code;
}

std::optional<std::string> TakeJoinRequest()
{
  std::lock_guard lock(s_request_mutex);
  return std::exchange(s_join_request, std::nullopt);
}

void RequestHost(const std::string& code)
{
  std::lock_guard lock(s_request_mutex);
  s_host_request = code;
}

std::optional<std::string> TakeHostRequest()
{
  std::lock_guard lock(s_request_mutex);
  return std::exchange(s_host_request, std::nullopt);
}

std::string RoomQueue()
{
  const auto room = Room();
  return room ? room->PlayQueue() : "private";
}

bool RoomDoubles()
{
  const auto room = Room();
  return room && room->Doubles();
}

std::map<int, int> RoomTeams()
{
  const auto room = Room();
  return room ? room->Teams() : std::map<int, int>{};
}

bool MatchLive()
{
  const auto room = Room();
  return room && room->MatchLive();
}

bool SetOver()
{
  const auto room = Room();
  return room && room->GetState() != Net::RoomState::Ended && room->SetOver();
}

void SetOpponentPlugged(bool plugged)
{
  if (const auto room = Room())
    room->SetOpponentPlugged(plugged);
}

void ReportGame(const Net::GameReport& report)
{
  if (const auto room = Room())
    room->ReportGame(report);
}

void ReportGameStart(int start)
{
  if (const auto room = Room())
    room->ReportGameStart(start);
}

int KnownSetGames()
{
  const auto room = Room();
  return room ? room->KnownSetGames() : 0;
}

void ReportPeerFault()
{
  if (const auto room = Room())
    room->ReportFault();
}

void KeepAlive()
{
  if (const auto room = Room())
    room->KeepAlive();
}

void SendReplayStream(const std::vector<u8>& bytes)
{
  if (const auto room = Room())
    room->SendReplayStream(bytes);
}

void SendReplayPacked(const std::vector<std::vector<u8>>& pieces, int frames, u64 generation)
{
  if (const auto room = Room(); room && room->Generation() == generation)
    room->SendReplayPacked(pieces, frames);
}

u64 RoomGeneration()
{
  const auto room = Room();
  return room ? room->Generation() : 0;
}

void ReportDesync()
{
  if (const auto room = Room())
    room->ReportDesync();
}

void ReportStall()
{
  if (const auto room = Room(); room && room->Queue() == "ranked" && room->MatchLive())
    room->ReportStall();
}

void RequestLeave()
{
  std::lock_guard lock(s_request_mutex);
  s_leave_request = true;
}

bool TakeLeaveRequest()
{
  std::lock_guard lock(s_request_mutex);
  return std::exchange(s_leave_request, false);
}

void SetDirectWanted(bool on)
{
  s_direct_wanted = on;
}

bool DirectWanted()
{
  return s_direct_wanted;
}

void SetRecordChoice(RecordChoice choice)
{
  s_record_choice = choice;
}

bool RecordWanted()
{
  const RecordChoice choice = s_record_choice;
  if (choice == RecordChoice::Off || !Status::Cap(Status::RECORD_CAP))
    return false;
  const auto room = Room();
  if (!room)
    return false;
  return choice == RecordChoice::On || room->Queue() == "ranked";
}

void SetChosenDelay(std::optional<int> frames)
{
  std::lock_guard lock(s_request_mutex);
  s_chosen_delay = frames;
}

std::optional<int> ChosenDelay()
{
  std::lock_guard lock(s_request_mutex);
  return s_chosen_delay;
}

namespace
{
std::mutex s_delay_request_mutex;
std::optional<DelayRequest> s_delay_request;
std::optional<int> s_volume_request;
std::atomic<bool> s_show_ping{false};
std::atomic<int> s_hud_names{-1};
std::atomic<bool> s_corner_sprite{true};
std::atomic<bool> s_corner_fps{true};
std::atomic<bool> s_corner_ping{true};
std::atomic<bool> s_frame_ms{false};

void TakeLobbyLine(const std::string& line)
{
  std::istringstream in(line);
  std::string cmd;
  if (in >> cmd && cmd == "ping")
  {
    int on = -1;
    if (in >> on && (on == 0 || on == 1))
      s_show_ping.store(on == 1, std::memory_order_relaxed);
    return;
  }
  if (cmd == "cornersprite" || cmd == "cornerfps" || cmd == "framems" || cmd == "cornerping")
  {
    std::atomic<bool>& flag = cmd == "cornersprite" ? s_corner_sprite :
                              cmd == "cornerfps"    ? s_corner_fps :
                              cmd == "framems"      ? s_frame_ms :
                                                      s_corner_ping;
    int on = -1;
    if (in >> on && (on == 0 || on == 1))
      flag.store(on == 1, std::memory_order_relaxed);
    return;
  }
  if (cmd == "names")
  {
    int mode = -1;
    if (in >> mode && mode >= 0 && mode <= 4)
      s_hud_names.store(mode, std::memory_order_relaxed);
    return;
  }
  if (cmd == "volume")
  {
    int volume = -1;
    if (in >> volume && volume >= 0 && volume <= 100)
    {
      std::lock_guard lock(s_delay_request_mutex);
      s_volume_request = volume;
    }
    return;
  }
  DelayRequest request;
  if (cmd != "delay" || !(in >> request.frames) || request.frames < 1 ||
      request.frames > Net::Config{}.max_delay)
  {
    return;
  }
  int at = -1;
  if (in >> at && at >= 0)
    request.at = at;
  std::lock_guard lock(s_delay_request_mutex);
  s_delay_request = request;
}

void StartLobbyReader()
{
  if (Orca::GetEnv("ORCA_SUBSPACE_COMMANDS") != "1")
    return;
  std::thread([] {
    std::string pending;
    char buffer[4096];
    while (true)
    {
#ifdef _WIN32
      const int got = _read(0, buffer, sizeof(buffer));
#else
      const ssize_t got = read(STDIN_FILENO, buffer, sizeof(buffer));
#endif
      if (got <= 0)
        return;
      pending.append(buffer, static_cast<size_t>(got));
      size_t end;
      while ((end = pending.find('\n')) != std::string::npos)
      {
        std::string line = pending.substr(0, end);
        pending.erase(0, end + 1);
        if (!line.empty() && line.back() == '\r')
          line.pop_back();
        TakeLobbyLine(line);
      }
      if (pending.size() > 4096)
        pending.clear();
    }
  }).detach();
}

void EnsureLobbyReader()
{
  static std::once_flag started;
  std::call_once(started, StartLobbyReader);
}
}

std::optional<DelayRequest> TakeDelayRequest()
{
  EnsureLobbyReader();
  std::lock_guard lock(s_delay_request_mutex);
  return std::exchange(s_delay_request, std::nullopt);
}

std::optional<int> TakeVolumeRequest()
{
  EnsureLobbyReader();
  std::lock_guard lock(s_delay_request_mutex);
  return std::exchange(s_volume_request, std::nullopt);
}

bool ShowPing()
{
  EnsureLobbyReader();
  return s_show_ping.load(std::memory_order_relaxed);
}

bool ShowCornerSprite()
{
  EnsureLobbyReader();
  return s_corner_sprite.load(std::memory_order_relaxed);
}

bool ShowCornerFps()
{
  EnsureLobbyReader();
  return s_corner_fps.load(std::memory_order_relaxed);
}

bool ShowCornerPing()
{
  EnsureLobbyReader();
  return s_corner_ping.load(std::memory_order_relaxed);
}

bool ShowFrameMs()
{
  EnsureLobbyReader();
  return s_frame_ms.load(std::memory_order_relaxed);
}

int HudNames()
{
  EnsureLobbyReader();
  if (const int mode = s_hud_names.load(std::memory_order_relaxed); mode >= 0)
    return mode;
  const std::string launch = Orca::GetEnv("ORCA_SUBSPACE_HUD_NAMES");
  return launch == "0" ? 0 : launch == "1" ? 1 : launch == "2" ? 2 : launch == "4" ? 4 : 3;
}

std::optional<bool> Joining()
{
  const auto room = Room();
  return room ? room->Joining() : std::nullopt;
}

Net::Transport* Transport()
{
  const auto room = Room();
  return room.get();
}

int Seat()
{
  const auto room = Room();
  return room ? room->Seat() : -1;
}

int HostPort()
{
  static const int port = [] {
    const std::string value = Env("ORCA_HOST_PORT");
    return value.size() == 1 && value[0] >= '0' && value[0] <= '3' ? value[0] - '0' : 0;
  }();
  return port;
}

int SeatPort(int seat)
{
  const int host = HostPort();
  if (seat == 0)
    return host;
  if (seat == host)
    return 0;
  return seat;
}

int PortSeat(int port)
{
  return SeatPort(port);
}

std::vector<int> CoSeats()
{
  const auto room = Room();
  return room ? room->CoSeats() : std::vector<int>{};
}

std::optional<std::vector<int>> CoSeatsForSolo()
{
  const auto room = Room();
  return room ? room->CoSeatsForSolo() : std::optional(std::vector<int>{});
}

int LocalIndex(int seat)
{
  const auto room = Room();
  if (!room || seat < 0)
    return -1;
  if (seat == room->Seat())
    return 0;
  const auto co_seats = room->CoSeats();
  const auto co = std::ranges::find(co_seats, seat);
  return co == co_seats.end() ? -1 : static_cast<int>(co - co_seats.begin()) + 1;
}

void SetSessionActive(bool active)
{
  if (const auto room = Room())
    room->SetSessionActive(active);
}

bool TryStartSession(const std::vector<int>& co_seats)
{
  const auto room = Room();
  return room ? room->TryStartSession(co_seats) : co_seats.empty();
}

std::vector<int> GroupOf(int seat)
{
  const auto room = Room();
  return room ? room->GroupOf(seat) : std::vector<int>{};
}

std::vector<Net::PeerEvent> TakePeerEvents()
{
  const auto room = Room();
  return room ? room->TakePeerEvents() : std::vector<Net::PeerEvent>{};
}

bool HostLeftPending()
{
  const auto room = Room();
  return room && room->HostLeftPending();
}

std::vector<int> LeftSeatsPending()
{
  const auto room = Room();
  return room ? room->LeftSeatsPending() : std::vector<int>{};
}

std::optional<std::chrono::steady_clock::time_point> HostAwayDeadline()
{
  const auto room = Room();
  return room ? room->HostAwayDeadline() : std::nullopt;
}

void RetryPeer(int seat)
{
  if (const auto room = Room())
    room->RetryPeer(seat);
}

bool HoldInputSilence(std::chrono::steady_clock::time_point since,
                       std::chrono::steady_clock::time_point now,
                       std::optional<std::vector<int>> missing)
{
  const auto room = Room();
  return room && room->HoldInputSilence(since, now, std::move(missing));
}

bool RoomRecovery()
{
  const auto room = Room();
  return room && room->RoomRecovery();
}

bool RoomRecovering(std::chrono::steady_clock::time_point now)
{
  const auto room = Room();
  return room && room->RoomRecovering(now);
}

bool RoomEnded()
{
  const auto room = Room();
  return !room || room->GetState() == Net::RoomState::Ended;
}

bool InRoom()
{
  const auto room = Room();
  return room && room->Connected();
}

bool FreshTicket(bool fresh, std::string* ticket, std::string* store_url, std::string* error)
{
  const auto room = Room();
  if (!room)
  {
    *error = "not in a room";
    return false;
  }
  return room->FreshTicket(fresh, ticket, store_url, error);
}

void DropPeer(int seat, const std::string& reason)
{
  if (const auto room = Room())
    room->DropPeer(seat, reason);
}

void HoldJoins(bool holding, bool fight)
{
  if (const auto room = Room())
    room->HoldJoins(holding, fight);
}

bool IsDesync(const std::string& session_error)
{
  return session_error.starts_with("Desync") ||
         session_error.find("changed its input") != std::string::npos ||
         session_error.find("arrived after") != std::string::npos ||
         session_error.find("history changed") != std::string::npos ||
         session_error.starts_with("No local input");
}

bool IsRollbackFailure(const std::string& session_error)
{
  return session_error.starts_with("No snapshot to roll back") ||
         session_error.find("exceeds the limit") != std::string::npos;
}

void BroadcastNames(const std::vector<Net::KeyframeInfo::Name>& names, int version)
{
  if (const auto room = Room())
    room->BroadcastNames(names, version);
}

namespace
{
struct Teardowns
{
  std::mutex mutex;
  std::condition_variable done;
  int running = 0;
};
Teardowns& TheTeardowns()
{
  static Teardowns* const teardowns = new Teardowns;
  return *teardowns;
}
}

bool RoomsLeaving()
{
  Teardowns& teardowns = TheTeardowns();
  std::lock_guard lock(teardowns.mutex);
  return teardowns.running > 0;
}

void ShutdownInBackground()
{
  std::shared_ptr<Net::RelayRoom> room;
  {
    std::lock_guard lock(s_mutex);
    room = std::move(s_room);
  }
  if (!room)
    return;
  Teardowns& teardowns = TheTeardowns();
  {
    std::lock_guard lock(teardowns.mutex);
    ++teardowns.running;
  }
  std::thread([leaving_room = std::move(room), &teardowns]() mutable {
    leaving_room->Leave();
    leaving_room.reset();
    std::lock_guard lock(teardowns.mutex);
    --teardowns.running;
    teardowns.done.notify_all();
  }).detach();
}

void OfferKeyframe(int seat, const Net::KeyframeInfo& info)
{
  if (const auto room = Room())
    room->OfferKeyframe(seat, info);
}

namespace
{
std::atomic<bool> s_prepare_join{false};
std::atomic<bool> s_let_in{false};
}

void PrepareJoin()
{
  s_prepare_join.store(true, std::memory_order_relaxed);
}

bool TakePrepareJoin()
{
  return s_prepare_join.exchange(false, std::memory_order_relaxed);
}

void LetIn()
{
  s_let_in.store(true, std::memory_order_relaxed);
}

bool TakeLetIn()
{
  return s_let_in.exchange(false, std::memory_order_relaxed);
}

bool DropInPending()
{
  {
    std::lock_guard lock(s_request_mutex);
    if ((s_join_request && Status::Cap("join")) || (s_leave_request && Status::Cap("leave")) ||
        (s_host_request && Status::Cap("host")))
    {
      return true;
    }
  }
  const auto room = Room();
  if (!room || room->GetState() == Net::RoomState::Ended)
    return false;
  return s_prepare_join.load(std::memory_order_relaxed) ||
         s_let_in.load(std::memory_order_relaxed) || room->ArrivalPending();
}

bool ArrivalPending()
{
  const auto room = Room();
  return room && room->GetState() != Net::RoomState::Ended && room->ArrivalPending();
}

int RoundTripMs(u32* sequence)
{
  const auto room = Room();
  return room ? room->RoundTripMs(sequence) : -1;
}

Net::DirectSummary Direct()
{
  const auto room = Room();
  return room ? room->Direct() : Net::DirectSummary{};
}

std::vector<int> RecentRoundTrips(u32* sequence)
{
  const auto room = Room();
  return room ? room->RecentRoundTrips(sequence) : std::vector<int>{};
}

namespace
{
std::string AppCode(std::string code)
{
  return code == "taken" ? "network" : code;
}
}

std::string RoomErrorCode()
{
  const auto room = Room();
  return room ? AppCode(room->ErrorCode()) : "";
}

bool RoomTaken()
{
  const auto room = Room();
  return room && room->ErrorCode() == "taken";
}

std::string StatusLine()
{
  const auto room = Room();
  return room ? room->Status() : "";
}

bool ReportSessionEnd(const std::string& session_error, bool from_peer)
{
  const auto room = Room();
  if (IsDesync(session_error))
  {
    if (room)
    {
      room->ReportDesync();
      room->SetLeaveReason("desync");
    }
    Status::Report("desync", "Your games went out of sync, so the match ended");
    return true;
  }
  if (IsRollbackFailure(session_error))
  {
    Status::Report(
        "network",
        fmt::format("Your game couldn't roll back ({}), so you left the match", session_error));
    return true;
  }
  if (room && room->GetState() == Net::RoomState::Ended)
  {
    if (room->ErrorCode() == "room-unstable")
    {
      if (room->Joining().value_or(false))
        Status::Report("network", room->Status());
      return true;
    }
    if (const std::string code = AppCode(room->ErrorCode()); !code.empty() && code != "match-over")
      Status::Report(code, room->Status());
    return true;
  }
  if (session_error.empty())
    return true;
  if (from_peer || session_error.find("sent an invalid packet") != std::string::npos)
  {
    if (room)
      room->ReportFault();
    Status::Report("network", session_error);
    return true;
  }
  Status::Error("internal", session_error);
  return false;
}

void SetLeaveReason(const std::string& reason)
{
  if (const auto room = Room())
    room->SetLeaveReason(reason);
}

void ReportPeerStalled(std::string_view sentence)
{
  if (const auto room = Room())
    room->SetLeaveReason("stalled");
  Status::Report("peer_left", sentence);
}

void TestDropRoom()
{
  if (const auto room = Room())
  {
    if (!room->RoomRecovery() && Env("ORCA_TEST_DROP_SILENT") == "1")
    {
      static std::vector<std::shared_ptr<Net::RelayRoom>> s_kept;
      s_kept.push_back(room);
    }
    room->TestDropConnection();
  }
}

void TestDirect(const std::string& command)
{
  if (const auto room = Room())
    room->TestDirect(command);
}

void Shutdown()
{
  std::shared_ptr<Net::RelayRoom> room;
  {
    std::lock_guard lock(s_mutex);
    room = std::move(s_room);
  }
  if (room)
    room->Leave();
  Teardowns& teardowns = TheTeardowns();
  std::unique_lock lock(teardowns.mutex);
  if (!teardowns.done.wait_for(lock, TEARDOWN_WAIT, [&] { return teardowns.running == 0; }))
    WARN_LOG_FMT(NETPLAY, "Orca: {} room(s) still leaving at shutdown", teardowns.running);
}
}
