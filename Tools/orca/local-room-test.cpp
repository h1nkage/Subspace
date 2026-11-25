// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Standalone end-to-end test of the NetPlay rollback room (Source/Core/Core/Orca/Local):
// RoomServer and Gateways with an in-memory tunnel in place of the NetPlay connection, driven by
// the engine's own libcurl WebSocket client (Common::WebSocket) and libcurl HTTP the way
// Session/Keyframe.cpp uses it. Not part of the CMake build. To run it on Linux, configure a build
// tree, build the `common` target, then compile this file with Orca/Local/*.cpp and
// Orca/SafeJson.cpp using the core target's flags and link libcommon, sfml-network, sfml-system,
// curl, mbedtls (tls, x509, crypto), fmt, zlib-ng, minizip-ng, zstd, liblzma, bzip2 and libiconv.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>
#include <picojson.h>

#include "Common/WebSocket.h"
#include "Core/Orca/Local/Gateway.h"
#include "Core/Orca/Local/LocalProto.h"
#include "Core/Orca/Local/RoomServer.h"

using namespace Orca::Local;
using namespace std::chrono_literals;

static int g_failures = 0;
#define CHECK(cond, ...)                                                                           \
  do                                                                                               \
  {                                                                                                \
    if (!(cond))                                                                                   \
    {                                                                                              \
      ++g_failures;                                                                                \
      std::printf("FAIL %s:%d: %s ", __FILE__, __LINE__, #cond);                                   \
      std::printf(__VA_ARGS__);                                                                    \
      std::printf("\n");                                                                           \
    }                                                                                              \
    else                                                                                           \
    {                                                                                              \
      std::printf("ok   %s\n", #cond);                                                             \
    }                                                                                              \
  } while (0)

struct Host;

struct Tunnel final : Gateway::Backend
{
  Host& host;
  u8 pid;
  int pong_delay = 0;
  Tunnel(Host& h, u8 p) : host(h), pid(p) {}
  void Open(u32 conn, const std::string& room, const std::string& ticket) override;
  void Text(u32 conn, const std::string& text) override;
  void Close(u32 conn) override;
  void KfPut(u32 req, const std::string& room, const std::string& id, std::vector<u8> data) override;
  void KfGet(u32 req, const std::string& room, const std::string& id) override;
  void KfDelete(u32 req, const std::string& room, const std::string& id) override;
  int PongDelayMs() override { return pong_delay; }
};

struct Host final : RoomServer::Sink
{
  RoomServer room{*this};
  std::map<u8, std::unique_ptr<Tunnel>> tunnels;
  std::map<u8, std::unique_ptr<Gateway>> gateways;
  std::map<u8, LaunchInfo> launches;

  void OpenResult(u8 pid, u32 conn, u16 status, const std::string& body) override
  {
    if (gateways.contains(pid))
      gateways[pid]->OnOpenResult(conn, status, body);
  }
  void Text(u8 pid, u32 conn, const std::string& text) override
  {
    if (gateways.contains(pid))
      gateways[pid]->OnText(conn, text);
  }
  void Closed(u8 pid, u32 conn) override
  {
    if (gateways.contains(pid))
      gateways[pid]->OnClosed(conn);
  }
  void KfResult(u8 pid, u32 req, u16 status, std::vector<u8> body) override
  {
    if (gateways.contains(pid))
      gateways[pid]->OnKfResult(req, status, std::move(body));
  }
  void Launch(u8 pid, const LaunchInfo& launch) override
  {
    launches[pid] = launch;
    tunnels[pid] = std::make_unique<Tunnel>(*this, pid);
    gateways[pid] = std::make_unique<Gateway>(*tunnels[pid], launch.room, launch.ticket);
    std::string error;
    if (!gateways[pid]->Start(&error))
      std::printf("gateway start failed: %s\n", error.c_str());
  }
};

void Tunnel::Open(u32 conn, const std::string& room, const std::string& ticket)
{
  host.room.Open(pid, conn, room, ticket);
}
void Tunnel::Text(u32 conn, const std::string& text)
{
  host.room.Text(pid, conn, text);
}
void Tunnel::Close(u32 conn)
{
  host.room.Close(pid, conn);
}
void Tunnel::KfPut(u32 req, const std::string& room, const std::string& id, std::vector<u8> data)
{
  host.room.KfPut(pid, req, room, id, std::move(data));
}
void Tunnel::KfGet(u32 req, const std::string& room, const std::string& id)
{
  host.room.KfGet(pid, req, room, id);
}
void Tunnel::KfDelete(u32 req, const std::string& room, const std::string& id)
{
  host.room.KfDelete(pid, req, room, id);
}

// ---- HTTP with libcurl, as Session/Keyframe.cpp does it ----
struct HttpResult
{
  long status = 0;
  std::string body;
};

static size_t Collect(char* data, size_t size, size_t count, void* user)
{
  static_cast<std::string*>(user)->append(data, size * count);
  return size * count;
}

static HttpResult Http(const std::string& method, const std::string& url,
                       const std::vector<std::string>& headers, const std::string* body)
{
  HttpResult result;
  CURL* curl = curl_easy_init();
  curl_slist* list = nullptr;
  for (const std::string& header : headers)
    list = curl_slist_append(list, header.c_str());
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &Collect);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 10000L);
  if (method == "POST")
  {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body->size()));
  }
  else if (method == "PUT")
  {
    // Like Keyframe.cpp: an upload with a known size (curl adds Expect: 100-continue past 1 MB).
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body->data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body->size()));
  }
  else if (method != "GET")
  {
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
  }
  const CURLcode code = curl_easy_perform(curl);
  if (code == CURLE_OK)
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
  else
    result.body = curl_easy_strerror(code);
  curl_slist_free_all(list);
  curl_easy_cleanup(curl);
  return result;
}

static picojson::object Parse(const std::string& text)
{
  picojson::value value;
  picojson::parse(value, text);
  return value.is<picojson::object>() ? value.get<picojson::object>() : picojson::object{};
}

static std::string Str(const picojson::object& o, const char* key)
{
  const auto it = o.find(key);
  return it != o.end() && it->second.is<std::string>() ? it->second.get<std::string>() : "";
}

// The next message of type `t` (others skipped), or empty.
static picojson::object Next(Common::WebSocket& ws, const std::string& t,
                             std::chrono::milliseconds timeout = 3000ms)
{
  const auto until = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < until)
  {
    const auto text = ws.ReceiveText(100ms);
    if (!text)
    {
      if (!ws.IsOpen())
        return {};
      continue;
    }
    picojson::object message = Parse(*text);
    if (Str(message, "t") == t)
      return message;
    std::printf("     (skipped %s)\n", text->substr(0, 80).c_str());
  }
  return {};
}

static std::string WsUrl(const Gateway& gateway, const std::string& room, const std::string& ticket)
{
  return "ws://127.0.0.1:" + std::to_string(gateway.Port()) + "/room/" + room +
         "?ticket=" + ticket;
}

static u64 NowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

int main()
{
  curl_global_init(CURL_GLOBAL_ALL);

  // PartAssembly
  {
    PartAssembly a;
    const u8 x[3] = {1, 2, 3};
    CHECK(a.Add(5, 0, x, 3) && !a.Complete(), "first part");
    CHECK(a.Add(5, 3, x, 2) && a.Complete(), "second part completes");
    CHECK(a.Take().size() == 5, "five bytes");
    PartAssembly b;
    CHECK(!b.Add(5, 2, x, 3), "must start at 0");
    PartAssembly c;
    CHECK(c.Add(4, 0, x, 3) && !c.Add(4, 3, x, 3), "overflow refused");
    PartAssembly empty;
    CHECK(empty.Add(0, 0, nullptr, 0) && empty.Complete(), "empty body completes");
  }

  Host host;
  CHECK(host.room.Start({{1, "Hinkage", 0}, {2, "Friend", 1}}, 0) == 2, "room opened for two");
  const std::string code = host.room.Code();
  CHECK(ValidRoomCode(code), "code %s", code.c_str());
  CHECK(host.gateways.contains(1) && !host.gateways.contains(2), "only the host launched first");
  CHECK(!host.launches[1].joining, "host is not joining");
  Gateway& gw1 = *host.gateways[1];

  // Ticket, as RelayRoom::Ticket posts it.
  {
    const std::string body = R"({"dev":"local","player_id":"orca-x","name":"Hinkage","players":4,)"
                             R"("mode":"orca-rsbe01","room":")" + code + R"(","lobby":{}})";
    const HttpResult r = Http("POST", gw1.Site() + "/api/multiplayer/ticket",
                              {"Content-Type: application/json"}, &body);
    const auto reply = Parse(r.body);
    CHECK(r.status == 200, "ticket status %ld %s", r.status, r.body.c_str());
    CHECK(Str(reply, "url") == gw1.Site(), "url %s", Str(reply, "url").c_str());
    CHECK(Str(reply, "ticket") == "np1", "ticket");
    CHECK(reply.count("direct") && !reply.at("direct").get<bool>(), "direct links off");
    const std::string wrong = R"({"room":"zzzzzzzz"})";
    CHECK(Http("POST", gw1.Site() + "/api/multiplayer/ticket", {}, &wrong).status == 403,
          "other room refused");
  }

  Common::WebSocket ws1;
  std::string error;
  CHECK(!ws1.Connect(WsUrl(gw1, code, "np2"), 3000ms, &error), "wrong ticket refused: %s",
        error.c_str());
  CHECK(ws1.Connect(WsUrl(gw1, code, "np1"), 3000ms, &error), "host connects: %s", error.c_str());
  auto welcome1 = Next(ws1, "welcome");
  CHECK(Str(welcome1, "me") == "np1" && Str(welcome1, "host") == "np1", "host welcome %s",
        picojson::value(welcome1).serialize().c_str());
  CHECK(Str(welcome1, "lifecycle") == "game", "lifecycle game");

  // The friend launches once the host is in.
  for (int i = 0; i < 50 && !host.gateways.contains(2); ++i)
    std::this_thread::sleep_for(10ms);
  CHECK(host.gateways.contains(2) && host.launches[2].joining, "friend launched, joining");
  CHECK(host.launches[2].room_pids.size() == 2, "launch lists the room");
  Gateway& gw2 = *host.gateways[2];

  Common::WebSocket ws2;
  CHECK(ws2.Connect(WsUrl(gw2, code, "np2"), 3000ms, &error), "friend connects: %s",
        error.c_str());
  auto welcome2 = Next(ws2, "welcome");
  CHECK(Str(welcome2, "me") == "np2" && Str(welcome2, "host") == "np1", "friend welcome");
  CHECK(welcome2["participants"].get<picojson::array>().size() == 2, "two participants");
  {
    const auto& p = welcome2["participants"].get<picojson::array>();
    CHECK(p[0].get("slot").get<double>() == 0 && p[1].get("slot").get<double>() == 1 &&
              p[1].get("name").get<std::string>() == "Friend",
          "seats by lobby order");
  }
  auto join = Next(ws1, "join");
  CHECK(!join.empty() && join["player"].get("id").get<std::string>() == "np2", "host hears join");

  // Relay: broadcast and addressed, small and large.
  CHECK(ws2.SendText(R"({"t":"msg","d":{"k":"hello","n":7}})", 1000ms), "send");
  auto msg = Next(ws1, "msg");
  CHECK(Str(msg, "from") == "np2" && msg["d"].get("n").get<double>() == 7, "broadcast relayed");
  const std::string big(100 * 1024, 'x');
  CHECK(ws1.SendText(R"({"t":"msg","to":"np2","d":{"k":"kf","b":")" + big + R"("}})", 1000ms),
        "send 100 KB");
  msg = Next(ws2, "msg");
  CHECK(Str(msg, "from") == "np1" && msg["d"].get("b").get<std::string>() == big,
        "100 KB message relayed intact");

  // Ping and the link estimate.
  host.tunnels[1]->pong_delay = 80;
  {
    const u64 sent = NowMs();
    ws1.SendText(R"({"t":"ping","at":)" + std::to_string(sent) + "}", 1000ms);
    auto pong = Next(ws1, "pong");
    const u64 rtt = NowMs() - sent;
    CHECK(!pong.empty() && static_cast<u64>(pong["at"].get<double>()) == sent, "pong echoes at");
    CHECK(rtt >= 80 && rtt < 400, "pong held back by the link estimate (%llu ms)",
          static_cast<unsigned long long>(rtt));
  }
  host.tunnels[1]->pong_delay = 0;

  // Keyframe store across players.
  {
    std::string blob(3 * 1024 * 1024 + 17, '\0');
    for (size_t i = 0; i < blob.size(); ++i)
      blob[i] = static_cast<char>((i * 131) ^ (i >> 7));
    const std::string base1 = gw1.Site() + "/api/orca/keyframes/" + code + "/";
    const std::string base2 = gw2.Site() + "/api/orca/keyframes/" + code + "/";
    const std::string id = "replay-1200-0123abcd";
    HttpResult r = Http("PUT", base1 + id,
                        {"Authorization: Ticket np1", "Content-Type: application/octet-stream"},
                        &blob);
    CHECK(r.status == 200, "put %ld %s", r.status, r.body.c_str());
    r = Http("PUT", base1 + id, {"Authorization: Ticket np1"}, &blob);
    CHECK(r.status == 409 && Str(Parse(r.body), "code") == "exists", "second put: exists");
    r = Http("GET", base2 + id, {"Authorization: Ticket np2"}, nullptr);
    CHECK(r.status == 200 && r.body == blob, "friend downloads the same %zu bytes (got %zu)",
          blob.size(), r.body.size());
    r = Http("GET", base2 + id, {"Authorization: Ticket np1"}, nullptr);
    CHECK(r.status == 403, "another player's ticket refused");
    r = Http("GET", base2 + "replay-5-ffffffff", {"Authorization: Ticket np2"}, nullptr);
    CHECK(r.status == 404 && Str(Parse(r.body), "code") == "gone", "missing: gone");
    r = Http("DELETE", base1 + id, {"Authorization: Ticket np1"}, nullptr);
    CHECK(r.status == 200, "delete");
    r = Http("GET", base2 + id, {"Authorization: Ticket np2"}, nullptr);
    CHECK(r.status == 404, "deleted");
  }

  // Socket lost without a leave: away, then back.
  ws2.Close();
  auto away = Next(ws1, "away");
  CHECK(!away.empty(), "host hears away");
  {
    bool flagged = false;
    for (const auto& p : away["players"].get<picojson::array>())
      flagged |= p.get("id").get<std::string>() == "np2" && p.get("away").is<bool>();
    CHECK(flagged, "roster marks the friend away");
  }
  Common::WebSocket ws2b;
  CHECK(ws2b.Connect(WsUrl(gw2, code, "np2"), 3000ms, &error), "friend reconnects");
  CHECK(!Next(ws2b, "welcome").empty(), "welcome again");
  CHECK(!Next(ws1, "back").empty(), "host hears back");

  // A third player dropping in mid-room.
  CHECK(host.room.Start({{1, "Hinkage", 0}, {2, "Friend", 1}, {3, "Third", 2}}, 0) == 1, "drop-in added");
  CHECK(host.gateways.contains(3) && host.launches[3].joining, "third launched at once");
  Common::WebSocket ws3;
  CHECK(ws3.Connect(WsUrl(*host.gateways[3], code, "np3"), 3000ms, &error), "third connects");
  auto welcome3 = Next(ws3, "welcome");
  CHECK(welcome3["participants"].get<picojson::array>().size() == 3, "three in the room");
  CHECK(!Next(ws1, "join").empty() && !Next(ws2b, "join").empty(), "both hear the third");

  // Leave.
  ws2b.SendText(R"({"t":"leave"})", 1000ms);
  auto leave = Next(ws1, "leave");
  CHECK(!leave.empty() && leave["players"].get<picojson::array>().size() == 2, "host hears leave");
  std::this_thread::sleep_for(200ms);
  CHECK(!ws2b.ReceiveText(300ms) && !ws2b.IsOpen(), "leaver's socket closed");

  // Co-seat commands are refused cleanly.
  ws3.SendText(R"({"t":"local-add"})", 1000ms);
  auto refused = Next(ws3, "command-error");
  CHECK(Str(refused, "error") == "not-doubles", "local-add refused");

  // The host quitting the lobby ends the room for everyone.
  host.room.PlayerLeftLobby(1);
  std::this_thread::sleep_for(200ms);
  (void)ws1.ReceiveText(300ms);
  (void)ws3.ReceiveText(300ms);
  CHECK(!ws1.IsOpen() && !ws3.IsOpen(), "room closed for everyone");
  CHECK(!host.room.Running(), "room stopped");
  Common::WebSocket late;
  CHECK(!late.Connect(WsUrl(gw1, code, "np1"), 3000ms, &error) &&
            error.find("host_gone") != std::string::npos,
        "a stopped room refuses as host_gone: %s", error.c_str());

  // Start again after games closed (the lobby's Start pressed again).
  {
    host.launches.clear();
    CHECK(host.room.Start({{1, "Hinkage", 0}, {2, "Friend", 1}}, 0) == 2, "a new room for two");
    const std::string code2 = host.room.Code();
    CHECK(code2 != code, "with a new code");
    Common::WebSocket h1;
    CHECK(h1.Connect(WsUrl(*host.gateways[1], code2, "np1"), 3000ms, &error), "host in: %s",
          error.c_str());
    CHECK(!Next(h1, "welcome").empty(), "host welcome");
    // Host::Launch records a launch before its gateway starts.
    const auto launched = [&host](u8 pid) {
      for (int i = 0; i < 50 && !host.launches.contains(pid); ++i)
        std::this_thread::sleep_for(10ms);
      std::this_thread::sleep_for(100ms);
      return host.launches.contains(pid);
    };
    CHECK(launched(2), "friend launched after the host");
    Common::WebSocket f1;
    CHECK(f1.Connect(WsUrl(*host.gateways[2], code2, "np2"), 3000ms, &error), "friend in: %s",
          error.c_str());
    CHECK(!Next(f1, "welcome").empty() && !Next(h1, "join").empty(), "both in");
    CHECK(host.room.Start({{1, "Hinkage", 0}, {2, "Friend", 1}}, 0) == 0, "both playing: nobody to add");

    // The friend's game leaves (window closed): Start launches it again, into the same room.
    f1.SendText(R"({"t":"leave"})", 1000ms);
    CHECK(!Next(h1, "leave").empty(), "host hears the friend leave");
    host.launches.clear();
    CHECK(host.room.Start({{1, "Hinkage", 0}, {2, "Friend", 1}}, 0) == 1, "friend relaunched");
    CHECK(launched(2) && host.launches[2].joining && host.launches[2].room == code2,
          "joining the same room");
    Common::WebSocket f2;
    CHECK(f2.Connect(WsUrl(*host.gateways[2], code2, "np2"), 3000ms, &error), "friend back in");
    CHECK(!Next(f2, "welcome").empty() && !Next(h1, "join").empty(), "joins as new");

    // The friend's game crashes (socket lost, no leave): Start launches it again too.
    f2.Close();
    CHECK(!Next(h1, "away").empty(), "host hears away");
    host.launches.clear();
    CHECK(host.room.Start({{1, "Hinkage", 0}, {2, "Friend", 1}}, 0) == 1, "crashed friend relaunched");
    CHECK(launched(2), "launched");
    Common::WebSocket f3;
    CHECK(f3.Connect(WsUrl(*host.gateways[2], code2, "np2"), 3000ms, &error), "in again");
    CHECK(!Next(f3, "welcome").empty(), "welcome again");

    // The host's game closes: the room ends, and Start opens a fresh one for everyone.
    h1.SendText(R"({"t":"leave"})", 1000ms);
    std::this_thread::sleep_for(200ms);
    (void)f3.ReceiveText(300ms);
    CHECK(!host.room.Running() && !f3.IsOpen(), "host's leave ends the room");
    host.launches.clear();
    CHECK(host.room.Start({{1, "Hinkage", 0}, {2, "Friend", 1}}, 0) == 2, "fresh room after the host left");
    CHECK(launched(1) && !host.launches[1].joining, "host launched first again");
    const std::string code3 = host.room.Code();
    Common::WebSocket h2;
    CHECK(h2.Connect(WsUrl(*host.gateways[1], code3, "np1"), 3000ms, &error), "host in");
    CHECK(!Next(h2, "welcome").empty(), "welcome");

    // ...and the same when the host's game dies without a leave.
    h2.Close();
    std::this_thread::sleep_for(200ms);
    host.launches.clear();
    CHECK(host.room.Start({{1, "Hinkage", 0}, {2, "Friend", 1}}, 0) == 2, "fresh room after a crash");
    CHECK(host.room.Code() != code3, "new code");
    host.room.Stop();
  }

  for (auto& [pid, gateway] : host.gateways)
    gateway->Stop();
  std::printf(g_failures ? "\n%d FAILED\n" : "\nALL PASSED\n", g_failures);
  return g_failures ? 1 : 0;
}
