// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <mutex>
#include <optional>
#include <string>

#include "Common/CommonTypes.h"

struct ImDrawList;

namespace Orca::UX::Widgets
{
enum class Tone : int;
}

namespace Orca::UX::SetEnd
{
class RatingCount
{
public:
  static constexpr double WAIT_MS = 10000;
  static constexpr double COUNT_MS = 1000;
  static constexpr double POP_MS = 450;
  static constexpr float POP_SCALE = 0.28f;

  enum class Phase
  {
    None,
    Updating,
    Counting,
    Landed,
    Gone,
  };
  struct View
  {
    Phase phase = Phase::None;
    int shown = -1;
    bool has_delta = false;
    int delta = 0;
    float pop = 1;
    float badge_pop = 1;
    bool operator==(const View&) const = default;
  };

  void Start(int before, double now);
  void Arrive(int after, double now, int before = -1);
  bool Started() const { return m_started; }
  bool Arrived() const { return m_after >= 0; }
  int Before() const { return m_before; }
  int After() const { return m_after; }
  double LandsAt() const;
  View At(double now) const;

private:
  bool m_started = false;
  double m_start = 0;
  int m_before = -1;
  int m_from = -1;
  int m_after = -1;
  double m_arrived = -1;
};

int SecondsLeft(double deadline_ms, double now_ms);

enum class Outcome
{
  Won,
  Lost,
  Draw,
  Void,
};

struct Notice
{
  std::string title;
  std::string line;
  int seconds = -1;
  float fraction = 0;
  Widgets::Tone tone{};
  float alpha = 1;
  float scale = 1;
  bool operator==(const Notice&) const = default;
};
struct Panel
{
  bool ranked = true;
  Outcome out = Outcome::Won;
  std::string title;
  std::string score;
  std::string detail;
  bool show_rating = false;
  RatingCount::View rating;
  float alpha = 1;
  float scale = 1;
  bool operator==(const Panel&) const = default;
};
struct Hint
{
  std::string text;
  float progress = 0;
  bool operator==(const Hint&) const = default;
};
struct View
{
  std::optional<Notice> notice;
  std::optional<Panel> panel;
  std::optional<Hint> hint;
  bool hides_waiting = false;
  bool Empty() const { return !notice && !panel && !hint; }
};

class Model
{
public:
  static constexpr double LEAVE_FORFEIT_MS = 15000;
  static constexpr double STALL_FORFEIT_MS = 10000;
  static constexpr double STALL_NOTICE_MS = 1500;
  static constexpr double NO_VERDICT_MS = 20000;
  static constexpr double FORFEITED_MS = 2500;
  static constexpr double PANEL_MS = 7000;
  static constexpr double PANEL_AFTER_LAND_MS = 3000;
  static constexpr double PANEL_MAX_MS = 15000;
  static constexpr double CASUAL_MS = 6000;
  static constexpr double NOTICE_MS = 4000;
  static constexpr double ENTER_MS = 220;
  static constexpr double FADE_MS = 400;

  void MatchBegan(bool ranked, std::string opponent, double now);
  void BlockSetOver(u32 key, bool won, int mine, int theirs, int rating_before, double now);
  void SetVerdict(Outcome out, bool forfeit, int mine, int theirs, int before, int after,
                  int rating_before, double now);
  void CasualGame(Outcome out, int mine, int theirs, double now);
  void QueueRating(int rating, double now);
  void GameBegan(double now);
  void OpponentLeft(double now);
  void OpponentBack(double now);
  void Stall(int stalled_ms, double now);
  void StallOver(bool resumed, double now);
  void RoomHold(std::optional<double> deadline_ms);
  void OpponentDropped(double now);
  void CasualOpponentLeft(double now);
  void SelfLeft(bool no_show, double now);
  void NoVerdict(double now);
  void SetOver(double now);
  void OpponentLeftAfterSet(double now);
  void LeaveHint(bool shown, bool opponent_gone, float progress);
  void LeavingSetRoom(double now);
  bool InSetOver() const;

  View At(double now) const;
  std::string Opponent() const;

private:
  void ResetLocked();
  void OpenPanel(bool ranked, Outcome out, int mine, int theirs, bool forfeit, int rating_before,
                 double now);
  std::optional<Notice> NoticeAt(double now, bool* hides_waiting) const;
  std::optional<Panel> PanelAt(double now) const;
  std::string Name() const;

  mutable std::mutex m_lock;
  std::string m_opponent;
  bool m_ranked_live = false;
  bool m_casual_live = false;
  bool m_game_began = false;
  std::optional<double> m_room_hold;

  std::optional<double> m_left_at;
  std::optional<double> m_stall_at;
  bool m_dropped = false;
  std::optional<double> m_forfeited_at;

  bool m_panel = false;
  bool m_panel_ranked = true;
  Outcome m_out = Outcome::Won;
  int m_mine = 0, m_theirs = 0;
  bool m_by_forfeit = false;
  double m_panel_at = -1;
  u32 m_block_key = 0;
  bool m_block_key_set = false;
  RatingCount m_rating;
  bool m_rating_from_room = false;

  std::string m_note_title, m_note_line;
  Widgets::Tone m_note_tone{};
  double m_note_at = -1;
  double m_self_left_at = -1;
  bool m_note_after_set = false;

  bool m_set_over = false;
  bool m_set_over_leaving = false;
  bool m_hint = false;
  bool m_hint_alone = false;
  float m_hint_progress = 0;
};

Model& Current();
double NowMs();

void Draw(ImDrawList* dl, float unit, float pic_x, float pic_y, float pic_w, float pic_h,
          const View& view);

double DemoFrame(double now);
}
