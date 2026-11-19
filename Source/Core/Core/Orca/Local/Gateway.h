// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Common/CommonTypes.h"

// What a player's own Subspace engine sees as its "site" when it plays in a NetPlay lobby's room
// (LocalProto.h): an HTTP and WebSocket server on 127.0.0.1 only. It answers
//   POST /api/multiplayer/ticket          itself (the lobby already named the room and seat);
//   GET  /room/<code>?ticket=...          as the room's WebSocket, carried to the lobby host;
//   PUT|GET|DELETE /api/orca/keyframes/<code>/<id>   by asking the lobby host's store.
// Only the engine it launched talks to it: requests must carry this launch's room and ticket.
namespace Orca::Local
{
class Gateway
{
public:
  struct Backend
  {
    virtual ~Backend() = default;
    virtual void Open(u32 conn, const std::string& room, const std::string& ticket) = 0;
    virtual void Text(u32 conn, const std::string& text) = 0;
    virtual void Close(u32 conn) = 0;
    virtual void KfPut(u32 req, const std::string& room, const std::string& id,
                       std::vector<u8> data) = 0;
    virtual void KfGet(u32 req, const std::string& room, const std::string& id) = 0;
    virtual void KfDelete(u32 req, const std::string& room, const std::string& id) = 0;
    // Added to the room's pongs before the engine sees them, in ms: the part of the real link to
    // the other players a pong from the lobby host doesn't cross (see NetPlayClient).
    virtual int PongDelayMs() = 0;
  };

  Gateway(Backend& backend, std::string room, std::string ticket);
  ~Gateway();
  Gateway(const Gateway&) = delete;
  Gateway& operator=(const Gateway&) = delete;

  // Listens on 127.0.0.1 (a free port) and starts serving.
  bool Start(std::string* error);
  u16 Port() const { return m_port; }
  // "http://127.0.0.1:<port>": the engine's ORCA_SITE.
  std::string Site() const;
  void Stop();

  // From the lobby host, on any thread.
  void OnOpenResult(u32 conn, u16 status, const std::string& body);
  void OnText(u32 conn, const std::string& text);
  void OnClosed(u32 conn);
  void OnKfResult(u32 req, u16 status, std::vector<u8> body);

private:
  struct Conn;
  void Run();
  void Accept();
  void Read(const std::shared_ptr<Conn>& conn);
  void HandleHttp(const std::shared_ptr<Conn>& conn);
  void HandleFrames(const std::shared_ptr<Conn>& conn);
  void Route(const std::shared_ptr<Conn>& conn, const std::string& method,
             const std::string& target, const std::map<std::string, std::string>& headers,
             std::vector<u8> body);
  std::shared_ptr<Conn> Find(u32 id);
  void Drop(const std::shared_ptr<Conn>& conn, bool tell_room);
  void TellRoomClosed(const std::shared_ptr<Conn>& conn);
  void FlushPongs();

  Backend& m_backend;
  const std::string m_room;
  const std::string m_ticket;
  u16 m_port = 0;
  std::atomic<bool> m_stop{false};
  std::thread m_thread;
  struct Listener;
  std::unique_ptr<Listener> m_listener;

  std::mutex m_conns_mutex;
  std::map<u32, std::shared_ptr<Conn>> m_conns;
  u32 m_next_conn = 1;

  struct DelayedPong
  {
    std::chrono::steady_clock::time_point due;
    u32 conn;
    std::string text;
  };
  std::mutex m_pongs_mutex;
  std::vector<DelayedPong> m_pongs;
};

// WebSocket helpers (exposed for tests).
std::string WebSocketAccept(const std::string& key);
std::string WebSocketFrame(u8 opcode, const std::string& payload);
}  // namespace Orca::Local
