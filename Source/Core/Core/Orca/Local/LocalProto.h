// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace sf
{
class Packet;
}

// A rollback room carried over a Dolphin NetPlay lobby (no hosted service, no extra server).
//
// The lobby's host runs the room (RoomServer, inside NetPlayServer). Every player's Dolphin, the
// host's included, runs a Gateway on 127.0.0.1 that the Subspace engine treats as its "site"
// (ORCA_SITE, ORCA_LOCAL_ROOM=1): the engine's ticket, room WebSocket and keyframe store requests
// land there and travel to the room as MessageID::OrcaRoom packets over the lobby's own ENet
// connection, the one Dolphin made through its traversal server (host code) or by direct IP. So
// players connect exactly as for ordinary NetPlay.
//
// Every OrcaRoom packet is MessageID::OrcaRoom, then an Op, then the op's fields:
//   Open       client->host  u32 conn, string room, string ticket       (the engine's WebSocket)
//   OpenResult host->client  u32 conn, u16 status (101: open), string body
//   Text       both ways     u32 conn, string text                       (one room message)
//   Close      client->host  u32 conn                                    (the WebSocket closed)
//   Closed     host->client  u32 conn                                    (the room closed it)
//   KfPut      client->host  u32 req, string room, string id, u64 total, u64 offset, bytes...
//   KfGet      client->host  u32 req, string room, string id
//   KfDelete   client->host  u32 req, string room, string id
//   KfResult   host->client  u32 req, u16 status, u64 total, u64 offset, bytes...
//   Launch     host->client  string room, string ticket, u8 joining, u8 count, u8 pid...
// Keyframe bodies travel in parts of at most KF_PART_BYTES on the chunked-data channel, so a
// large one never holds up room messages; the receiver acts once offset + part reaches total.
namespace Orca::Local
{
enum class Op : u8
{
  Open = 1,
  OpenResult = 2,
  Text = 3,
  Close = 4,
  Closed = 5,
  KfPut = 6,
  KfGet = 7,
  KfDelete = 8,
  KfResult = 9,
  Launch = 10,
  Delay = 11,
};

constexpr u64 KF_PART_BYTES = 1 << 20;
// The engine's own cap (Session/Keyframe.cpp MAX_KEYFRAME_BYTES).
constexpr u64 MAX_KF_BYTES = 128ull << 20;
// Everything the host keeps for one room at once.
constexpr u64 MAX_KF_STORE_BYTES = 256ull << 20;
// A room message's cap. The largest an engine sends is a keyframe offer with an inline replay
// (Session/RelayRoom.h INLINE_KEYFRAME_MAX_TEXT), well under this.
constexpr size_t MAX_TEXT_BYTES = 1 << 20;
// Seats in a room (Session/Session.h MAX_SEATS).
constexpr int MAX_ROOM_SEATS = 4;

// Tell this player's Dolphin to start its engine in the room.
struct LaunchInfo
{
  std::string room;
  // The player's room identity, which is also its ticket ("np<pid>").
  std::string ticket;
  bool joining = false;
  // NetPlay ids of everyone launched into the room so far (for the link estimate in pongs).
  std::vector<u8> room_pids;
  u8 host_port = 0;
};

// The room identity of a lobby player.
std::string TicketFor(u8 pid);
// A fresh room code: 8 characters [a-z0-9], as online rooms use.
std::string NewRoomCode();
// Whether `code` is a room code the engine accepts (6-12 of [a-z0-9]).
bool ValidRoomCode(const std::string& code);
// Whether `id` is a keyframe id this store accepts (1-64 of [A-Za-z0-9_-]).
bool ValidKeyframeId(const std::string& id);

// One body arriving in parts. Feed every part; Complete() once all `total` bytes are in.
class PartAssembly
{
public:
  // False (and the body is dropped) when the part doesn't fit what came before or the cap.
  bool Add(u64 total, u64 offset, const u8* data, size_t size, u64 cap = MAX_KF_BYTES);
  bool Complete() const { return m_started && m_data.size() == m_total; }
  std::vector<u8> Take();

private:
  bool m_started = false;
  u64 m_total = 0;
  std::vector<u8> m_data;
};

// The bytes of `packet` from its read position to the end (a part's payload, always last).
std::vector<u8> RestOfPacket(const sf::Packet& packet);
const u8* RestOfPacketData(const sf::Packet& packet, size_t* size);
}  // namespace Orca::Local
