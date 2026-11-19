// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/Local/LocalProto.h"

// The room a NetPlay lobby's host runs for its players' Subspace engines (LocalProto.h). It
// speaks the protocol of the service's friends rooms, as the existing room fixture
// (Tools/orca/doubles-rooms.py) does: rosters ("welcome", "join", "back", "away", "leave"),
// "msg" relayed to one player or all, "ping"/"pong", and the keyframe store. No matchmaking,
// ratings or accounts: who plays is who the lobby launched.
//
// Seats come from the lobby: the host is seat 0 (the engine's game host, port 1), then the other
// players with a GameCube port assigned, in port order, up to four. The host's engine is launched
// first and the others once it is in the room, as the fixture does, so the host always holds the
// game when friends arrive.
//
// Thread-safe. Sink calls are made with the room's lock held and must not call back in.
namespace Orca::Local
{
class RoomServer
{
public:
  struct Sink
  {
    virtual ~Sink() = default;
    virtual void OpenResult(u8 pid, u32 conn, u16 status, const std::string& body) = 0;
    virtual void Text(u8 pid, u32 conn, const std::string& text) = 0;
    virtual void Closed(u8 pid, u32 conn) = 0;
    virtual void KfResult(u8 pid, u32 req, u16 status, std::vector<u8> body) = 0;
    virtual void Launch(u8 pid, const LaunchInfo& launch) = 0;
  };

  struct Player
  {
    u8 pid = 0;
    std::string name;
  };

  explicit RoomServer(Sink& sink);

  // Opens a room for `players` (the lobby host first, at most four) and launches the host's engine.
  // While a room is open, launches only players not in it yet, into free seats (drop-in), and
  // returns how many were added. -1: no room could be opened (empty list).
  int Start(const std::vector<Player>& players);
  bool Running() const;
  std::string Code() const;
  // Lobby players with a seat in the room.
  std::vector<u8> LaunchedPids() const;
  // Closes the room for everyone.
  void Stop();

  // From the tunnel (the player's NetPlay connection names the player).
  void Open(u8 pid, u32 conn, const std::string& room, const std::string& ticket);
  void Text(u8 pid, u32 conn, const std::string& text);
  void Close(u8 pid, u32 conn);
  void KfPut(u8 pid, u32 req, const std::string& room, const std::string& id,
             std::vector<u8> data);
  void KfGet(u8 pid, u32 req, const std::string& room, const std::string& id);
  void KfDelete(u8 pid, u32 req, const std::string& room, const std::string& id);
  // The player left the lobby: it leaves the room too. The host leaving ends the room.
  void PlayerLeftLobby(u8 pid);

private:
  struct Seat
  {
    std::string name;
    int seat = -1;
  };

  std::string IdLocked(u8 pid) const { return TicketFor(pid); }
  std::string RosterLocked(u8 local, const std::string& kind, u8 changed, bool away_flag) const;
  void LaunchLocked(u8 pid, bool joining);
  void SendOthersLocked(u8 except, const std::string& text);
  void RosterToOthersLocked(u8 changed, const std::string& kind, bool away_flag);
  void RemoveMemberLocked(u8 pid);
  // Drops a joiner from this game entirely (seat, launch, socket), as if never launched.
  void ForgetLocked(u8 pid);
  void StopLocked();
  bool StoreRoomOk(const std::string& room) const { return m_running && room == m_code; }

  Sink& m_sink;
  mutable std::mutex m_mutex;
  bool m_running = false;
  std::string m_code;
  bool m_doubles = false;
  // Everyone the lobby launched, by NetPlay id.
  std::map<u8, Seat> m_launched;
  // Launched but waiting for the host's engine to be in the room.
  std::vector<u8> m_pending;
  // Players holding a seat in the room (from their first open until they leave).
  std::set<u8> m_members;
  // Each member's open WebSocket, by its gateway connection id.
  std::map<u8, u32> m_sockets;
  std::set<u8> m_joined;
  std::set<u8> m_away;
  std::map<std::string, std::vector<u8>> m_keyframes;
  u64 m_keyframe_bytes = 0;
};
}  // namespace Orca::Local
