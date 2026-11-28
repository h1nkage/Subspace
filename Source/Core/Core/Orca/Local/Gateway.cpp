// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Local/Gateway.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <iterator>

#include <SFML/Network/IpAddress.hpp>
#include <SFML/Network/SocketSelector.hpp>
#include <SFML/Network/TcpListener.hpp>
#include <SFML/Network/TcpSocket.hpp>
#include <fmt/format.h>
#include <picojson.h>

#include "Common/Crypto/SHA1.h"
#include "Common/Logging/Log.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/Orca/Local/LocalProto.h"
#include "Core/Orca/SafeJson.h"

namespace Orca::Local
{
namespace
{
// Requests and their bodies, and a WebSocket message (a keyframe offer with an inline replay is
// the largest at about 44 KB).
constexpr size_t MAX_HEADER_BYTES = 16 * 1024;
constexpr size_t MAX_FRAME_BYTES = MAX_TEXT_BYTES;
// The engine opens one room socket and a few store requests at a time.
constexpr size_t MAX_CONNECTIONS = 32;

std::string Base64(const u8* data, size_t size)
{
  static constexpr char TABLE[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  for (size_t i = 0; i < size; i += 3)
  {
    const u32 n = (u32(data[i]) << 16) | (i + 1 < size ? u32(data[i + 1]) << 8 : 0) |
                  (i + 2 < size ? u32(data[i + 2]) : 0);
    out += TABLE[(n >> 18) & 63];
    out += TABLE[(n >> 12) & 63];
    out += i + 1 < size ? TABLE[(n >> 6) & 63] : '=';
    out += i + 2 < size ? TABLE[n & 63] : '=';
  }
  return out;
}

std::string Lower(std::string text)
{
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

std::string Trim(const std::string& text)
{
  const size_t start = text.find_first_not_of(" \t");
  if (start == std::string::npos)
    return "";
  const size_t end = text.find_last_not_of(" \t\r");
  return text.substr(start, end - start + 1);
}

std::string PercentDecode(const std::string& text)
{
  std::string out;
  for (size_t i = 0; i < text.size(); ++i)
  {
    if (text[i] == '%' && i + 2 < text.size() && std::isxdigit(static_cast<u8>(text[i + 1])) &&
        std::isxdigit(static_cast<u8>(text[i + 2])))
    {
      out += static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16));
      i += 2;
    }
    else
    {
      out += text[i] == '+' ? ' ' : text[i];
    }
  }
  return out;
}

const char* Reason(u16 status)
{
  switch (status)
  {
  case 100:
    return "Continue";
  case 101:
    return "Switching Protocols";
  case 200:
    return "OK";
  case 400:
    return "Bad Request";
  case 403:
    return "Forbidden";
  case 404:
    return "Not Found";
  case 409:
    return "Conflict";
  case 411:
    return "Length Required";
  case 413:
    return "Payload Too Large";
  case 431:
    return "Request Header Fields Too Large";
  case 503:
    return "Service Unavailable";
  default:
    return "Error";
  }
}

std::string Response(u16 status, const std::string& content_type, const std::string& body)
{
  return fmt::format("HTTP/1.1 {} {}\r\nContent-Type: {}\r\nContent-Length: {}\r\n"
                     "Connection: close\r\nCache-Control: no-store\r\n\r\n",
                     status, Reason(status), content_type, body.size()) +
         body;
}

std::string JsonResponse(u16 status, const std::string& body)
{
  return Response(status, "application/json", body);
}
}  // namespace

std::string WebSocketAccept(const std::string& key)
{
  const auto digest =
      Common::SHA1::CalculateDigest(std::string_view(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
  return Base64(digest.data(), digest.size());
}

std::string WebSocketFrame(u8 opcode, const std::string& payload)
{
  std::string frame;
  frame += static_cast<char>(0x80 | (opcode & 0x0F));
  const u64 size = payload.size();
  if (size < 126)
  {
    frame += static_cast<char>(size);
  }
  else if (size <= 0xFFFF)
  {
    frame += static_cast<char>(126);
    frame += static_cast<char>((size >> 8) & 0xFF);
    frame += static_cast<char>(size & 0xFF);
  }
  else
  {
    frame += static_cast<char>(127);
    for (int shift = 56; shift >= 0; shift -= 8)
      frame += static_cast<char>((size >> shift) & 0xFF);
  }
  return frame + payload;
}

struct Gateway::Listener
{
  sf::TcpListener socket;
};

struct Gateway::Conn
{
  enum class State
  {
    Http,
    // Upgrade asked; the lobby host decides.
    WaitingOpen,
    WebSocket,
    // A keyframe request is with the lobby host.
    WaitingStore,
  };

  u32 id = 0;
  sf::TcpSocket socket;
  std::mutex write_mutex;
  std::atomic<State> state{State::Http};
  // Set once this end closed it; the loop then forgets it without telling the room.
  std::atomic<bool> dead{false};
  // The room has heard this socket is gone (or never needs to).
  std::atomic<bool> room_told{false};
  std::string in;
  bool continue_sent = false;
  std::string ws_key;
  std::string fragments;
  bool fragmenting = false;
  bool fragment_text = false;

  bool Write(const std::string& data)
  {
    std::lock_guard lock(write_mutex);
    if (dead || data.empty())
      return false;
    if (socket.send(data.data(), data.size()) != sf::Socket::Status::Done)
    {
      dead = true;
      socket.disconnect();
      return false;
    }
    return true;
  }

  void Kill()
  {
    std::lock_guard lock(write_mutex);
    dead = true;
    socket.disconnect();
  }

  void WriteAndClose(const std::string& data)
  {
    std::lock_guard lock(write_mutex);
    if (!dead && !data.empty())
      (void)socket.send(data.data(), data.size());
    dead = true;
    socket.disconnect();
  }
};

Gateway::Gateway(Backend& backend, std::string room, std::string ticket)
    : m_backend(backend), m_room(std::move(room)), m_ticket(std::move(ticket)),
      m_listener(std::make_unique<Listener>())
{
}

Gateway::~Gateway()
{
  Stop();
}

std::string Gateway::Site() const
{
  return fmt::format("http://127.0.0.1:{}", m_port);
}

bool Gateway::Start(std::string* error)
{
  if (m_listener->socket.listen(sf::Socket::AnyPort, sf::IpAddress::LocalHost) !=
      sf::Socket::Status::Done)
  {
    *error = "could not listen on 127.0.0.1";
    return false;
  }
  m_port = m_listener->socket.getLocalPort();
  m_stop = false;
  m_thread = std::thread([this] { Run(); });
  INFO_LOG_FMT(NETPLAY, "Orca local room: gateway for room {} on {}", m_room, Site());
  return true;
}

void Gateway::Stop()
{
  m_stop = true;
  if (m_thread.joinable())
    m_thread.join();
  m_listener->socket.close();
  std::map<u32, std::shared_ptr<Conn>> conns;
  {
    std::lock_guard lock(m_conns_mutex);
    conns.swap(m_conns);
  }
  for (auto& [id, conn] : conns)
  {
    if (conn->state == Conn::State::WebSocket)
      conn->WriteAndClose(WebSocketFrame(8, std::string("\x03\xE9", 2)));  // 1001: going away
    else
      conn->Kill();
  }
}

std::shared_ptr<Gateway::Conn> Gateway::Find(u32 id)
{
  std::lock_guard lock(m_conns_mutex);
  const auto it = m_conns.find(id);
  return it == m_conns.end() ? nullptr : it->second;
}

void Gateway::Run()
{
  sf::SocketSelector selector;
  while (!m_stop)
  {
    std::vector<std::shared_ptr<Conn>> conns, pruned;
    {
      std::lock_guard lock(m_conns_mutex);
      for (auto it = m_conns.begin(); it != m_conns.end();)
      {
        if (it->second->dead)
        {
          pruned.push_back(it->second);
          it = m_conns.erase(it);
          continue;
        }
        conns.push_back(it->second);
        ++it;
      }
    }
    // A socket that failed on a write from another thread: the room still thinks it's open.
    for (const auto& conn : pruned)
      TellRoomClosed(conn);
    selector.clear();
    selector.add(m_listener->socket);
    for (const auto& conn : conns)
      selector.add(conn->socket);
    if (selector.wait(sf::milliseconds(5)))
    {
      if (selector.isReady(m_listener->socket))
        Accept();
      for (const auto& conn : conns)
      {
        if (!conn->dead && selector.isReady(conn->socket))
          Read(conn);
      }
    }
    FlushPongs();
  }
}

void Gateway::Accept()
{
  auto conn = std::make_shared<Conn>();
  if (m_listener->socket.accept(conn->socket) != sf::Socket::Status::Done)
    return;
  conn->socket.setBlocking(true);
  std::lock_guard lock(m_conns_mutex);
  if (m_conns.size() >= MAX_CONNECTIONS)
  {
    conn->Kill();
    return;
  }
  conn->id = m_next_conn++;
  m_conns.emplace(conn->id, std::move(conn));
}

void Gateway::TellRoomClosed(const std::shared_ptr<Conn>& conn)
{
  const Conn::State state = conn->state;
  if ((state == Conn::State::WebSocket || state == Conn::State::WaitingOpen) &&
      !conn->room_told.exchange(true))
  {
    m_backend.Close(conn->id);
  }
}

void Gateway::Drop(const std::shared_ptr<Conn>& conn, bool tell_room)
{
  conn->Kill();
  {
    std::lock_guard lock(m_conns_mutex);
    m_conns.erase(conn->id);
  }
  if (tell_room)
    TellRoomClosed(conn);
  else
    conn->room_told = true;
}

void Gateway::Read(const std::shared_ptr<Conn>& conn)
{
  char buffer[64 * 1024];
  std::size_t received = 0;
  const sf::Socket::Status status = conn->socket.receive(buffer, sizeof(buffer), received);
  if (status != sf::Socket::Status::Done)
  {
    Drop(conn, true);
    return;
  }
  conn->in.append(buffer, received);
  switch (conn->state.load())
  {
  case Conn::State::Http:
    HandleHttp(conn);
    break;
  case Conn::State::WebSocket:
    HandleFrames(conn);
    break;
  default:
    // Nothing more is expected until the lobby host answers.
    if (conn->in.size() > MAX_HEADER_BYTES)
      Drop(conn, true);
    break;
  }
}

void Gateway::HandleHttp(const std::shared_ptr<Conn>& conn)
{
  const size_t header_end = conn->in.find("\r\n\r\n");
  if (header_end == std::string::npos)
  {
    if (conn->in.size() > MAX_HEADER_BYTES)
      conn->WriteAndClose(JsonResponse(431, "{}"));
    return;
  }
  const std::string head = conn->in.substr(0, header_end);
  size_t line_end = head.find("\r\n");
  const std::string request_line = head.substr(0, line_end);
  const size_t first_space = request_line.find(' ');
  const size_t second_space = request_line.find(' ', first_space + 1);
  if (first_space == std::string::npos || second_space == std::string::npos)
  {
    conn->WriteAndClose(JsonResponse(400, "{}"));
    return;
  }
  const std::string method = request_line.substr(0, first_space);
  const std::string target = request_line.substr(first_space + 1, second_space - first_space - 1);

  std::map<std::string, std::string> headers;
  while (line_end != std::string::npos && line_end < head.size())
  {
    const size_t next = head.find("\r\n", line_end + 2);
    const std::string line = head.substr(line_end + 2, next == std::string::npos ?
                                                           std::string::npos :
                                                           next - line_end - 2);
    if (const size_t colon = line.find(':'); colon != std::string::npos)
      headers[Lower(Trim(line.substr(0, colon)))] = Trim(line.substr(colon + 1));
    line_end = next;
  }

  if (headers.contains("transfer-encoding"))
  {
    conn->WriteAndClose(JsonResponse(411, "{}"));
    return;
  }
  u64 length = 0;
  if (const auto it = headers.find("content-length"); it != headers.end())
  {
    const std::string& text = it->second;
    if (text.empty() || text.size() > 12 ||
        !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; }))
    {
      conn->WriteAndClose(JsonResponse(400, "{}"));
      return;
    }
    length = std::stoull(text);
  }
  if (length > MAX_KF_BYTES)
  {
    conn->WriteAndClose(JsonResponse(413, R"({"code":"too_large"})"));
    return;
  }
  const size_t body_start = header_end + 4;
  if (conn->in.size() - body_start < length)
  {
    // libcurl waits for this before a large body.
    if (!conn->continue_sent && Lower(headers["expect"]) == "100-continue")
    {
      conn->continue_sent = true;
      conn->Write("HTTP/1.1 100 Continue\r\n\r\n");
    }
    return;
  }
  std::vector<u8> body(conn->in.begin() + body_start, conn->in.begin() + body_start + length);
  conn->in.erase(0, body_start + length);
  Route(conn, method, target, headers, std::move(body));
}

void Gateway::Route(const std::shared_ptr<Conn>& conn, const std::string& method,
                    const std::string& target, const std::map<std::string, std::string>& headers,
                    std::vector<u8> body)
{
  const size_t question = target.find('?');
  const std::string path = target.substr(0, question);
  const std::string query = question == std::string::npos ? "" : target.substr(question + 1);
  const auto header = [&](const char* name) {
    const auto it = headers.find(name);
    return it == headers.end() ? std::string() : it->second;
  };

  if (method == "POST" && path == "/api/multiplayer/ticket")
  {
    picojson::value request;
    if (!Json::ParseJson(std::string(body.begin(), body.end()), &request) ||
        !request.is<picojson::object>())
    {
      conn->WriteAndClose(JsonResponse(400, R"({"error":"bad request"})"));
      return;
    }
    const picojson::object& fields = request.get<picojson::object>();
    const auto room = fields.find("room");
    if (room != fields.end() && room->second.is<std::string>() &&
        room->second.get<std::string>() != m_room)
    {
      conn->WriteAndClose(
          JsonResponse(403, R"({"error":"This game was started for a different room"})"));
      return;
    }
    const picojson::object stun{{"urls", picojson::value("stun:stun.l.google.com:19302")}};
    const picojson::object reply{
        {"url", picojson::value(Site())},
        {"ticket", picojson::value(m_ticket)},
        {"direct", picojson::value(Config::Get(Config::NETPLAY_SUBSPACE_DIRECT_LINKS))},
        {"ice", picojson::value(picojson::array{picojson::value(stun)})}};
    conn->WriteAndClose(JsonResponse(200, picojson::value(reply).serialize()));
    return;
  }

  if (method == "GET" && path.starts_with("/room/"))
  {
    const std::string code = path.substr(6);
    std::string ticket;
    size_t start = 0;
    while (start <= query.size())
    {
      const size_t end = query.find('&', start);
      const std::string pair = query.substr(start, end == std::string::npos ? end : end - start);
      if (pair.starts_with("ticket="))
        ticket = PercentDecode(pair.substr(7));
      if (end == std::string::npos)
        break;
      start = end + 1;
    }
    const std::string key = header("sec-websocket-key");
    if (Lower(header("upgrade")) != "websocket" || key.empty())
    {
      conn->WriteAndClose(JsonResponse(400, R"({"error":"not a WebSocket upgrade"})"));
      return;
    }
    if (code != m_room || ticket != m_ticket)
    {
      conn->WriteAndClose(JsonResponse(403, R"({"error":"not this game's room"})"));
      return;
    }
    conn->ws_key = key;
    conn->state = Conn::State::WaitingOpen;
    m_backend.Open(conn->id, code, ticket);
    return;
  }

  constexpr std::string_view STORE = "/api/orca/keyframes/";
  if (path.starts_with(STORE))
  {
    const std::string rest = path.substr(STORE.size());
    const size_t slash = rest.find('/');
    const std::string code = rest.substr(0, slash);
    const std::string id = slash == std::string::npos ? "" : rest.substr(slash + 1);
    if (header("authorization") != "Ticket " + m_ticket || code != m_room ||
        !ValidKeyframeId(id))
    {
      conn->WriteAndClose(JsonResponse(403, R"({"error":"forbidden"})"));
      return;
    }
    if (method != "PUT" && method != "GET" && method != "DELETE")
    {
      conn->WriteAndClose(JsonResponse(404, "{}"));
      return;
    }
    conn->state = Conn::State::WaitingStore;
    if (method == "PUT")
      m_backend.KfPut(conn->id, code, id, std::move(body));
    else if (method == "GET")
      m_backend.KfGet(conn->id, code, id);
    else
      m_backend.KfDelete(conn->id, code, id);
    return;
  }

  conn->WriteAndClose(JsonResponse(404, R"({"error":"not found"})"));
}

void Gateway::HandleFrames(const std::shared_ptr<Conn>& conn)
{
  std::string& in = conn->in;
  while (!conn->dead)
  {
    if (in.size() < 2)
      return;
    const u8 b0 = static_cast<u8>(in[0]), b1 = static_cast<u8>(in[1]);
    const bool fin = b0 & 0x80;
    const u8 opcode = b0 & 0x0F;
    const bool masked = b1 & 0x80;
    u64 size = b1 & 0x7F;
    size_t at = 2;
    if (size == 126)
    {
      if (in.size() < 4)
        return;
      size = (u64(u8(in[2])) << 8) | u8(in[3]);
      at = 4;
    }
    else if (size == 127)
    {
      if (in.size() < 10)
        return;
      size = 0;
      for (int i = 0; i < 8; ++i)
        size = (size << 8) | u8(in[2 + i]);
      at = 10;
    }
    // A client must mask its frames (RFC 6455 5.1).
    if (!masked || size > MAX_FRAME_BYTES)
    {
      conn->WriteAndClose(WebSocketFrame(8, std::string("\x03\xEA", 2)));  // 1002
      Drop(conn, true);
      return;
    }
    if (in.size() < at + 4 + size)
      return;
    const u8 mask[4] = {u8(in[at]), u8(in[at + 1]), u8(in[at + 2]), u8(in[at + 3])};
    std::string payload = in.substr(at + 4, static_cast<size_t>(size));
    for (size_t i = 0; i < payload.size(); ++i)
      payload[i] = static_cast<char>(u8(payload[i]) ^ mask[i % 4]);
    in.erase(0, at + 4 + static_cast<size_t>(size));

    switch (opcode)
    {
    case 0:  // continuation
      if (!conn->fragmenting || conn->fragments.size() + payload.size() > MAX_FRAME_BYTES)
      {
        Drop(conn, true);
        return;
      }
      conn->fragments += payload;
      if (fin)
      {
        conn->fragmenting = false;
        if (conn->fragment_text)
          m_backend.Text(conn->id, conn->fragments);
        conn->fragments.clear();
      }
      break;
    case 1:  // text
    case 2:  // binary (unused by the room)
      if (fin)
      {
        if (opcode == 1)
          m_backend.Text(conn->id, payload);
      }
      else
      {
        conn->fragmenting = true;
        conn->fragment_text = opcode == 1;
        conn->fragments = std::move(payload);
      }
      break;
    case 8:  // close
      conn->WriteAndClose(WebSocketFrame(8, payload.substr(0, 2)));
      Drop(conn, true);
      return;
    case 9:  // ping
      conn->Write(WebSocketFrame(10, payload));
      break;
    default:  // pong, reserved
      break;
    }
  }
}

void Gateway::OnOpenResult(u32 id, u16 status, const std::string& body)
{
  const auto conn = Find(id);
  if (!conn || conn->state != Conn::State::WaitingOpen)
    return;
  if (status != 101)
  {
    conn->room_told = true;  // refused: nothing is open in the room
    conn->WriteAndClose(JsonResponse(status, body.empty() ? "{}" : body));
    return;
  }
  // Under the write lock, so a room message sent right after this can't overtake it. The state
  // changes first: the engine may answer the 101 before send() even returns here.
  std::lock_guard lock(conn->write_mutex);
  if (conn->dead)
    return;
  conn->state = Conn::State::WebSocket;
  const std::string reply = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                            "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
                            WebSocketAccept(conn->ws_key) + "\r\n\r\n";
  if (conn->socket.send(reply.data(), reply.size()) != sf::Socket::Status::Done)
  {
    conn->dead = true;
    conn->socket.disconnect();
  }
}

void Gateway::OnText(u32 id, const std::string& text)
{
  const auto conn = Find(id);
  if (!conn || conn->state != Conn::State::WebSocket)
    return;
  if (text.starts_with(R"({"t":"pong")"))
  {
    if (const int delay = m_backend.PongDelayMs(); delay > 0)
    {
      std::lock_guard lock(m_pongs_mutex);
      m_pongs.push_back(
          {std::chrono::steady_clock::now() + std::chrono::milliseconds(delay), id, text});
      return;
    }
  }
  conn->Write(WebSocketFrame(1, text));
}

void Gateway::FlushPongs()
{
  std::vector<DelayedPong> due;
  {
    std::lock_guard lock(m_pongs_mutex);
    if (m_pongs.empty())
      return;
    const auto now = std::chrono::steady_clock::now();
    const auto split = std::partition(m_pongs.begin(), m_pongs.end(),
                                      [&](const DelayedPong& pong) { return pong.due > now; });
    due.assign(std::make_move_iterator(split), std::make_move_iterator(m_pongs.end()));
    m_pongs.erase(split, m_pongs.end());
  }
  for (const DelayedPong& pong : due)
  {
    if (const auto conn = Find(pong.conn); conn && conn->state == Conn::State::WebSocket)
      conn->Write(WebSocketFrame(1, pong.text));
  }
}

void Gateway::OnClosed(u32 id)
{
  const auto conn = Find(id);
  if (!conn)
    return;
  conn->room_told = true;  // the room closed it
  if (conn->state == Conn::State::WebSocket)
    conn->WriteAndClose(WebSocketFrame(8, std::string("\x03\xE8", 2)));  // 1000
  else
    conn->Kill();
}

void Gateway::OnKfResult(u32 req, u16 status, std::vector<u8> body)
{
  const auto conn = Find(req);
  if (!conn || conn->state != Conn::State::WaitingStore)
    return;
  const bool json = status != 200 || body.empty() || body.front() == '{';
  // A GET's 200 is the keyframe itself; everything else the store's JSON.
  conn->WriteAndClose(Response(status, json ? "application/json" : "application/octet-stream",
                               std::string(body.begin(), body.end())));
}
}  // namespace Orca::Local
