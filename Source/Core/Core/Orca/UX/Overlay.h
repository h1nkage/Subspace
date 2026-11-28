// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Orca/Session/Events.h"

namespace Orca::UX
{
enum class Stats
{
  Off,
  Ping,
  Full,
};

enum class Perf
{
  Off,
  Fps,
  Detailed,
};

enum class Signal
{
  Good,
  Ok,
  Bad,
};

struct LinkView
{
  bool online = false;
  int delay = 0;
  double rollbacks_per_second = 0;
  int deepest_rollback = 0;
  int stalls = 0;
  int round_trip_ms = -1;
  int transport = 0;
  int stalled_ms = 0;
  int waiting_seat = -1;
  int lamp_ms = -1;
  std::array<int, 4> seat_lamp_ms{-1, -1, -1, -1};
};
LinkView ViewOf(const Events::LinkStats& stats);

class OverlayModel
{
public:
  static constexpr double TOAST_MS = 3000;
  static constexpr double TOAST_FADE_MS = 500;
  static constexpr int WAITING_AFTER_MS = 250;
  static constexpr double PERF_WINDOW_MS = 1000;
  static constexpr double PERF_GRAPH_MS = 2000;
  static constexpr double PERF_HOLD_MS = 2000;
  static constexpr int PERF_OK_FPS = 59;
  static constexpr double PERF_OK_FRAME_MS = 20;
  static constexpr int PERF_BAD_FPS = 50;
  static constexpr double PERF_BAD_FRAME_MS = 2000.0 / 60;
  static constexpr int PERF_MAX_UNSHOWN = 60;

  static constexpr double CHAT_MS = 8000;
  static constexpr std::size_t CHAT_KEEP = 3;

  struct Toast
  {
    std::string text;
    float alpha;
    double at = 0;
  };
  struct ChatToast
  {
    std::string name;
    std::string text;
    float alpha;
    double at = 0;
  };
  struct Frame
  {
    bool show_signal = false;
    Signal signal = Signal::Good;
    int lamp = 5;
    std::array<int, 4> port_lamps{};
    std::array<std::string, 4> port_names;
    bool show_ping = false;
    int hud_names = 0;
    std::array<int, 4> port_ms{-1, -1, -1, -1};
    std::string waiting;
    std::vector<Toast> toasts;
    std::vector<ChatToast> chats;
    std::string stats;
    std::string perf;
    std::string perf_fps;
    std::string perf_ms;
    bool corner_sprite = true;
    bool corner_fps = true;
    bool frame_ms = false;
    bool corner_ping = true;
    Signal perf_level = Signal::Good;
    std::vector<float> graph;
  };

  void OnPorts(int frame, const std::vector<Events::PortInfo>& ports, double now_ms);
  void AddToast(std::string text, double now_ms);
  void AddChat(std::string name, std::string text, double now_ms);
  void Update(const LinkView& link, double now_ms);
  void OnFrame(double now_ms, bool shown, double game_held_ms = 0, int game_held_frames = 0);
  void Pause(double now_ms);
  void Resume(double now_ms);
  Frame View(double now_ms, Stats stats, Perf perf = Perf::Off) const;

private:
  Signal Quality(double now_ms) const;
  int Lamp(double now_ms) const;
  void Meter(double now_ms, Stats stats, Perf perf, Frame* f) const;
  double RunningMs(double now_ms) const;

  mutable std::mutex m_lock;
  bool m_seen_ports = false;
  std::vector<Events::PortInfo> m_ports;
  std::deque<std::pair<double, std::string>> m_toasts;
  struct ChatEntry
  {
    double at;
    std::string name;
    std::string text;
  };
  std::deque<ChatEntry> m_chats;
  LinkView m_link;
  std::deque<std::pair<double, int>> m_stall_history;
  std::array<int, 4> m_seat_stalls{};
  std::deque<std::pair<double, std::array<int, 4>>> m_seat_stall_history;
  int m_last_stalls = -1;
  int m_stall_seat = -1;
  std::deque<std::pair<double, double>> m_frames;
  double m_first_frame = -1;
  double m_last_frame = -1;
  int m_unshown = 0;
  bool m_paused = false;
  double m_paused_at = 0;
  double m_paused_ms = 0;
};

void InitOverlay();
void ShutdownOverlay();
void RepresentDuringStall(int stalled_ms);
void SetHostVisible(bool visible);
bool HostVisible();
void SetDim(int percent);
int Dim();
constexpr double DIM_EASE_S = 0.2;
void SetStats(Stats stats);
void SetPerf(Perf perf);
void FrameBoundary(bool shown, double game_held_ms, int game_held_frames);
void ShowToast(std::string text);
void ShowRestartForJoin(std::string_view name);
void ShowRestartForMatch();
void ShowChat(std::string name, std::string text);
void SetRulesLines(std::string line, std::string note);
void SetRuleLine(std::string text);
void SetQueueLines(std::string line, std::string note, std::array<std::string, 4> labels,
                   std::array<bool, 4> locked = {});
struct PortColour
{
  int r, g, b;
};
PortColour PortColourOf(int port);
OverlayModel& Overlay();
constexpr float OVERLAY_MIN_HEIGHT = 100;
float OverlayUnit(float display_height);
struct PictureArea
{
  float x = 0, y = 0, w = 0, h = 0;
};
PictureArea VisiblePicture(const PictureArea& picture, float display_w, float display_h);
void SetPictureForTests(std::optional<PictureArea> picture);
}
