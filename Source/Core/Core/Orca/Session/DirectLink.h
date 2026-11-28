// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <picojson.h>

#include "Common/CommonTypes.h"
#include "Core/Orca/Sandbox.h"

namespace Orca::Net
{
struct IceConfig
{
  std::string stun_host = "stun.cloudflare.com";
  u16 stun_port = 3478;
  std::string turn_host;
  u16 turn_port = 3478;
  std::string turn_user;
  std::string turn_pass;
  bool operator==(const IceConfig&) const = default;
};

bool ParseIce(const picojson::value& ice, IceConfig* out);

enum class LinkKind
{
  Relay = 0,
  Direct = 1,
  Turn = 2,
};

struct DirectStats
{
  LinkKind kind = LinkKind::Relay;
  int rtt_ms = -1;
  std::string pair;
  std::string test_local_address;
  int generation = 0;
  u64 sent = 0;
  u64 received = 0;
  u64 auth_drops = 0;
  u64 replays = 0;
  u64 floods = 0;
  int max_send_us = 0;
  int builds = 0;
  std::string reason;
};

struct DirectOptions
{
  std::string me;
  std::string room;
  IceConfig ice;
  std::chrono::steady_clock::time_point ice_minted = std::chrono::steady_clock::now();
  std::function<std::optional<IceConfig>()> fresh_ice;

  std::string force;
  bool loopback = false;
  int loss_percent = 0;
  bool blocked = false;
  Orca::Sandbox::NetMode net = Orca::Sandbox::NetMode::All;
  std::optional<int> net_delay_ms;
  int net_jitter_ms = 0;
  std::optional<std::pair<std::chrono::steady_clock::time_point,
                          std::chrono::steady_clock::time_point>> blackout;
};

std::optional<DirectOptions> DirectOptionsFromEnv();

class DirectLink
{
public:
  struct Received
  {
    std::string from;
    int slot = -1;
    std::string payload;
  };

  DirectLink();
  ~DirectLink();
  DirectLink(const DirectLink&) = delete;
  DirectLink& operator=(const DirectLink&) = delete;

  bool Start(DirectOptions options);
  void AddPeer(const std::string& id, int slot);
  void RemovePeer(const std::string& id);
  void OnSignal(const std::string& id, const picojson::object& message);
  std::vector<std::pair<std::string, picojson::object>> TakeSignals();
  void OnRelayPacket(const std::string& id);
  void SetIce(const IceConfig& ice);

  void Send(const std::string& payload, int seat = -1);
  std::vector<Received> Receive();
  std::optional<DirectStats> Stats(const std::string& id) const;
  void Test(const std::string& command);
  void Stop();

  static constexpr size_t MAX_DATAGRAM = 1200;
  static constexpr size_t HEADER_SIZE = 18;
  static constexpr size_t TAG_SIZE = 16;
  static constexpr size_t MAX_PAYLOAD = MAX_DATAGRAM - HEADER_SIZE - TAG_SIZE;

  struct Impl;

private:
  std::unique_ptr<Impl> m_impl;
};

std::string DescriptionFor(std::string_view description, Orca::Sandbox::NetMode net);

bool OneLan(const std::vector<std::string>& my_hosts, const std::vector<std::string>& their_hosts,
            const std::vector<std::string>& my_srflx, const std::vector<std::string>& their_srflx);

namespace DirectCrypto
{
using Key = std::array<u8, 32>;
std::pair<Key, Key> DeriveKeys(const Key& low_half, const Key& high_half, const std::string& room,
                               u16 generation, const std::string& low_id,
                               const std::string& high_id);

struct Header
{
  u8 kind = 1;
  u16 generation = 0;
  u64 counter = 0;
  u32 echo = 0;
  u16 hold = 0;
};
std::vector<u8> Seal(const Key& key, const Header& header, std::string_view payload);
std::optional<std::string> Open(const Key& key, const u8* data, size_t size, Header* header);
bool PeekHeader(const u8* data, size_t size, Header* header);
struct ReplayWindow
{
  u64 top = 0;
  u64 seen = 0;
  bool Fresh(u64 counter) const;
  void Take(u64 counter);
};
}
}
