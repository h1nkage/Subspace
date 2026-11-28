// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <deque>
#include <memory>
#include <vector>

#include "Common/CommonTypes.h"

namespace Rollback
{
class UndoLog
{
public:
  UndoLog(std::size_t page_size, std::size_t page_count, u64 first_id = 1);
  UndoLog(const UndoLog&) = delete;
  UndoLog& operator=(const UndoLog&) = delete;

  std::size_t PageSize() const { return m_page_size; }
  std::size_t PageCount() const { return m_page_count; }

  u64 Open();
  bool Has(u64 id) const;
  u64 Newest() const { return m_logs.empty() ? 0 : m_logs.back().id; }
  std::size_t SnapshotCount() const { return m_logs.size(); }
  u64 NextId() const { return m_next_id; }

  bool LoggedInNewest(std::size_t page) const;
  const u8* NewestPreImage(std::size_t page) const
  {
    return LoggedInNewest(page) ? m_recorded_data[page] : nullptr;
  }
  bool Record(std::size_t page, const u8* contents);
  std::vector<u32> NewestPages() const;

  template <typename F>
  void ForEachPreImage(u64 id, F&& f) const
  {
    const auto first = FindLog(id);
    if (first == m_logs.end())
      return;
    const u32 mark = NextMark();
    for (auto log = first; log != m_logs.end(); ++log)
    {
      for (const Entry& entry : log->entries)
      {
        if (m_scratch[entry.page] == mark)
          continue;
        m_scratch[entry.page] = mark;
        f(static_cast<std::size_t>(entry.page), static_cast<const u8*>(entry.data));
      }
    }
  }

  bool RewindTo(u64 id);
  void Drop(u64 id);
  void Clear();

  std::size_t PagesHeld() const { return m_pages_held; }
  void Reserve(std::size_t pages);

private:
  struct Entry
  {
    u32 page;
    u8* data;
  };
  struct Log
  {
    u64 id;
    std::vector<Entry> entries;
  };

  std::deque<Log>::iterator FindLog(u64 id);
  std::deque<Log>::const_iterator FindLog(u64 id) const;
  u32 NextMark() const;
  u8* TakeBuffer();
  void FreeEntries(Log* log);

  std::size_t m_page_size;
  std::size_t m_page_count;
  std::deque<Log> m_logs;
  u64 m_next_id = 1;
  std::vector<u64> m_recorded_in;
  std::vector<u8*> m_recorded_data;
  mutable std::vector<u32> m_scratch;
  mutable u32 m_mark = 0;
  std::size_t m_pages_held = 0;
  std::vector<std::unique_ptr<u8[]>> m_blocks;
  std::vector<u8*> m_free;
};
}
