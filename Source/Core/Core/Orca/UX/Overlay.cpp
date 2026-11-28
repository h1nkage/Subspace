// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/Overlay.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <imgui.h>

#include "Common/HookableEvent.h"
#include "Common/FileUtil.h"
#include "Common/StringUtil.h"
#include "Core/Config/SYSCONFSettings.h"
#include "Core/Core.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/UX/Kit.h"
#include "Core/Orca/UX/OrbCombo.h"
#include "Core/Orca/UX/QueueApp.h"
#include "Core/Orca/UX/RankedSteps.h"
#include "Core/Orca/UX/Search.h"
#include "Core/Orca/UX/SetEnd.h"
#include "Core/Orca/UX/UX.h"
#include "Core/Orca/UX/AppCard.h"
#include "Core/Orca/UX/AppOrbDraw.h"
#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/Present.h"

namespace Orca::UX
{
namespace
{
constexpr double STALL_WINDOW_MS = 3000;

constexpr u32 NOTICE_MS = 4000;
void Notice(std::string text)
{
  for (char& c : text)
  {
    if (!Common::IsPrintableCharacter(c))
      c = '?';
  }
  OSD::AddMessage(std::move(text), NOTICE_MS);
}

double NowMs()
{
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

template <typename T>
int StalledMs(const T& stats)
{
  if constexpr (requires { stats.stalled_ms; })
    return stats.stalled_ms;
  else
    return -1;
}
template <typename T>
int WaitingSeat(const T& stats)
{
  if constexpr (requires { stats.waiting_seat; })
    return stats.waiting_seat;
  else
    return -1;
}

std::string NameOf(const Events::PortInfo& p)
{
  return p.name.empty() ? fmt::format("Player {}", p.port + 1) : p.name;
}

constexpr std::size_t HUD_NAME_CHARS = 14;
std::size_t CharsEnd(std::string_view text, std::size_t n)
{
  std::size_t count = 0;
  for (std::size_t i = 0; i < text.size(); ++i)
  {
    if ((static_cast<unsigned char>(text[i]) & 0xC0) == 0x80)
      continue;
    if (count == n)
      return i;
    ++count;
  }
  return text.size();
}
std::string HudName(std::string_view name)
{
  const std::size_t cut = CharsEnd(name, HUD_NAME_CHARS);
  if (cut >= name.size())
    return std::string(name);
  std::string_view first = name.substr(0, cut);
  std::string_view rest = name.substr(cut);
  const std::size_t space = name.substr(0, CharsEnd(name, HUD_NAME_CHARS + 1)).rfind(' ');
  const bool at_space = space != std::string_view::npos && space > 0;
  if (at_space)
  {
    first = name.substr(0, space);
    rest = name.substr(space + 1);
  }
  while (!first.empty() && first.back() == ' ')
    first.remove_suffix(1);
  while (!rest.empty() && rest.front() == ' ')
    rest.remove_prefix(1);
  if (rest.empty())
    return std::string(first);
  const std::string_view second = rest.substr(0, CharsEnd(rest, HUD_NAME_CHARS));
  return std::string(first) + (at_space ? "" : "-") + "\n" + std::string(second);
}

Signal LevelOf(int fps, bool hitch, bool late)
{
  if (fps < OverlayModel::PERF_BAD_FPS || hitch)
    return Signal::Bad;
  if (fps < OverlayModel::PERF_OK_FPS || late)
    return Signal::Ok;
  return Signal::Good;
}

Signal Worse(Signal a, Signal b)
{
  return static_cast<int>(a) >= static_cast<int>(b) ? a : b;
}
}

LinkView ViewOf(const Events::LinkStats& stats)
{
  LinkView v;
  v.online = stats.online;
  v.delay = stats.delay;
  v.rollbacks_per_second = stats.rollbacks_per_second;
  v.deepest_rollback = stats.deepest_rollback;
  v.stalls = stats.stalls;
  v.transport = stats.link_rtt_ms >= 0 ? stats.transport : 0;
  v.round_trip_ms = v.transport > 0 ? stats.link_rtt_ms : stats.round_trip_ms;
  v.lamp_ms = stats.lamp_ms;
  v.seat_lamp_ms = stats.seat_lamp_ms;
  v.stalled_ms = StalledMs(stats);
  v.waiting_seat = WaitingSeat(stats);
  return v;
}

void OverlayModel::OnPorts(int frame, const std::vector<Events::PortInfo>& ports, double now_ms)
{
  (void)now_ms;
  std::vector<std::string> notices;
  std::unique_lock lk(m_lock);
  (void)frame;
  const auto find = [](const std::vector<Events::PortInfo>& list, int port) {
    return std::find_if(list.begin(), list.end(),
                        [port](const Events::PortInfo& p) { return p.port == port; });
  };
  const bool first = !m_seen_ports;
  m_seen_ports = true;
  for (const Events::PortInfo& p : ports)
  {
    if (!p.remote)
      continue;
    const auto was = find(m_ports, p.port);
    if (was != m_ports.end() && was->name == p.name && was->remote)
      continue;
    notices.push_back(first ? fmt::format("{} is in Port {}", NameOf(p), p.port + 1) :
                              fmt::format("{} joined in Port {}", NameOf(p), p.port + 1));
  }
  for (const Events::PortInfo& p : m_ports)
  {
    if (!p.remote)
      continue;
    const auto now = find(ports, p.port);
    if (now == ports.end() || now->name != p.name)
      notices.push_back(fmt::format("{} left", NameOf(p)));
  }
  m_ports = ports;
  lk.unlock();
  for (std::string& text : notices)
    Notice(std::move(text));
}

void OverlayModel::AddToast(std::string text, double now_ms)
{
  (void)now_ms;
  Notice(std::move(text));
}

void OverlayModel::AddChat(std::string name, std::string text, double now_ms)
{
  std::lock_guard lk(m_lock);
  m_chats.push_back({now_ms, std::move(name), std::move(text)});
  while (m_chats.size() > CHAT_KEEP)
    m_chats.pop_front();
}

void OverlayModel::Update(const LinkView& link, double now_ms)
{
  std::lock_guard lk(m_lock);
  m_link = link;
  m_stall_history.emplace_back(now_ms, link.stalls);
  while (m_stall_history.size() > 1 && m_stall_history.front().first < now_ms - STALL_WINDOW_MS)
    m_stall_history.pop_front();
  const bool added = m_last_stalls >= 0 && link.stalls > m_last_stalls;
  if (added)
  {
    const int seat = link.waiting_seat >= 0 ? link.waiting_seat : m_stall_seat;
    for (int s = 0; s < 4; ++s)
    {
      if (seat < 0 || seat >= 4 || s == seat)
        m_seat_stalls[s] += link.stalls - m_last_stalls;
    }
  }
  if (link.waiting_seat >= 0)
    m_stall_seat = link.waiting_seat;
  else if (link.stalled_ms <= 0 && !added)
    m_stall_seat = -1;
  m_last_stalls = link.stalls;
  m_seat_stall_history.emplace_back(now_ms, m_seat_stalls);
  while (m_seat_stall_history.size() > 1 &&
         m_seat_stall_history.front().first < now_ms - STALL_WINDOW_MS)
  {
    m_seat_stall_history.pop_front();
  }
  while (!m_toasts.empty() && m_toasts.front().first < now_ms - TOAST_MS)
    m_toasts.pop_front();
  while (!m_chats.empty() && m_chats.front().at < now_ms - CHAT_MS)
    m_chats.pop_front();
}

Signal OverlayModel::Quality(double) const
{
  const int recent_stalls = m_stall_history.empty() ?
                                0 :
                                m_stall_history.back().second - m_stall_history.front().second;
  const int rtt = m_link.round_trip_ms;
  if (m_link.stalled_ms > WAITING_AFTER_MS || rtt >= 150 || recent_stalls >= 10)
    return Signal::Bad;
  if (rtt >= 80 || recent_stalls > 0)
    return Signal::Ok;
  return Signal::Good;
}

namespace
{
int LampLevel(int ms, bool stalled_now, int recent_stalls)
{
  if (stalled_now || ms >= 150 || recent_stalls >= 10)
    return 1;
  if (ms >= 121 || recent_stalls > 0)
    return 2;
  if (ms >= 81)
    return 3;
  if (ms >= 40)
    return 4;
  return 5;
}
}

int OverlayModel::Lamp(double) const
{
  const int recent_stalls = m_stall_history.empty() ?
                                0 :
                                m_stall_history.back().second - m_stall_history.front().second;
  return LampLevel(m_link.lamp_ms, m_link.stalled_ms > WAITING_AFTER_MS, recent_stalls);
}

double OverlayModel::RunningMs(double now_ms) const
{
  return (m_paused ? m_paused_at : now_ms) - m_paused_ms;
}

void OverlayModel::OnFrame(double now_ms, bool shown, double game_held_ms, int game_held_frames)
{
  std::lock_guard lk(m_lock);
  if (!shown)
  {
    ++m_unshown;
    return;
  }
  const double now = RunningMs(now_ms);
  if (std::exchange(m_unshown, 0) > PERF_MAX_UNSHOWN)
  {
    m_frames.clear();
    m_first_frame = -1;
    m_last_frame = -1;
  }
  if (m_first_frame < 0)
  {
    if (m_last_frame >= 0)
      m_first_frame = now;
  }
  else
  {
    constexpr double frame_ms = 1000 / 59.94;
    const double ms = now - m_last_frame;
    const int held = std::max(0, game_held_frames);
    for (int k = 1; k <= held; ++k)
      m_frames.emplace_back(m_last_frame + ms * k / (held + 1), std::min(ms, frame_ms));
    m_frames.emplace_back(now, std::max(ms - game_held_ms, std::min(ms, frame_ms)));
  }
  m_last_frame = now;
  while (!m_frames.empty() && m_frames.front().first <= now - PERF_WINDOW_MS - PERF_HOLD_MS)
    m_frames.pop_front();
}

void OverlayModel::Pause(double now_ms)
{
  std::lock_guard lk(m_lock);
  if (m_paused)
    return;
  m_paused = true;
  m_paused_at = now_ms;
}

void OverlayModel::Resume(double now_ms)
{
  std::lock_guard lk(m_lock);
  if (!m_paused)
    return;
  m_paused = false;
  m_paused_ms += std::max(0.0, now_ms - m_paused_at);
}

void OverlayModel::Meter(double now_ms, Stats stats, Perf perf, Frame* f) const
{
  const double now = RunningMs(now_ms);
  if (perf == Perf::Off || m_first_frame < 0 || now < m_first_frame + PERF_WINDOW_MS)
    return;
  std::size_t first = 0;
  std::ptrdiff_t hitch = -1, late = -1;
  const auto level_at = [&](std::size_t end, double t) {
    while (first < end && m_frames[first].first <= t - PERF_WINDOW_MS)
      ++first;
    const auto oldest = static_cast<std::ptrdiff_t>(first);
    return LevelOf(static_cast<int>(end - first), hitch >= oldest, late >= oldest);
  };
  Signal level = Signal::Good;
  std::size_t end = 0;
  for (; end < m_frames.size() && m_frames[end].first <= now; ++end)
  {
    const auto [at, ms] = m_frames[end];
    if (ms > PERF_BAD_FRAME_MS)
      hitch = static_cast<std::ptrdiff_t>(end);
    if (ms > PERF_OK_FRAME_MS)
      late = static_cast<std::ptrdiff_t>(end);
    if (at > now - PERF_HOLD_MS && at >= m_first_frame + PERF_WINDOW_MS)
      level = Worse(level, level_at(end + 1, at));
  }
  level_at(end, now);
  const int fps = static_cast<int>(end - first);
  double longest_ms = std::max(0.0, now - m_last_frame);
  for (std::size_t i = first; i < end; ++i)
    longest_ms = std::max(longest_ms, m_frames[i].second);
  f->perf_level = Worse(level, LevelOf(fps, longest_ms > PERF_BAD_FRAME_MS,
                                       longest_ms > PERF_OK_FRAME_MS));
  f->perf = fmt::format("{} fps · {:.0f} ms", fps, longest_ms);
  f->perf_fps = fmt::format("{} fps", fps);
  f->perf_ms = fmt::format("{:.0f} ms", longest_ms);
  if (perf != Perf::Detailed)
    return;
  if (m_link.online && stats != Stats::Full)
    f->perf += fmt::format(" · delay {} · {:.1f} rb/s", m_link.delay, m_link.rollbacks_per_second);
  for (std::size_t i = 0; i < end; ++i)
  {
    if (m_frames[i].first > now - PERF_GRAPH_MS)
      f->graph.push_back(static_cast<float>(m_frames[i].second));
  }
}

OverlayModel::Frame OverlayModel::View(double now_ms, Stats stats, Perf perf) const
{
  std::lock_guard lk(m_lock);
  Frame f;
  f.show_signal = m_link.online;
  f.signal = Quality(now_ms);
  f.lamp = Lamp(now_ms);
  for (int seat = 0; seat < 4 && m_link.online; ++seat)
  {
    if (m_link.seat_lamp_ms[seat] < 0)
      continue;
    const int port = ::Orca::Online::SeatPort(seat);
    if (port < 0 || port >= 4)
      continue;
    const int recent = m_seat_stall_history.empty() ?
                           0 :
                           m_seat_stall_history.back().second[seat] -
                               m_seat_stall_history.front().second[seat];
    const bool stalled_now = m_link.stalled_ms > WAITING_AFTER_MS &&
                             (m_link.waiting_seat < 0 || m_link.waiting_seat == seat);
    f.port_lamps[port] = LampLevel(m_link.seat_lamp_ms[seat], stalled_now, recent);
    f.port_ms[port] = m_link.seat_lamp_ms[seat];
  }
  f.show_ping = ::Orca::Online::ShowPing();
  f.hud_names = ::Orca::Online::HudNames();
  f.corner_sprite = ::Orca::Online::ShowCornerSprite();
  f.corner_fps = ::Orca::Online::ShowCornerFps();
  f.frame_ms = ::Orca::Online::ShowFrameMs();
  f.corner_ping = ::Orca::Online::ShowCornerPing();
  for (const Events::PortInfo& p : m_ports)
  {
    if (!m_link.online || p.port < 0 || p.port >= 4)
      continue;
    if (p.remote)
    {
      const int seat = ::Orca::Online::PortSeat(p.port);
      if (seat < 0 || seat >= 4 || m_link.seat_lamp_ms[seat] < 0)
        continue;
    }
    f.port_names[p.port] = HudName(NameOf(p));
    if (!p.remote)
    {
      f.port_lamps[p.port] = f.lamp;
      f.port_ms[p.port] = m_link.lamp_ms;
    }
  }
  if (m_link.online && m_link.stalled_ms > WAITING_AFTER_MS)
  {
    std::string who = "your friend";
    for (const Events::PortInfo& p : m_ports)
    {
      if (m_link.waiting_seat >= 0 && p.port == ::Orca::Online::SeatPort(m_link.waiting_seat) && p.remote)
        who = NameOf(p);
    }
    f.waiting = fmt::format("Waiting for {}...", who);
  }
  for (const auto& [at, text] : m_toasts)
  {
    const double left = at + TOAST_MS - now_ms;
    if (left <= 0)
      continue;
    f.toasts.push_back(
        {text, static_cast<float>(std::clamp(left / TOAST_FADE_MS, 0.0, 1.0)), at});
  }
  for (const ChatEntry& c : m_chats)
  {
    const double left = c.at + CHAT_MS - now_ms;
    if (left <= 0)
      continue;
    f.chats.push_back(
        {c.name, c.text, static_cast<float>(std::clamp(left / TOAST_FADE_MS, 0.0, 1.0)), c.at});
  }
  if (stats != Stats::Off && m_link.online)
  {
    const int ms = m_link.lamp_ms >= 0 ? m_link.lamp_ms : m_link.round_trip_ms;
    const std::string ping = ms >= 0 ? fmt::format("{} ms", ms) : std::string("– ms");
    const char* path = m_link.transport == 1 ? " direct" : m_link.transport == 2 ? " TURN" : "";
    f.stats = stats == Stats::Full ? fmt::format("{}{} · delay {} · {:.1f} rb/s", ping, path,
                                                 m_link.delay, m_link.rollbacks_per_second) :
                                     ping;
  }
  Meter(now_ms, stats, perf, &f);
  return f;
}

namespace
{
std::mutex s_rules_lock;
std::string s_rules_line;
std::string s_rules_note;
OverlayModel s_overlay;
std::atomic<Stats> s_stats{Stats::Ping};
std::atomic<Perf> s_perf{Perf::Fps};
Common::EventHook s_state_hook;
std::atomic<bool> s_visible{true};
std::atomic<int> s_dim{0};
float s_dim_shown = 0;
double s_dim_at = -1;
std::optional<PictureArea> s_test_picture;
std::mutex s_rule_line_lock;
std::string s_rule_line;
std::mutex s_queue_lock;
std::string s_queue_line, s_queue_note;
std::array<std::string, 4> s_queue_labels;
std::array<bool, 4> s_queue_locked{};
bool s_demo = false;
double s_demo_start = -1;

LinkView DemoLink(double now)
{
  if (s_demo_start < 0)
  {
    s_demo_start = now;
    s_overlay.OnPorts(0, {{0, "bo", false}}, now);
    s_overlay.OnPorts(1, {{0, "bo", false}, {1, "ada", true}}, now);
  }
  const double t = std::fmod(now - s_demo_start, 12000);
  static int s_chat_round = -1;
  const int round = static_cast<int>((now - s_demo_start) / 12000);
  if (round != s_chat_round && t >= 1500)
  {
    s_chat_round = round;
    s_overlay.AddToast("ada joined on port 2", now);
    s_overlay.AddChat("ada", "gl hf!", now);
    s_overlay.AddChat("ada", "one more after this? I want a rematch on Battlefield", now);
  }
  LinkView v;
  v.online = true;
  v.delay = 2;
  v.rollbacks_per_second = t < 4000 ? 0.8 : 6.5;
  v.round_trip_ms = t < 4000 ? 35 : t < 8000 ? 95 : 180;
  v.stalled_ms = t >= 8000 ? static_cast<int>(t - 8000) : 0;
  v.waiting_seat = 1;
  return v;
}

ImU32 Rgba(int r, int g, int b, float a)
{
  return IM_COL32(r, g, b, static_cast<int>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255)));
}

ImU32 LevelColour(Signal level, float alpha)
{
  return level == Signal::Bad ? Rgba(255, 72, 64, alpha) :
         level == Signal::Ok  ? Rgba(255, 208, 64, alpha) :
                                Rgba(255, 255, 255, alpha);
}

void FrameGraph(ImDrawList* dl, ImVec2 pos, ImVec2 size, float unit,
                const std::vector<float>& frames)
{
  constexpr float TOP_MS = 50;
  constexpr float SLOTS = OverlayModel::PERF_GRAPH_MS * 60 / 1000;
  Kit::Plate(dl, pos, ImVec2(pos.x + size.x, pos.y + size.y), unit, Kit::Tone::Panel, 0.9f);
  const float pad = 0.9f * unit;
  pos = ImVec2(pos.x + pad, pos.y + pad);
  size = ImVec2(size.x - 2 * pad, size.y - 2 * pad);
  dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), Rgba(0, 0, 0, 0.45f),
                    0.4f * unit);
  const float slot = size.x / SLOTS;
  const float gap = slot >= 3 ? slot * 0.2f : 0;
  const float bottom = pos.y + size.y;
  float x = pos.x + size.x;
  for (auto it = frames.rbegin(); it != frames.rend() && x - slot >= pos.x - 0.5f; ++it)
  {
    const float ms = *it;
    const Signal level = ms > OverlayModel::PERF_BAD_FRAME_MS ? Signal::Bad :
                         ms > OverlayModel::PERF_OK_FRAME_MS  ? Signal::Ok :
                                                                Signal::Good;
    const float h = std::max(1.0f, std::min(ms, TOP_MS) / TOP_MS * size.y);
    dl->AddRectFilled(ImVec2(x - slot, bottom - h), ImVec2(x - gap, bottom),
                      level == Signal::Good ? Rgba(170, 220, 255, 0.85f) : LevelColour(level, 1));
    x -= slot;
  }
  const float period_y = bottom - 1000.0f / 60 / TOP_MS * size.y;
  dl->AddLine(ImVec2(pos.x, period_y), ImVec2(pos.x + size.x, period_y),
              Rgba(255, 255, 255, 0.4f), std::max(1.0f, size.y / 40));
}

float PanelCentre(int port)
{
  return 0.236f + 0.177f * static_cast<float>(port);
}

constexpr float NAME_TAB_Y = 0.575f;

Kit::Box PortraitRect(int port, ImVec2 pic, ImVec2 pic_size)
{
  const bool pplus = Kit::CurrentLook() == Kit::Look::ProjectPlus;
  const float cx = pic.x + pic_size.x * PanelCentre(port);
  const float half = pic_size.x * 0.078f;
  return {ImVec2(cx - half, pic.y + pic_size.y * (pplus ? 0.6f : 0.647f)),
          ImVec2(cx + half, pic.y + pic_size.y * (pplus ? 0.872f : 0.864f))};
}

constexpr float DOTS_Y = 1.9f;
constexpr float TOP_FIRST = 1.6f, TOP_UNDER_DOTS = 3.9f;

struct GameTimerPlace
{
  float x, y, size;
};
constexpr GameTimerPlace BAND_TIMER_BRAWL{0.927f, 0.6125f, 5.2f};
constexpr GameTimerPlace BAND_TIMER_PPLUS{0.922f, 0.567f, 5.2f};
constexpr GameTimerPlace LINE_TIMER_BRAWL{0.371f, 0.935f, 4.4f};
constexpr GameTimerPlace LINE_TIMER_PPLUS{0.414f, 0.064f, 5.4f};
constexpr float GUIDE_RIGHT = 0.975f, GUIDE_Y = 0.969f;

std::string Upper(std::string_view name)
{
  std::string out(name);
  for (char& c : out)
  {
    if (c >= 'a' && c <= 'z')
      c = static_cast<char>(c - 'a' + 'A');
  }
  return out;
}

void PictureRect(ImVec2* pic, ImVec2* pic_size)
{
  *pic = ImVec2(0, 0);
  *pic_size = ImGui::GetIO().DisplaySize;
  if (s_test_picture)
  {
    *pic = ImVec2(s_test_picture->x, s_test_picture->y);
    *pic_size = ImVec2(s_test_picture->w, s_test_picture->h);
  }
  else if (g_presenter)
  {
    const MathUtil::Rectangle<int>& r = g_presenter->GetTargetRectangle();
    if (r.GetWidth() > 0 && r.GetHeight() > 0)
    {
      *pic = ImVec2(static_cast<float>(r.left), static_cast<float>(r.top));
      *pic_size = ImVec2(static_cast<float>(r.GetWidth()), static_cast<float>(r.GetHeight()));
    }
  }
}

void DrawStageCursors(ImDrawList* dl, const Orca::Engine::Display& d, ImVec2 pic, ImVec2 pic_size,
                      float unit, double now_s)
{
  namespace StageCursors = Orca::Engine::Cursors;
  const StageCursors::View& v = d.cursors;
  const StageCursors::Layout* layout = d.layout;
  if (!layout)
    return;
  const auto at16 = [&](int x16, int y16) {
    return ImVec2(
        pic.x + pic_size.x * (layout->u0 + layout->du * static_cast<float>(x16) / 16.0f),
        pic.y + pic_size.y * (layout->v0 + layout->dv * static_cast<float>(y16) / 16.0f));
  };
  for (int port = 0; port < 2; ++port)
  {
    const StageCursors::View::Player& p = v.players[port];
    if (!p.present || !Ranked::Named(p.proposal))
      continue;
    const StageCursors::Rect* rect = d.proposals[port];
    if (p.proposal == Ranked::PREF_RANDOM)
      rect = layout->random_art.x0 < layout->random_art.x1 ? &layout->random_art :
                                                             &layout->random_button;
    if (!rect || rect->x0 >= rect->x1 || rect->y0 >= rect->y1)
      continue;
    const bool both = v.players[1 - port].present && v.players[1 - port].proposal == p.proposal;
    if (both && port == 1)
      continue;
    const ImVec2 c0 = at16(rect->x0, rect->y1), c1 = at16(rect->x1, rect->y0);
    Kit::Frame(dl, ImVec2(std::min(c0.x, c1.x), std::min(c0.y, c1.y)),
               ImVec2(std::max(c0.x, c1.x), std::max(c0.y, c1.y)), unit,
               both ? Kit::Tone::Gold : Kit::PortTone(port),
               both ? Upper(p.name) + " + " + Upper(v.players[1].name) : Upper(p.name), port == 0);
  }
  static std::array<StageCursors::TurnCue::Mode, 2> s_mode{};
  static std::array<u8, 2> s_kind{};
  static std::array<double, 2> s_since{};
  static double s_last = -1, s_opened = 0;
  if (s_last < 0 || now_s - s_last > 0.5)
  {
    s_mode = {};
    s_kind = {};
    s_opened = now_s;
  }
  s_last = now_s;
  std::array<StageCursors::TurnCue, 2> cues;
  for (int port = 0; port < 2; ++port)
  {
    cues[port] = d.cues[port];
    const bool acting = cues[port].mode == StageCursors::TurnCue::Mode::Acting;
    if (acting && (s_mode[port] != StageCursors::TurnCue::Mode::Acting || s_kind[port] != v.kind))
      s_since[port] = now_s;
    s_mode[port] = cues[port].mode;
    s_kind[port] = v.kind;
  }
  const float radius = 1.7f * std::abs(layout->du) * pic_size.x;
  for (int port = 0; port < 2; ++port)
  {
    const StageCursors::View::Player& p = v.players[port];
    if (p.present)
    {
      const Kit::PointerMode mode =
          cues[port].mode == StageCursors::TurnCue::Mode::Acting  ? Kit::PointerMode::Acting :
          cues[port].mode == StageCursors::TurnCue::Mode::Waiting ? Kit::PointerMode::Waiting :
                                                                    Kit::PointerMode::Plain;
      Kit::Pointer(dl, at16(p.x, p.y), radius, unit, port,
                   fmt::format("P{} {}", port + 1, Upper(p.name)), mode, port == 0, now_s,
                   s_since[port]);
    }
  }
  const ImVec2 clamp_min(pic.x + 1.0f * unit, pic.y + 1.0f * unit);
  const ImVec2 clamp_max(pic.x + pic_size.x - 1.0f * unit, pic.y + pic_size.y - 1.0f * unit);
  for (int port = 0; port < 2; ++port)
  {
    const StageCursors::View::Player& p = v.players[port];
    const StageCursors::TurnCue& cue = cues[port];
    if (!p.present || cue.mode == StageCursors::TurnCue::Mode::Idle)
      continue;
    if (cue.mode == StageCursors::TurnCue::Mode::Waiting)
    {
      Kit::TurnTag(dl, at16(p.x, p.y), radius, unit, cue.title, {}, Kit::Tone::Grey, -1, now_s,
                   port == 0, clamp_min, clamp_max, 0, 2.6f, true);
      continue;
    }
    const Kit::Blink blink = Kit::Attention(now_s, s_since[port]);
    const bool gold = cue.local && !v.both;
    Kit::TurnTag(dl, at16(p.x, p.y), radius, unit, cue.title, cue.detail,
                 gold ? Kit::Tone::Gold : Kit::PortTone(port), cue.seconds, now_s, port == 0,
                 clamp_min, clamp_max, cue.local ? blink.on : 0.0f);
  }
  const std::string& mk = d.mk_note;
  const double shown = now_s - s_opened;
  if (!mk.empty() && shown < 3.0)
  {
    const float alpha = static_cast<float>(std::clamp(std::min(shown / 0.15, (3.0 - shown) / 0.4),
                                                      0.0, 1.0));
    const float height = 9.0f * unit;
    const char* title = "META KNIGHT CLAUSE";
    const float width = std::min(pic_size.x - 4.0f * unit,
                                 Kit::BannerWidth(height, unit, title, mk) + 6.0f * unit);
    const float cx = pic.x + pic_size.x / 2;
    Kit::Banner(dl, cx - width / 2, cx + width / 2, pic.y + pic_size.y * 0.45f - height / 2,
                height, unit, Kit::Tone::Panel, title, mk, alpha);
  }
}

void DrawKitDemo()
{
  ImVec2 pic, pic_size;
  PictureRect(&pic, &pic_size);
  const float unit = OverlayUnit(pic_size.y);
  if (unit <= 0 || !(pic_size.x >= 1))
    return;
  Kit::DrawDemo(ImGui::GetForegroundDrawList(), pic, pic_size, unit, ImGui::GetTime());
}

int TakeTimer(std::string* line)
{
  static const std::string sep = " · ";
  const std::size_t at = line->rfind(sep);
  if (at == std::string::npos)
    return -1;
  const std::string tail = line->substr(at + sep.size());
  const std::size_t colon = tail.find(':');
  if (colon == std::string::npos || colon == 0 || tail.size() != colon + 3)
    return -1;
  int minutes = 0;
  for (std::size_t i = 0; i < colon; ++i)
  {
    if (tail[i] < '0' || tail[i] > '9')
      return -1;
    minutes = minutes * 10 + (tail[i] - '0');
  }
  if (tail[colon + 1] < '0' || tail[colon + 1] > '5' || tail[colon + 2] < '0' || tail[colon + 2] > '9')
    return -1;
  line->resize(at);
  return minutes * 60 + (tail[colon + 1] - '0') * 10 + (tail[colon + 2] - '0');
}

struct Scene
{
  OverlayModel::Frame f;
  SetEnd::View set_end;
  std::string search, search_note, rules, rules_note, hint;
  int band_seconds = -1;
  int line_seconds = -1;
  bool legend = false;
  std::array<std::string, 4> labels;
  std::array<bool, 4> locked{};
  Orca::Engine::Display display;
  int picker = -1;
  std::string picker_ribbon;
  Orca::Engine::SetDots dots;
  bool searching = false;

  bool Empty() const
  {
    return !f.show_signal && f.waiting.empty() && f.toasts.empty() && f.chats.empty() &&
           f.stats.empty() && f.perf.empty() && search.empty() && search_note.empty() &&
           rules.empty() && rules_note.empty() && hint.empty() &&
           std::all_of(labels.begin(), labels.end(),
                       [](const std::string& l) { return l.empty(); }) &&
           std::none_of(locked.begin(), locked.end(), [](bool l) { return l; }) &&
           !display.cursors.on &&
           picker < 0 && !dots.on && !searching && band_seconds < 0 && line_seconds < 0 &&
           set_end.Empty();
  }
};

Scene Gather(double now)
{
  Scene s;
  s.display = Game().Show();
  const Orca::Engine::Display& game = s.display;
  s_overlay.Update(s_demo ? DemoLink(now) : ViewOf(Events::GetLinkStats()), now);
  s.f = s_overlay.View(now, s_stats.load(std::memory_order_relaxed),
                       s_perf.load(std::memory_order_relaxed));
  s.set_end = SetEnd::Current().At(SetEnd::DemoFrame(now));
  if (s.set_end.hides_waiting)
    s.f.waiting.clear();
  std::tie(s.search, s.search_note) = Search::Lines();
  const bool joining = Search::Joining();
  {
    std::lock_guard lk(s_queue_lock);
    if (!joining && (!s_queue_line.empty() || !s_queue_note.empty()))
    {
      s.search = s_queue_line;
      s.search_note = s_queue_note;
    }
    s.labels = s_queue_labels;
    s.locked = s_queue_locked;
  }
  const bool band_clock = game.band_clock;
  const auto take = [](int* seconds, std::string* line) {
    const int t = TakeTimer(line);
    if (*seconds < 0)
      *seconds = t;
  };
  if (band_clock)
  {
    take(&s.band_seconds, &s.search);
    take(&s.band_seconds, &s.search_note);
  }
  for (int port = 0; port < 4; ++port)
  {
    if (game.plates & (1 << port))
      s.labels[port].clear();
  }
  if (game.rules_bar && !joining)
  {
    s.hint = QueueApp::OnSoloCss() ? std::string() : std::move(s.search_note);
    s.search.clear();
    s.search_note.clear();
  }
  if (QueueApp::OnSoloCss())
  {
    if (const std::string& tip = game.tip; !tip.empty())
      s.hint = s.hint.empty() ? tip : s.hint + " \u00b7 " + tip;
  }
  std::string rule_line;
  {
    std::lock_guard lk(s_rules_lock);
    s.rules = s_rules_line;
    s.rules_note = s_rules_note;
  }
  {
    std::lock_guard lk(s_rule_line_lock);
    rule_line = s_rule_line;
  }
  if (s.rules.empty() && s.rules_note.empty())
  {
    s.rules = game.ranked_set;
    s.rules_note = game.ranked_turn;
  }
  if (s.rules.empty())
    s.rules = game.score_line;
  if (s.rules_note.empty())
    s.rules_note = rule_line;
  if (band_clock)
  {
    take(&s.band_seconds, &s.rules);
    take(&s.band_seconds, &s.rules_note);
  }
  if (game.line_relabelled)
  {
    take(&s.line_seconds, &s.rules);
    take(&s.line_seconds, &s.rules_note);
    const auto join = [](std::string* to, const std::string& more) {
      if (!more.empty())
        *to = to->empty() ? more : *to + " \u00b7 " + more;
    };
    join(&s.hint, s.rules_note);
    if (s.rules.find("MK clause") != std::string::npos)
      join(&s.hint, "Meta Knight clause: any legal stage");
    s.rules.clear();
    s.rules_note.clear();
  }
  s.legend = game.legend;
  if (game.rules_bar || QueueApp::OnSoloCss())
  {
    s.rules.clear();
    s.rules_note.clear();
  }
  if (QueueApp::OnQueueCss())
  {
    s.picker = game.picker;
    if (s.picker >= 0)
      s.picker_ribbon = game.picker_ribbon;
  }
  if ((QueueApp::OnQueueCss() && !QueueApp::OnSoloCss()) || game.cursors.on)
    s.dots = game.dots;
  s.searching = QueueApp::OnSoloCss() && QueueApp::SoloReady();
  if (s.searching)
  {
    const std::string waiting = Search::WaitingLine();
    if (!waiting.empty())
      s.hint = s.hint.empty() ? waiting : waiting + " · " + s.hint;
  }
  return s;
}

constexpr std::array<std::array<float, 4>, 3> HUD_LAMP_X{{
    {0.3580f, 0.5976f, 0, 0},
    {0.2618f, 0.4780f, 0.6942f, 0},
    {0.1355f, 0.3631f, 0.5910f, 0.8069f},
}};
constexpr float HUD_LAMP_DX = -0.0237f;
constexpr float HUD_NAME_DX = -0.0088f;
constexpr float HUD_ROW_Y = 0.9405f;
constexpr float HUD_NAME_PX = 0.047f;
constexpr float HUD_NAME_PX2 = 0.035f;
constexpr float HUD_NAME_MAX_W = 0.18f;
constexpr float HUD_LAMP_R = 0.01815f;

constexpr const char* HUD_NAME_FONT = "Subspace Liberation Serif Bold";
ImFont* HudNameFont()
{
  static bool s_unusable = false;
  ImGuiIO& io = ImGui::GetIO();
  for (ImFont* font : io.Fonts->Fonts)
  {
    if (font && std::string_view(font->GetDebugName()) == HUD_NAME_FONT)
      return font;
  }
  if (!s_unusable && (io.BackendFlags & ImGuiBackendFlags_RendererHasTextures))
  {
    const std::string path = File::GetSysDirectory() + "Subspace/Fonts/LiberationSerif-Bold.ttf";
    ImFont* font = nullptr;
    if (File::Exists(path))
    {
      ImFontConfig config;
      config.Flags |= ImFontFlags_NoLoadError;
      std::snprintf(config.Name, sizeof(config.Name), "%s", HUD_NAME_FONT);
      font = io.Fonts->AddFontFromFileTTF(path.c_str(), 16.0f, &config);
    }
    s_unusable = font == nullptr;
    if (font)
      return font;
  }
  return ImGui::GetFont();
}

struct HudNameFit
{
  std::array<std::string_view, 2> lines;
  int count = 1;
  float size = 1;
  float width = 0;
};
HudNameFit FitHudName(std::string_view text, float px, float px2, float max_w)
{
  ImFont* font = HudNameFont();
  HudNameFit fit;
  fit.lines = {text, {}};
  if (const std::size_t split = text.find('\n'); split != std::string_view::npos)
  {
    fit.lines = {text.substr(0, split), text.substr(split + 1)};
    fit.count = 2;
  }
  const auto widest = [&](float size) {
    float w = 0;
    for (int i = 0; i < fit.count; ++i)
    {
      const std::string_view l = fit.lines[i];
      w = std::max(w, font->CalcTextSizeA(size, FLT_MAX, 0, l.data(), l.data() + l.size()).x);
    }
    return w;
  };
  fit.size = std::max(1.0f, fit.count == 1 ? px : px2);
  fit.width = widest(fit.size);
  if (fit.width > max_w && fit.width > 0)
  {
    fit.size = std::max(1.0f, fit.size * max_w / fit.width);
    fit.width = widest(fit.size);
  }
  return fit;
}

void HudNameText(ImDrawList* dl, float x, float cy, const HudNameFit& fit, float px2, float line1)
{
  ImFont* font = HudNameFont();
  const float o = std::max(1.0f, fit.size * 0.11f);
  for (int i = 0; i < fit.count; ++i)
  {
    const char* begin = fit.lines[i].data();
    const char* end = begin + fit.lines[i].size();
    const float h = font->CalcTextSizeA(fit.size, FLT_MAX, 0, begin, end).y;
    const float line_cy = fit.count == 1 ? cy : cy + (line1 + i) * std::max(1.0f, px2);
    const ImVec2 at(std::floor(x + 0.5f), std::floor(line_cy - h / 2 + 0.5f));
    static const std::string font_path =
        File::GetSysDirectory() + "Subspace/Fonts/LiberationSerif-Bold.ttf";
    if (Kit::OutlinedText(dl, font_path, fit.size, at, fit.lines[i], o, IM_COL32_WHITE))
      continue;
    for (int k = 0; k < 24; ++k)
    {
      const bool outer = k < 16;
      const float angle = (outer ? k / 16.0f : (k - 16) / 8.0f) * 6.2831853f;
      const float radius = outer ? o : o / 2;
      dl->AddText(font, fit.size,
                  ImVec2(at.x + radius * std::cos(angle), at.y + radius * std::sin(angle)),
                  IM_COL32(0, 0, 0, 255), begin, end);
    }
    dl->AddText(font, fit.size, at, IM_COL32_WHITE, begin, end);
  }
}

constexpr float CSS_ROW_Y = 0.968f;
constexpr float CSS_NAME_PX = 0.042f;
constexpr float CSS_NAME_PX2 = 0.022f;
constexpr float CSS_NAME_MAX_W = 0.133f;
constexpr float HUD_LAMP_GAP = 0.0065f;
constexpr float HUD_BELOW_ROW_Y = 0.971f;
constexpr float HUD_FIGHTER_NAME_Y = 0.9326f;
constexpr float HUD_BELOW_NAME_PX = 0.040f;
constexpr float HUD_SMALL_NAME_PX = 0.034f;
constexpr float HUD_BELOW_NAME_PX2 = 0.0245f;
constexpr float HUD_STROKE_LEAD = 0.0297f;
constexpr float HUD_STROKE_DY = 0.003f;
constexpr float HUD_BELOW_STROKE_DY = 0.002f;
constexpr float HUD_BELOW_STROKE_H = 0.052f;
constexpr float HUD_BELOW_STROKE_W = 0.185f;
constexpr float HUD_STROKE_H = 0.0813f;
constexpr float HUD_STROKE_W = 0.215f;
constexpr std::array<ImU32, 4> PORT_STROKE{IM_COL32(230, 40, 40, 255), IM_COL32(40, 90, 230, 255),
                                           IM_COL32(230, 190, 30, 255),
                                           IM_COL32(40, 170, 60, 255)};

constexpr float HUD_PING_PX = 0.040f;

constexpr float HUD_FADED = 0.75f;

void FadeSince(ImDrawList* dl, int first, float alpha)
{
  if (alpha >= 1.0f)
    return;
  const u32 scale = static_cast<u32>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
  for (int i = first; i < dl->VtxBuffer.Size; ++i)
  {
    ImU32& col = dl->VtxBuffer[i].col;
    const u32 a = (col >> IM_COL32_A_SHIFT) & 0xFF;
    col = (col & ~IM_COL32_A_MASK) | (((a * scale + 127) / 255) << IM_COL32_A_SHIFT);
  }
}

std::string PingText(int ms)
{
  return ms >= 0 ? fmt::format("{}", ms) : std::string("--");
}

std::string CornerStats(const OverlayModel::Frame& f)
{
  return f.corner_ping ? f.stats : std::string();
}

std::string CornerPerf(const OverlayModel::Frame& f)
{
  if (f.perf.empty())
    return {};
  if (f.corner_fps && f.frame_ms)
    return f.perf;
  if (f.corner_fps)
    return f.perf_fps;
  if (f.frame_ms)
    return f.perf_ms;
  return {};
}

bool CornerLamp(const OverlayModel::Frame& f)
{
  return f.show_signal && f.corner_sprite;
}

float DrawCorner(ImDrawList* dl, const OverlayModel::Frame& f, ImVec2 at, float unit)
{
  constexpr float SIZE = 2.6f;
  const float line_h = Kit::LabelLook(unit, SIZE).px;
  const std::string stats = f.show_signal ? CornerStats(f) : std::string();
  const std::string perf = CornerPerf(f);
  float y = at.y;
  if (CornerLamp(f))
  {
    const float r = 3.0f * unit;
    const ImVec2 c(at.x + r, at.y + r);
    Kit::Connection(dl, c, r, unit, f.lamp);
    const int lines = (stats.empty() ? 0 : 1) + (perf.empty() ? 0 : 1);
    float ty = c.y - static_cast<float>(lines) * line_h / 2;
    const float tx = c.x + r + 0.9f * unit;
    if (!stats.empty())
      ty = Kit::Label(dl, ImVec2(tx, ty), unit, stats, SIZE).max.y;
    if (!perf.empty())
      ty = Kit::Label(dl, ImVec2(tx, ty), unit, perf, SIZE, Kit::Align::Left,
                      LevelColour(f.perf_level, 1))
               .max.y;
    y = std::max(c.y + r, ty) + 1.2f * unit;
  }
  else if (!stats.empty() || !perf.empty())
  {
    float ty = at.y;
    if (!stats.empty())
      ty = Kit::Label(dl, ImVec2(at.x, ty), unit, stats, SIZE).max.y;
    if (!perf.empty())
      ty = Kit::Label(dl, ImVec2(at.x, ty), unit, perf, SIZE, Kit::Align::Left,
                      LevelColour(f.perf_level, 1))
               .max.y;
    y = ty + 0.8f * unit;
  }
  if (!f.graph.empty())
  {
    const ImVec2 size(40 * unit, 9 * unit);
    FrameGraph(dl, ImVec2(at.x, y), size, unit, f.graph);
    y += size.y + 1.2f * unit;
  }
  return y;
}

float CornerWidth(const OverlayModel::Frame& f, float unit)
{
  constexpr float SIZE = 2.6f;
  const Kit::TextLook look = Kit::LabelLook(unit, SIZE);
  float text = 0;
  const std::string stats = f.show_signal ? CornerStats(f) : std::string();
  const std::string perf = CornerPerf(f);
  if (!stats.empty())
    text = std::max(text, Kit::Measure(look, stats).x);
  if (!perf.empty())
    text = std::max(text, Kit::Measure(look, perf).x);
  float w = CornerLamp(f) ? 2 * 3.0f * unit + 0.9f * unit + text : text;
  if (!f.graph.empty())
    w = std::max(w, 40 * unit);
  return w;
}

std::string FitNote(const Kit::TextLook& look, const std::string& note, float width)
{
  static const std::string sep = " \u00b7 ";
  std::vector<std::string> parts;
  for (std::size_t at = 0;;)
  {
    const std::size_t next = note.find(sep, at);
    parts.push_back(note.substr(at, next == std::string::npos ? std::string::npos : next - at));
    if (next == std::string::npos)
      break;
    at = next + sep.size();
  }
  std::vector<std::string> lines;
  for (const std::string& part : parts)
  {
    const std::string longer = lines.empty() ? part : lines.back() + sep + part;
    if (!lines.empty() && Kit::Measure(look, longer).x <= width)
      lines.back() = longer;
    else if (Kit::Measure(look, part).x <= width)
      lines.push_back(part);
    else
      for (std::string& l : Kit::Wrap(look, part, width))
        lines.push_back(std::move(l));
  }
  std::string out;
  for (const std::string& l : lines)
    out += (out.empty() ? "" : "\n") + l;
  return out;
}

std::pair<std::string, float> FitBanner(const std::string& title, const std::string& note,
                                        float row, float width, float unit)
{
  if (note.empty() || Kit::BannerWidth(row, unit, title, note) <= width)
    return {note, row};
  const std::string fitted =
      FitNote(Kit::InkLook(unit, row * 0.3f / unit), note, width - row * 0.6f - 4.0f * unit);
  const auto lines = static_cast<std::size_t>(std::count(fitted.begin(), fitted.end(), '\n') + 1);
  return {fitted, Kit::BannerHeight(row, lines)};
}

Kit::Box DrawTopCentre(ImDrawList* dl, const Scene& s, ImVec2 pic, ImVec2 pic_size, ImVec2 vis,
                       ImVec2 vis_size, float unit, double now_s, float first_top,
                       std::string* guide, float right_edge)
{
  *guide = s.hint;
  Kit::Box drawn{ImVec2(vis.x + vis_size.x, vis.y), ImVec2(vis.x, vis.y)};
  const auto add = [&](const Kit::Box& b) {
    drawn.min.x = std::min(drawn.min.x, b.min.x);
    drawn.max.y = std::max(drawn.max.y, b.max.y);
  };
  std::vector<std::pair<std::string, std::string>> groups;
  for (const auto& [title, note] : {std::pair{s.search, s.search_note},
                                    std::pair{s.rules, s.rules_note}})
  {
    if (!title.empty())
      groups.emplace_back(title, note);
    else if (!note.empty())
      groups.emplace_back(note, std::string());
  }
  int seconds = -1;
  for (auto& [title, note] : groups)
  {
    if (seconds < 0)
      seconds = TakeTimer(&title);
    if (seconds < 0)
      seconds = TakeTimer(&note);
  }
  if (Kit::CurrentLook() == Kit::Look::ProjectPlus && QueueApp::OnQueueCss() && !groups.empty())
  {
    std::string title = groups[0].first, note, hint = s.hint;
    if (!s.search.empty())
    {
      note = s.rules;
      const std::string& more = s.search_note.empty() ? s.rules_note : s.search_note;
      hint = hint.empty() ? more : more.empty() ? hint : hint + " \u00b7 " + more;
    }
    else
    {
      note = s.rules_note;
    }
    TakeTimer(&note);
    TakeTimer(&hint);
    const float left = pic.x + pic_size.x * 0.4f;
    const float right = std::min(pic.x + pic_size.x * 0.99f, right_edge);
    const float top = pic.y + pic_size.y * 0.056f;
    const auto [fitted, h] = FitBanner(title, note, pic_size.y * 0.092f, right - left, unit);
    add(Kit::Banner(dl, left, right, top, h, unit, Kit::Tone::Panel, title, fitted));
    const float x = right - 1.5f * unit;
    const float y = top + h + 1.0f * unit;
    if (seconds >= 0)
      add(Kit::Timer(dl, ImVec2(x - 5.5f * unit, y + 2.4f * unit), unit, seconds, now_s));
    *guide = hint;
    return drawn;
  }
  const float cx = vis.x + vis_size.x / 2;
  float top = first_top;
  for (std::size_t i = 0; i < groups.size(); ++i)
  {
    const auto& [title, note] = groups[i];
    const float row =
        i == 0 ? (note.empty() ? 6.4f : 8.6f) * unit : (note.empty() ? 5.0f : 7.0f) * unit;
    const float max_w = std::max(0.62f * vis_size.x, 26.0f * unit);
    const auto [fitted, h] = FitBanner(title, note, row, max_w, unit);
    const float w = std::clamp(Kit::BannerWidth(h, unit, title, fitted), 26.0f * unit, max_w);
    const Kit::Box b = Kit::Banner(dl, cx - w / 2, cx + w / 2, top, h, unit, Kit::Tone::Panel,
                                   title, fitted);
    add(b);
    if (i == 0 && seconds >= 0)
    {
      add(Kit::Timer(dl, ImVec2(b.max.x + 6.0f * unit, (b.min.y + b.max.y) / 2), unit, seconds,
                     now_s));
    }
    top = b.max.y + 1.0f * unit;
  }
  return drawn;
}

void DrawScene()
{
  Kit::BeginFrame();
  const double now = NowMs();
  const Scene s = Gather(now);
  if (Kit::DemoMode() == 1)
  {
    DrawKitDemo();
    return;
  }
  AppCard::Warm();
  const AppOrb::Model::View orb_view = AppOrb::HostView(now);
  if (!orb_view.on)
    OSD::SetObscuredPixelsTop(0);
  if (s.Empty() && !orb_view.on && !orb_view.showing)
    return;
  ImVec2 pic, pic_size;
  PictureRect(&pic, &pic_size);
  const float unit = OverlayUnit(pic_size.y);
  if (unit <= 0 || !(pic_size.x >= 1))
    return;
  const PictureArea visible = VisiblePicture({pic.x, pic.y, pic_size.x, pic_size.y},
                                      ImGui::GetIO().DisplaySize.x, ImGui::GetIO().DisplaySize.y);
  const ImVec2 vis(visible.x, visible.y), vis_size(visible.w, visible.h);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const double now_s = ImGui::GetTime();
  const OverlayModel::Frame& f = s.f;
  if (s.display.cursors.on)
    DrawStageCursors(dl, s.display, pic, pic_size, unit, now_s);
  static int s_picker_was = -1;
  static double s_picker_since = 0;
  if (s.picker != s_picker_was)
  {
    s_picker_was = s.picker;
    s_picker_since = now_s;
  }
  for (int port = 0; port < 4; ++port)
  {
    const float centre = pic.x + pic_size.x * PanelCentre(port);
    if (!s.labels[port].empty())
    {
      Kit::LabelPlate(dl, ImVec2(centre, pic.y + pic_size.y * NAME_TAB_Y), unit, s.labels[port],
                      2.8f, Kit::Align::Centre, Kit::PortTone(port));
    }
    const Kit::Box portrait = PortraitRect(port, pic, pic_size);
    if (s.locked[port])
    {
      Kit::PortraitFrame(dl, portrait.min, portrait.max, unit, Kit::Tone::Gold);
      Kit::Ribbon(dl, ImVec2(portrait.min.x + 1.9f * unit, portrait.min.y + 1.4f * unit),
                  ImVec2(portrait.max.x - 1.9f * unit, portrait.min.y + 5.8f * unit), unit,
                  "LOCKED IN", Kit::Tone::Gold);
    }
    else if (port == s.picker)
    {
      const Kit::Blink blink = Kit::Attention(now_s, s_picker_since);
      const float g = 0.9f * unit;
      dl->AddRect(ImVec2(portrait.min.x - g, portrait.min.y - g),
                  ImVec2(portrait.max.x + g, portrait.max.y + g),
                  IM_COL32(0, 0, 0, static_cast<int>(115 * blink.on)), 1.2f * unit, 0,
                  0.9f * unit);
      dl->AddRect(ImVec2(portrait.min.x - g, portrait.min.y - g),
                  ImVec2(portrait.max.x + g, portrait.max.y + g),
                  IM_COL32(255, 255, 255, static_cast<int>(255 * blink.on)), 1.2f * unit, 0,
                  0.55f * unit);
      Kit::PortraitFrame(dl, portrait.min, portrait.max, unit, Kit::PortTone(port));
      if (!s.picker_ribbon.empty())
      {
        Kit::Ribbon(dl, ImVec2(portrait.min.x + 1.9f * unit, portrait.min.y + 1.4f * unit),
                    ImVec2(portrait.max.x - 1.9f * unit, portrait.min.y + 5.8f * unit), unit,
                    s.picker_ribbon, Kit::PortTone(port));
      }
    }
  }
  if (s.searching)
  {
    const Kit::Box seat = PortraitRect(1, pic, pic_size);
    Kit::SearchRing(dl, ImVec2((seat.min.x + seat.max.x) / 2, (seat.min.y + seat.max.y) / 2),
                    pic_size.y * 0.072f, unit, now_s);
  }
  if (s.dots.on)
  {
    Kit::SetDots(dl, ImVec2(pic.x + pic_size.x / 2, pic.y + DOTS_Y * unit), unit, s.dots.games,
                 s.dots.current);
  }
  if (f.show_signal && Kit::CurrentLook() != Kit::Look::ProjectPlus &&
      Config::Get(Config::SYSCONF_WIDESCREEN))
  {
    const u8 ports = s.display.hud_ports & 0x0F;
    const int count = std::popcount(static_cast<unsigned>(ports));
    if (count >= 2 && !(s.display.hud_ports & 0x10))
    {
      const std::array<float, 4>& xs = HUD_LAMP_X[count - 2];
      const float r = pic_size.y * HUD_LAMP_R;
      const int mode = f.hud_names;
      const bool below = mode == 2 || mode == 3;
      const bool no_names = mode == 4;
      const float row = pic.y + pic_size.y * (no_names ? HUD_FIGHTER_NAME_Y :
                                              below    ? HUD_BELOW_ROW_Y :
                                                         HUD_ROW_Y);
      const float name_px =
          pic_size.y * (mode == 2 ? HUD_BELOW_NAME_PX : mode == 3 ? HUD_SMALL_NAME_PX : HUD_NAME_PX);
      const float name_px2 = pic_size.y * (below ? HUD_BELOW_NAME_PX2 : HUD_NAME_PX2);
      const bool port_stroke = mode == 1;
      const bool stroke = port_stroke || mode == 2;
      int slot = 0;
      for (int port = 0; port < 4; ++port)
      {
        if (!(ports & (1 << port)))
          continue;
        if (!f.port_names[port].empty())
        {
          const int first_vertex = dl->VtxBuffer.Size;
          if (stroke)
          {
            const float left = pic.x + pic_size.x * (xs[slot] + HUD_LAMP_DX) - r * 32.0f / 30.0f -
                               pic_size.y * HUD_STROKE_LEAD;
            const float cy = row + pic_size.y * (below ? HUD_BELOW_STROKE_DY : HUD_STROKE_DY);
            const float h = pic_size.y * (below ? HUD_BELOW_STROKE_H : HUD_STROKE_H);
            const float w = pic_size.x * (below ? HUD_BELOW_STROKE_W : HUD_STROKE_W);
            Kit::NameStroke(dl, ImVec2(left, cy - h / 2), ImVec2(left + w, cy + h / 2),
                            port_stroke ? PORT_STROKE[port] : IM_COL32(0, 0, 0, 230));
          }
          if (!no_names)
          {
            const HudNameFit fit =
                FitHudName(f.port_names[port], name_px, name_px2, pic_size.x * HUD_NAME_MAX_W);
            HudNameText(dl, pic.x + pic_size.x * (xs[slot] + HUD_NAME_DX), row, fit, name_px2,
                        below ? -0.5f : -0.1f);
          }
          if (f.port_lamps[port] > 0 && f.show_ping)
          {
            const std::string ping = PingText(f.port_ms[port]);
            const float px = pic_size.y * HUD_PING_PX;
            const HudNameFit pf = FitHudName(ping, px, px, FLT_MAX);
            const float right = pic.x + pic_size.x * (xs[slot] + HUD_LAMP_DX) + r * 32.0f / 30.0f;
            HudNameText(dl, right - pf.width, row, pf, px, -0.5f);
          }
          else if (f.port_lamps[port] > 0)
          {
            Kit::Connection(dl, ImVec2(pic.x + pic_size.x * (xs[slot] + HUD_LAMP_DX), row), r,
                            unit, f.port_lamps[port]);
          }
          FadeSince(dl, first_vertex, 1.0f - HUD_FADED * s.display.hud_fade[port] / 255.0f);
        }
        ++slot;
      }
    }
    if (s.display.hud_ports & 0x20)
    {
      const float r = pic_size.y * HUD_LAMP_R;
      const float half = r * 32.0f / 30.0f;
      const float gap = pic_size.y * HUD_LAMP_GAP;
      const float row = pic.y + pic_size.y * CSS_ROW_Y;
      for (int port = 0; port < 4; ++port)
      {
        if (f.port_names[port].empty())
          continue;
        const HudNameFit fit =
            FitHudName(f.port_names[port], pic_size.y * CSS_NAME_PX,
                       pic_size.y * CSS_NAME_PX2, pic_size.x * CSS_NAME_MAX_W);
        const bool lamp = f.port_lamps[port] > 0;
        const float ping_px = pic_size.y * HUD_PING_PX;
        const HudNameFit pf = FitHudName(PingText(f.port_ms[port]), ping_px, ping_px, FLT_MAX);
        const float mark = !lamp ? 0 : f.show_ping ? pf.width : 2 * half;
        const bool no_names = f.hud_names == 4;
        const float width = no_names ? mark : (lamp ? mark + gap : 0) + fit.width;
        const float left = pic.x + pic_size.x * PanelCentre(port) - width / 2;
        if (lamp && f.show_ping)
          HudNameText(dl, left, row, pf, ping_px, -0.5f);
        else if (lamp)
          Kit::Connection(dl, ImVec2(left + half, row), r, unit, f.port_lamps[port]);
        if (!no_names)
        {
          HudNameText(dl, lamp ? left + mark + gap : left, row, fit, pic_size.y * CSS_NAME_PX2,
                      -0.5f);
        }
      }
    }
  }
  const float margin = 2.5f * unit;
  const Kit::Box orb = AppOrb::ButtonBox(orb_view);
  const bool orb_top = orb.max.x > orb.min.x;
  static float s_corner_w = 0;
  static double s_corner_at = 0;
  const float corner_now = CornerWidth(f, unit);
  if (corner_now >= s_corner_w || now - s_corner_at > 4000)
    s_corner_w = corner_now;
  if (corner_now >= s_corner_w)
    s_corner_at = now;
  const bool corner_right = orb_top && s_corner_w > 0;
  const float corner_x =
      corner_right ? vis.x + vis_size.x - margin - s_corner_w : vis.x + margin;
  std::string guide;
  const Kit::Box centre =
      DrawTopCentre(dl, s, pic, pic_size, vis, vis_size, unit, now_s,
                    std::max(pic.y + (s.dots.on ? TOP_UNDER_DOTS : TOP_FIRST) * unit,
                             vis.y + TOP_FIRST * unit),
                    &guide, corner_right ? corner_x - 1.0f * unit : pic.x + pic_size.x);
  const bool pplus = Kit::CurrentLook() == Kit::Look::ProjectPlus;
  if (s.band_seconds >= 0)
  {
    const GameTimerPlace& at = pplus ? BAND_TIMER_PPLUS : BAND_TIMER_BRAWL;
    Kit::GameTimer(dl, ImVec2(pic.x + pic_size.x * at.x, pic.y + pic_size.y * at.y), unit,
                   s.band_seconds, now_s, Kit::Lettering::Band, at.size);
  }
  if (s.line_seconds >= 0)
  {
    const GameTimerPlace& at = pplus ? LINE_TIMER_PPLUS : LINE_TIMER_BRAWL;
    const float half = Kit::Measure(Kit::TitleLook(unit, at.size), "0:00").x / 2;
    Kit::GameTimer(dl, ImVec2(pic.x + pic_size.x * at.x + half, pic.y + pic_size.y * at.y), unit,
                   s.line_seconds, now_s, Kit::Lettering::Line, at.size);
  }
  if (!guide.empty())
  {
    static std::string s_hint;
    static std::vector<Kit::GuideItem> s_items;
    if (guide != s_hint)
    {
      s_hint = guide;
      s_items = Kit::GuideOf(guide);
    }
    std::vector<Kit::GuideItem> items;
    for (const Kit::GuideItem& item : s_items)
    {
      if (!(s.legend && (item.button == "X" || item.button == "A" || item.button == "B")))
        items.push_back(item);
    }
    const float right = std::min(pic.x + pic_size.x * GUIDE_RIGHT,
                                 vis.x + vis_size.x - pic_size.x * (1 - GUIDE_RIGHT));
    const float cy =
        std::min(pic.y + pic_size.y * GUIDE_Y, vis.y + vis_size.y - pic_size.y * (1 - GUIDE_Y));
    Kit::Guide(dl, right, cy, unit, items, 1,
               std::max(right - vis.x - pic_size.x * 0.025f, 1.0f));
  }
  const float left = vis.x + margin;
  float y = DrawCorner(dl, f, ImVec2(corner_x, vis.y + margin), unit);
  if (!orb_top && corner_now > 0)
    OSD::SetObscuredPixelsTop(static_cast<int>(y));
  if (!f.waiting.empty())
  {
    const float top = std::max(vis.y + vis_size.y * 0.18f, centre.max.y + 1.5f * unit);
    Kit::TextLook look = Kit::TitleLook(unit, 7.0f);
    look.face = Kit::Face::Body;
    Kit::Text(dl, ImVec2(vis.x + vis_size.x / 2, top), look, f.waiting, 1.0f, Kit::Align::Centre);
  }
  if (orb_top)
  {
    std::array<AppOrb::PillRow, 12> rows;
    std::size_t n = 0;
    for (const OverlayModel::Toast& t : f.toasts)
    {
      if (n < rows.size())
        rows[n++] = AppOrb::PillRow{AppOrb::PillRow::Type::Toast, {}, t.text, {}, {},
                                   AppOrb::Kind::Join, t.alpha, t.at};
    }
    for (const OverlayModel::ChatToast& c : f.chats)
    {
      if (n < rows.size())
        rows[n++] = AppOrb::PillRow{AppOrb::PillRow::Type::Chat, c.name, c.text, {}, {},
                                   AppOrb::Kind::Join, c.alpha, c.at};
    }
    const float pill_right = centre.max.y > vis.y && centre.min.x > orb.min.x ?
                                 centre.min.x - 1.2f * unit :
                                 vis.x + vis_size.x;
    const Kit::Box pill =
        AppOrb::DrawPill(dl, orb_view, std::span<const AppOrb::PillRow>(rows.data(), n), now, pill_right);
    const float under = std::max(orb.max.y, pill.max.y > pill.min.y ? pill.max.y : 0.0f);
    OSD::SetObscuredPixelsTop(static_cast<int>(under + 0.8f * unit));
  }
  else if (!f.toasts.empty() || !f.chats.empty())
  {
    y = std::max(y, OSD::MessagesBottom() + unit);
    const float wrap =
        std::max(std::min(0.36f * pic_size.x, vis.x + vis_size.x - margin - left), 12.0f * unit);
    if (centre.max.y > vis.y && centre.min.x < left + wrap + 3.0f * unit)
      y = std::max(y, centre.max.y + 1.2f * unit);
    const float bottom = vis.y + vis_size.y - margin;
    for (const OverlayModel::Toast& t : f.toasts)
    {
      if (y >= bottom)
        break;
      y = AppCard::NoticeCard(dl, ImVec2(left, y), unit, t.text, wrap, t.alpha).max.y +
          0.8f * unit;
    }
    for (const OverlayModel::ChatToast& c : f.chats)
    {
      if (y >= bottom)
        break;
      y = AppCard::ChatCard(dl, ImVec2(left, y), unit, c.name, c.text, wrap, c.alpha).max.y +
          0.8f * unit;
    }
  }
  if (!s.set_end.Empty())
    SetEnd::Draw(dl, unit, vis.x, vis.y, vis_size.x, vis_size.y, s.set_end);
  AppOrb::DrawButton(dl, orb_view);
  AppOrb::DrawHostControls(dl, orb_view);
}

void DrawDim()
{
  const float target = static_cast<float>(s_dim.load(std::memory_order_relaxed)) / 100.0f;
  const double now = ImGui::GetTime();
  const double since = s_dim_at < 0 || now < s_dim_at ? DIM_EASE_S : now - s_dim_at;
  s_dim_at = now;
  const float step = static_cast<float>(since / DIM_EASE_S * 0.5);
  s_dim_shown = s_dim_shown < target ? std::min(target, s_dim_shown + step) :
                                       std::max(target, s_dim_shown - step);
  if (!(s_dim_shown > 0))
    return;
  const ImVec2 display = ImGui::GetIO().DisplaySize;
  if (!(display.x > 0 && display.y > 0))
    return;
  ImGui::GetForegroundDrawList()->AddRectFilled(
      ImVec2(0, 0), display,
      IM_COL32(0, 0, 0, static_cast<int>(std::lround(255.0f * std::min(s_dim_shown, 1.0f)))));
}

void Draw()
{
  DrawScene();
  DrawDim();
}
}

OverlayModel& Overlay()
{
  return s_overlay;
}

PortColour PortColourOf(int port)
{
  switch (port)
  {
  case 0:
    return {224, 44, 44};
  case 1:
    return {40, 104, 236};
  case 2:
    return {232, 180, 24};
  default:
    return {44, 168, 64};
  }
}

PictureArea VisiblePicture(const PictureArea& picture, float display_w, float display_h)
{
  const float x0 = std::max(picture.x, 0.0f), y0 = std::max(picture.y, 0.0f);
  const float x1 = std::min(picture.x + picture.w, display_w);
  const float y1 = std::min(picture.y + picture.h, display_h);
  if (!(x1 > x0 && y1 > y0))
    return picture;
  return {x0, y0, x1 - x0, y1 - y0};
}

void SetPictureForTests(std::optional<PictureArea> picture)
{
  s_test_picture = picture;
}

void SetDim(int percent)
{
  s_dim.store(std::clamp(percent, 0, 100), std::memory_order_relaxed);
}

int Dim()
{
  return s_dim.load(std::memory_order_relaxed);
}

float OverlayUnit(float display_height)
{
  if (!(display_height >= OVERLAY_MIN_HEIGHT))
    return 0;
  return display_height / 100;
}

void SetRulesLines(std::string line, std::string note)
{
  std::lock_guard lk(s_rules_lock);
  s_rules_line = std::move(line);
  s_rules_note = std::move(note);
}

void SetStats(Stats stats)
{
  s_stats = stats;
}

void SetPerf(Perf perf)
{
  s_perf = perf;
}

void FrameBoundary(bool shown, double game_held_ms, int game_held_frames)
{
  s_overlay.OnFrame(NowMs(), shown, game_held_ms, game_held_frames);
}

void ShowToast(std::string text)
{
  s_overlay.AddToast(std::move(text), NowMs());
}

void ShowRestartForMatch()
{
  ShowToast("Restarting the game for this match");
}

void ShowRestartForJoin(std::string_view name)
{
  ShowToast(fmt::format("Restarting the game in 3 seconds so {} can join. "
                       "Unsaved progress will be lost.", name.empty() ? "your friend" : name));
}

void SetQueueLines(std::string line, std::string note, std::array<std::string, 4> labels,
                   std::array<bool, 4> locked)
{
  std::lock_guard lk(s_queue_lock);
  s_queue_line = std::move(line);
  s_queue_note = std::move(note);
  s_queue_labels = std::move(labels);
  s_queue_locked = locked;
}

void ShowChat(std::string name, std::string text)
{
  s_overlay.AddChat(std::move(name), std::move(text), NowMs());
}

void SetRuleLine(std::string text)
{
  std::lock_guard lk(s_rule_line_lock);
  s_rule_line = std::move(text);
}

void ShutdownOverlay()
{
  OSD::SetHostOverlay({});
  Kit::Shutdown();
  s_state_hook.reset();
  s_test_picture.reset();
}

void SetHostVisible(bool visible)
{
  s_visible = visible;
}

bool HostVisible()
{
  return s_visible;
}

void RepresentDuringStall(int stalled_ms)
{
  static double s_last = 0;
  static double s_every = 66;
  static int s_stall_ms = 0;
  static bool s_off_this_stall = false;
  if (stalled_ms < s_stall_ms)
    s_off_this_stall = false;
  s_stall_ms = stalled_ms;
  if (s_off_this_stall || !s_visible.load(std::memory_order_relaxed) ||
      !VideoCommon::Presenter::IsSurfaceVisible() ||
      stalled_ms < OverlayModel::WAITING_AFTER_MS - 50)
  {
    return;
  }
  const double now = NowMs();
  if (now - s_last < s_every)
    return;
  AsyncRequests::GetInstance()->PushEvent([] {
    if (g_presenter)
      g_presenter->RepresentLast();
  });
  const double took = NowMs() - now;
  s_every = took > 12 ? 250 : 66;
  if (took > 50)
    s_off_this_stall = true;
  s_last = NowMs();
}

void InitOverlay()
{
  s_dim = 0;
  s_dim_shown = 0;
  s_dim_at = -1;
  if (const char* v = std::getenv("ORCA_UX_STATS"); v && (v[0] == '1' || v[0] == '2'))
    s_stats = v[0] == '2' ? Stats::Full : Stats::Ping;
  if (const char* v = std::getenv("ORCA_UX_OVERLAY_DEMO"); v && v[0] == '1')
    s_demo = true;
  s_state_hook = Core::AddOnStateChangedCallback([](Core::State state) {
    if (state == Core::State::Paused)
      s_overlay.Pause(NowMs());
    else if (state == Core::State::Running)
      s_overlay.Resume(NowMs());
    if (state == Core::State::Paused || state == Core::State::Stopping)
      LocalPadStopped();
  });
  OSD::SetHostOverlay(&Draw, &Kit::ReleaseGpu);
}
}
