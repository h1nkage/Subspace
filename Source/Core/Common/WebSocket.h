// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"

namespace Common
{
class WebSocket final
{
public:
  WebSocket();
  ~WebSocket();
  WebSocket(const WebSocket&) = delete;
  WebSocket& operator=(const WebSocket&) = delete;

  bool Connect(const std::string& url, std::chrono::milliseconds timeout, std::string* error,
               const std::function<bool()>& cancelled = {});

  bool SendText(std::string_view text, std::chrono::milliseconds timeout);

  std::optional<std::string> ReceiveText(std::chrono::milliseconds timeout);

  bool IsOpen() const;
  const std::string& Error() const;
  u16 CloseCode() const;
  std::string_view LossClass() const;
  std::optional<long> OsError() const;
  const std::string& EdgeColo() const;

  void Close(u16 code = 1000, std::chrono::milliseconds timeout = std::chrono::milliseconds(500));

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};
}
