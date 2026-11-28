// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/Orca/UX/OverlayKey.h"

namespace Orca::UX::AppOrb
{
enum class Kind
{
  Join,
  Leave,
  Away,
  Back,
  Voice,
  Call,
  Dm,
  Invite,
};

struct Place
{
  float x = 0;
  float y = 0;
  float size = 0;
  bool lit = false;
  bool away = false;
  int badge = 0;
};

struct ControlState
{
  bool hovered = false;
  bool pressed = false;
};

struct Command
{
  enum class Type
  {
    Off,
    Blink,
    Place,
  };
  Type type = Type::Off;
  Place place;
};
std::optional<Command> ParseOrb(std::string_view line);

struct Notice
{
  Kind kind = Kind::Join;
  std::string name;
  std::string text;
  std::string key;
  std::string verb;
};
std::optional<Notice> ParseNotice(std::string_view line);
inline constexpr std::size_t HINT_POINTS = 24;

struct Talk
{
  enum class Phase
  {
    Off,
    Listen,
    Send,
    Sent,
    Error,
  };
  Phase phase = Phase::Off;
  std::string name;
  std::string text;
};
std::optional<Talk> ParseTalk(std::string_view line);
inline constexpr double TALK_STALE_MS = 60000;
double TalkDwellMs(Talk::Phase phase);
float TalkAlpha(const Talk& talk, double at_ms, double now_ms);

Place ScaledPlace(const Place& place, float view_width, float display_width);

double DwellMs(const Notice& notice);

float LineAlpha(const Notice& notice, double at_ms, double now_ms);

class Model
{
public:
  static constexpr double FADE_MS = 400;
  static constexpr std::size_t KEEP = 3;
  static constexpr double RING_MS = 700;
  static constexpr int RINGS = 3;
  static constexpr double LISTEN_RING_MS = 1200;

  struct Entry
  {
    double at = 0;
    Notice notice;
  };
  struct TalkEntry
  {
    double at = 0;
    Talk talk;
  };
  struct View
  {
    bool on = false;
    Place place;
    float ring = -1;
    bool listening = false;
    std::uint64_t generation = 0;
    std::uint64_t talk_generation = 0;
    bool showing = false;
    float view_width = 0;
    ControlState orb;
    ControlState feedback;
  };

  void Apply(const Command& command, double now_ms);
  void Add(Notice notice, double now_ms);
  void SetTalk(Talk talk, double now_ms);
  void SetView(int width, int height);
  View At(double now_ms) const;
  std::uint64_t Lines(std::uint64_t generation, std::vector<Entry>* out) const;
  std::uint64_t TalkRow(std::uint64_t generation, TalkEntry* out) const;
  bool Hit(float x, float y) const;
  void SetPointer(int control, bool down);

private:
  mutable std::mutex m_lock;
  bool m_on = false;
  Place m_place;
  double m_blink_at = -1;
  std::deque<Entry> m_lines;
  std::uint64_t m_generation = 0;
  TalkEntry m_talk;
  std::uint64_t m_talk_generation = 0;
  double m_listen_at = 0;
  int m_view_width = 0;
  int m_hovered = -1;
  int m_pressed = -1;
  bool m_pointer_down = false;
};

enum class HostAction
{
  Feedback,
};

class FeedbackCooldown
{
public:
  bool FeedbackDue(double now_ms);
  void FeedbackFinished(double now_ms);

private:
  mutable std::mutex m_lock;
  double m_feedback_at = -5000;
  bool m_feedback_pending = false;
};

bool HostEnabled();
FeedbackCooldown& FeedbackRequests();
Model::View HostView(double now_ms);
int ControlAt(const Place& place, float x, float y);
void UpdateControlsPointer(float x, float y, bool down);
bool HitControls(float x, float y);
bool ClickControls(float x, float y, OverlayKey::Output output = nullptr);
void ToggleHostMenu(OverlayKey::Output output = nullptr);
enum class HostKey
{
  Feedback,
  Menu,
};
bool HandleHostKey(HostKey key, OverlayKey::Output output = nullptr);
void ActivateHost(HostAction action);
std::optional<std::string> PublishFeedback(const std::string& directory, std::string_view name,
                                          bool saved);

Model& Current();
double NowMs();
void Apply(const Command& command);
void Show(Notice notice);
void SetTalk(Talk talk);
void SetViewSize(int width, int height);
}
