// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Local/RoomServer.h"

#include <algorithm>

#include <picojson.h>

#include "Common/Logging/Log.h"
#include "Core/Orca/SafeJson.h"

namespace Orca::Local
{
namespace
{
std::string Json(const picojson::object& object)
{
  return picojson::value(object).serialize();
}

std::string Error(const std::string& code)
{
  return Json({{"error", picojson::value(code)}});
}

std::vector<u8> Bytes(const std::string& text)
{
  return std::vector<u8>(text.begin(), text.end());
}
}  // namespace

RoomServer::RoomServer(Sink& sink) : m_sink(sink)
{
}

int RoomServer::Start(const std::vector<Player>& players)
{
  std::lock_guard lock(m_mutex);
  if (m_running)
  {
    // Games that have closed since they joined (window closed, crash): the host's ends the room,
    // so Start opens a fresh one for everyone; a joiner's frees its seat, so Start launches it again.
    std::vector<u8> gone;
    for (const auto& [pid, seat] : m_launched)
    {
      if (m_joined.contains(pid) && !m_sockets.contains(pid))
        gone.push_back(pid);
    }
    for (const u8 pid : gone)
    {
      if (m_launched.at(pid).seat == 0)
      {
        StopLocked();
        break;
      }
      ForgetLocked(pid);
    }
  }
  if (!m_running)
  {
    if (players.empty())
      return -1;
    m_code = NewRoomCode();
    m_running = true;
    m_launched.clear();
    m_pending.clear();
    m_members.clear();
    m_sockets.clear();
    m_joined.clear();
    m_away.clear();
    m_keyframes.clear();
    m_keyframe_bytes = 0;
    const size_t count = std::min<size_t>(players.size(), MAX_ROOM_SEATS);
    // Two engines play an ordinary friends room; three or four the four-seat room (the
    // "doubles" room, which runs every player's inputs separately). Both are tested modes.
    m_doubles = count > 2;
    for (size_t i = 0; i < count; ++i)
      m_launched[players[i].pid] = {players[i].name, static_cast<int>(i)};
    const u8 host = players.front().pid;
    for (size_t i = 1; i < count; ++i)
      m_pending.push_back(players[i].pid);
    NOTICE_LOG_FMT(NETPLAY, "Orca local room {}: {} players{}", m_code, count,
                   m_doubles ? " (four-seat room)" : "");
    LaunchLocked(host, false);
    return static_cast<int>(count);
  }

  // Drop-in: newcomers take the lowest free seats.
  int added = 0;
  for (const Player& player : players)
  {
    if (m_launched.contains(player.pid))
      continue;
    std::set<int> used;
    for (const auto& [pid, seat] : m_launched)
      used.insert(seat.seat);
    int free_seat = -1;
    for (int seat = 1; seat < MAX_ROOM_SEATS; ++seat)
    {
      if (!used.contains(seat))
      {
        free_seat = seat;
        break;
      }
    }
    if (free_seat < 0)
      break;
    m_launched[player.pid] = {player.name, free_seat};
    ++added;
    // A joiner waits for the host's engine like the first ones did.
    const bool host_in = std::any_of(m_launched.begin(), m_launched.end(), [&](const auto& p) {
      return p.second.seat == 0 && m_sockets.contains(p.first);
    });
    if (host_in)
      LaunchLocked(player.pid, true);
    else
      m_pending.push_back(player.pid);
  }
  return added;
}

bool RoomServer::Running() const
{
  std::lock_guard lock(m_mutex);
  return m_running;
}

std::string RoomServer::Code() const
{
  std::lock_guard lock(m_mutex);
  return m_code;
}

std::vector<u8> RoomServer::LaunchedPids() const
{
  std::lock_guard lock(m_mutex);
  std::vector<u8> pids;
  for (const auto& [pid, seat] : m_launched)
    pids.push_back(pid);
  return pids;
}

void RoomServer::Stop()
{
  std::lock_guard lock(m_mutex);
  StopLocked();
}

void RoomServer::StopLocked()
{
  if (!m_running)
    return;
  for (const auto& [pid, conn] : m_sockets)
    m_sink.Closed(pid, conn);
  NOTICE_LOG_FMT(NETPLAY, "Orca local room {} closed", m_code);
  m_running = false;
  m_launched.clear();
  m_pending.clear();
  m_members.clear();
  m_sockets.clear();
  m_joined.clear();
  m_away.clear();
  m_keyframes.clear();
  m_keyframe_bytes = 0;
}

void RoomServer::LaunchLocked(u8 pid, bool joining)
{
  LaunchInfo launch;
  launch.room = m_code;
  launch.ticket = IdLocked(pid);
  launch.joining = joining;
  for (const auto& [launched, seat] : m_launched)
    launch.room_pids.push_back(launched);
  m_sink.Launch(pid, launch);
}

std::string RoomServer::RosterLocked(u8 local, const std::string& kind, u8 changed,
                                     bool away_flag) const
{
  // Members by seat, as the fixture's roster lists them.
  std::vector<std::pair<int, u8>> members;
  for (const u8 pid : m_members)
    members.emplace_back(m_launched.at(pid).seat, pid);
  std::sort(members.begin(), members.end());

  picojson::array players, participants;
  for (const auto& [seat, pid] : members)
  {
    const std::string id = IdLocked(pid);
    const std::string& name = m_launched.at(pid).name;
    picojson::object player{{"id", picojson::value(id)}, {"name", picojson::value(name)}};
    if (m_away.contains(pid) || (away_flag && pid == changed))
      player["away"] = picojson::value(true);
    players.emplace_back(player);
    participants.emplace_back(picojson::object{{"id", picojson::value(id)},
                                               {"connectionId", picojson::value(id)},
                                               {"slot", picojson::value(static_cast<double>(seat))},
                                               {"name", picojson::value(name)}});
  }
  const std::string me = IdLocked(local);
  picojson::object room{{"code", picojson::value(m_code)},
                        {"queue", picojson::value("private")},
                        {"size", picojson::value(static_cast<double>(MAX_ROOM_SEATS))},
                        {"doubles", picojson::value(m_doubles)}};
  picojson::object result{
      {"t", picojson::value(kind)},
      {"me", picojson::value(me)},
      {"host", picojson::value(members.empty() ? me : IdLocked(members.front().second))},
      {"lifecycle", picojson::value("game")},
      {"revision", picojson::value(1.0)},
      {"room", picojson::value(room)},
      {"player", picojson::value(picojson::object{
                     {"id", picojson::value(changed ? IdLocked(changed) : me)}})},
      {"players", picojson::value(players)},
      {"participants", picojson::value(participants)},
  };
  return Json(result);
}

void RoomServer::SendOthersLocked(u8 except, const std::string& text)
{
  for (const auto& [pid, conn] : m_sockets)
  {
    if (pid != except)
      m_sink.Text(pid, conn, text);
  }
}

void RoomServer::RosterToOthersLocked(u8 changed, const std::string& kind, bool away_flag)
{
  for (const auto& [pid, conn] : m_sockets)
  {
    if (pid != changed)
      m_sink.Text(pid, conn, RosterLocked(pid, kind, changed, away_flag));
  }
}

void RoomServer::Open(u8 pid, u32 conn, const std::string& room, const std::string& ticket)
{
  std::lock_guard lock(m_mutex);
  if (!m_running || room != m_code)
  {
    // The engine reads this code as its friend having left that game (Common/WebSocket.cpp).
    m_sink.OpenResult(pid, conn, 403, Json({{"code", picojson::value("host_gone")}}));
    return;
  }
  if (!m_launched.contains(pid) || ticket != IdLocked(pid))
  {
    // Not seated in this game: the engine says the room is full.
    m_sink.OpenResult(pid, conn, 409, Error("Room is full"));
    return;
  }

  const bool returning = m_joined.contains(pid);
  if (const auto old = m_sockets.find(pid); old != m_sockets.end() && old->second != conn)
    m_sink.Closed(pid, old->second);
  m_members.insert(pid);
  m_away.erase(pid);
  m_joined.insert(pid);
  m_sockets[pid] = conn;
  m_sink.OpenResult(pid, conn, 101, "");
  m_sink.Text(pid, conn, RosterLocked(pid, "welcome", 0, false));
  RosterToOthersLocked(pid, returning ? "back" : "join", false);
  INFO_LOG_FMT(NETPLAY, "Orca local room {}: player {} {} (seat {})", m_code, pid,
               returning ? "is back" : "is in", m_launched.at(pid).seat + 1);

  // The host is in: now its friends may come.
  if (m_launched.at(pid).seat == 0 && !m_pending.empty())
  {
    const std::vector<u8> pending = std::move(m_pending);
    m_pending.clear();
    for (const u8 friend_pid : pending)
    {
      if (m_launched.contains(friend_pid))
        LaunchLocked(friend_pid, true);
    }
  }
}

void RoomServer::Text(u8 pid, u32 conn, const std::string& text)
{
  std::lock_guard lock(m_mutex);
  const auto socket = m_sockets.find(pid);
  if (!m_running || socket == m_sockets.end() || socket->second != conn)
    return;
  picojson::value parsed;
  if (text.size() > MAX_TEXT_BYTES || !Json::ParseJson(text, &parsed) ||
      !parsed.is<picojson::object>())
    return;
  const picojson::object& message = parsed.get<picojson::object>();
  const auto field = [&](const char* name) -> const picojson::value* {
    const auto it = message.find(name);
    return it == message.end() ? nullptr : &it->second;
  };
  const picojson::value* type = field("t");
  if (!type || !type->is<std::string>())
    return;
  const std::string& t = type->get<std::string>();

  if (t == "ping")
  {
    // Written by hand so "t" comes first: the gateway spots pongs by their start.
    const picojson::value* at = field("at");
    m_sink.Text(pid, conn, at ? R"({"t":"pong","at":)" + at->serialize() + "}" :
                                R"({"t":"pong"})");
  }
  else if (t == "msg")
  {
    const picojson::value* data = field("d");
    if (!data || !data->is<picojson::object>())
      return;
    const std::string out = R"({"t":"msg","from":)" + picojson::value(IdLocked(pid)).serialize() +
                            R"(,"d":)" + data->serialize() + "}";
    const picojson::value* to = field("to");
    if (to && to->is<std::string>() && !to->get<std::string>().empty())
    {
      for (const auto& [other, other_conn] : m_sockets)
      {
        if (IdLocked(other) == to->get<std::string>())
          m_sink.Text(other, other_conn, out);
      }
    }
    else
    {
      SendOthersLocked(pid, out);
    }
  }
  else if (t == "local-add" || t == "local-remove")
  {
    // Second players on one machine need the desktop app's seat controls; refused, as a room
    // without them does.
    m_sink.Text(pid, conn,
                Json({{"t", picojson::value("command-error")},
                      {"action", picojson::value(t)},
                      {"error", picojson::value("not-doubles")}}));
  }
  else if (t == "leave")
  {
    // A game that leaves is over: the host's ends the room (its friends' games hear it is gone),
    // a joiner's frees its seat for the next Start.
    if (m_launched.contains(pid) && m_launched.at(pid).seat == 0)
    {
      StopLocked();
      return;
    }
    ForgetLocked(pid);
    m_sink.Closed(pid, conn);
  }
  // Anything else (matchmaking, page commands, keep-alives) means nothing in a lobby's room.
}

void RoomServer::RemoveMemberLocked(u8 pid)
{
  if (!m_members.contains(pid))
    return;
  m_members.erase(pid);
  m_away.erase(pid);
  // Leave rosters go out before the member's socket is dropped from the list, so everyone else
  // hears it (RosterToOthersLocked skips `pid` itself).
  RosterToOthersLocked(pid, "leave", false);
}

void RoomServer::ForgetLocked(u8 pid)
{
  RemoveMemberLocked(pid);
  m_sockets.erase(pid);
  m_launched.erase(pid);
  m_joined.erase(pid);
  m_away.erase(pid);
  m_pending.erase(std::remove(m_pending.begin(), m_pending.end(), pid), m_pending.end());
}

void RoomServer::Close(u8 pid, u32 conn)
{
  std::lock_guard lock(m_mutex);
  const auto socket = m_sockets.find(pid);
  if (socket == m_sockets.end() || socket->second != conn)
    return;
  m_sockets.erase(socket);
  // A socket lost without a leave: the seat is held and the others hear the player is away.
  if (m_running && m_members.contains(pid))
  {
    m_away.insert(pid);
    RosterToOthersLocked(pid, "away", true);
  }
}

void RoomServer::PlayerLeftLobby(u8 pid)
{
  std::lock_guard lock(m_mutex);
  if (!m_running || !m_launched.contains(pid))
    return;
  if (m_launched.at(pid).seat == 0)
  {
    StopLocked();
    return;
  }
  RemoveMemberLocked(pid);
  if (const auto socket = m_sockets.find(pid); socket != m_sockets.end())
  {
    m_sink.Closed(pid, socket->second);
    m_sockets.erase(socket);
  }
  m_launched.erase(pid);
  m_pending.erase(std::remove(m_pending.begin(), m_pending.end(), pid), m_pending.end());
  m_joined.erase(pid);
}

void RoomServer::KfPut(u8 pid, u32 req, const std::string& room, const std::string& id,
                       std::vector<u8> data)
{
  std::lock_guard lock(m_mutex);
  if (!StoreRoomOk(room) || !m_launched.contains(pid) || !ValidKeyframeId(id))
  {
    m_sink.KfResult(pid, req, 403, Bytes(Error("forbidden")));
    return;
  }
  if (m_keyframes.contains(id))
  {
    m_sink.KfResult(pid, req, 409, Bytes(Json({{"code", picojson::value("exists")}})));
    return;
  }
  if (data.size() > MAX_KF_BYTES || m_keyframe_bytes + data.size() > MAX_KF_STORE_BYTES)
  {
    m_sink.KfResult(pid, req, 413, Bytes(Json({{"code", picojson::value("too_large")}})));
    return;
  }
  const double size = static_cast<double>(data.size());
  m_keyframe_bytes += data.size();
  m_keyframes.emplace(id, std::move(data));
  m_sink.KfResult(pid, req, 200,
                  Bytes(Json({{"id", picojson::value(id)}, {"size", picojson::value(size)}})));
}

void RoomServer::KfGet(u8 pid, u32 req, const std::string& room, const std::string& id)
{
  std::lock_guard lock(m_mutex);
  if (!StoreRoomOk(room) || !m_launched.contains(pid))
  {
    m_sink.KfResult(pid, req, 403, Bytes(Error("forbidden")));
    return;
  }
  const auto it = m_keyframes.find(id);
  if (it == m_keyframes.end())
  {
    m_sink.KfResult(pid, req, 404, Bytes(Json({{"code", picojson::value("gone")}})));
    return;
  }
  m_sink.KfResult(pid, req, 200, it->second);
}

void RoomServer::KfDelete(u8 pid, u32 req, const std::string& room, const std::string& id)
{
  std::lock_guard lock(m_mutex);
  if (!StoreRoomOk(room) || !m_launched.contains(pid))
  {
    m_sink.KfResult(pid, req, 403, Bytes(Error("forbidden")));
    return;
  }
  if (const auto it = m_keyframes.find(id); it != m_keyframes.end())
  {
    m_keyframe_bytes -= it->second.size();
    m_keyframes.erase(it);
  }
  m_sink.KfResult(pid, req, 200, Bytes("{}"));
}
}  // namespace Orca::Local
