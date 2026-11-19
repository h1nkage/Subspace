// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Local/LocalProto.h"

#include <algorithm>
#include <random>

#include <SFML/Network/Packet.hpp>

namespace Orca::Local
{
std::string TicketFor(u8 pid)
{
  return "np" + std::to_string(pid);
}

std::string NewRoomCode()
{
  static constexpr char ALPHABET[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  std::random_device device;
  std::uniform_int_distribution<int> pick(0, static_cast<int>(sizeof(ALPHABET)) - 2);
  std::string code;
  for (int i = 0; i < 8; ++i)
    code += ALPHABET[pick(device)];
  return code;
}

bool ValidRoomCode(const std::string& code)
{
  return code.size() >= 6 && code.size() <= 12 &&
         std::all_of(code.begin(), code.end(),
                     [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); });
}

bool ValidKeyframeId(const std::string& id)
{
  return !id.empty() && id.size() <= 64 &&
         std::all_of(id.begin(), id.end(), [](char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_';
         });
}

bool PartAssembly::Add(u64 total, u64 offset, const u8* data, size_t size, u64 cap)
{
  if (!m_started)
  {
    if (offset != 0 || total > cap)
      return false;
    m_started = true;
    m_total = total;
    m_data.reserve(static_cast<size_t>(total));
  }
  if (total != m_total || offset != m_data.size() || size > m_total - m_data.size())
  {
    m_started = false;
    m_data = {};
    return false;
  }
  m_data.insert(m_data.end(), data, data + size);
  return true;
}

std::vector<u8> PartAssembly::Take()
{
  m_started = false;
  m_total = 0;
  return std::move(m_data);
}

const u8* RestOfPacketData(const sf::Packet& packet, size_t* size)
{
  const size_t position = std::min(packet.getReadPosition(), packet.getDataSize());
  *size = packet.getDataSize() - position;
  if (*size == 0)
    return nullptr;
  return static_cast<const u8*>(packet.getData()) + position;
}

std::vector<u8> RestOfPacket(const sf::Packet& packet)
{
  size_t size = 0;
  const u8* data = RestOfPacketData(packet, &size);
  return std::vector<u8>(data, data + size);
}
}  // namespace Orca::Local
