// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>
#include "Common/CommonTypes.h"
#include "Common/MathUtil.h"

class AbstractTexture;
class AbstractFramebuffer;
class AbstractStagingTexture;

class BroadcastFrame
{
public:
  BroadcastFrame();
  ~BroadcastFrame();
  void Capture(const AbstractTexture* texture, const MathUtil::Rectangle<int>& rect,
               float aspect_ratio);
private:
  struct Readback
  {
    std::unique_ptr<AbstractStagingTexture> texture;
    u64 timestamp = 0;
  };
  std::unique_ptr<AbstractTexture> m_target;
  std::unique_ptr<AbstractFramebuffer> m_framebuffer;
  std::array<Readback, 3> m_readbacks;
  Readback* m_work = nullptr;
  const u8* m_pointer = nullptr;
  size_t m_stride = 0;
  u64 m_next = 0;
  std::atomic<int> m_job{0};
  std::atomic<bool> m_stop{false};
  std::thread m_worker;
};
