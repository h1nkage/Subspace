// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Games/Brawl/StageCursors.h"

#include <algorithm>
#include <mutex>
#include <utility>

#include <fmt/format.h>

#include "Core/Orca/Engine/GuestMemory.h"
#include "Core/Orca/Games/Brawl/OnlineMenu.h"
#include "Core/Orca/Games/Brawl/OnlineRules.h"
#include "Core/Orca/UX/RankedSteps.h"

namespace Orca::UX::StageCursors
{
namespace
{
enum Shared : u32
{
  S_TAG = 0,
  S_GAME = 1,
  S_FOLLOWER = 2,
  S_PAGE_TURN = 3,
  S_PAGE_FRAMES = 4,
};
enum Cursor_ : u32
{
  C_X = 0,
  C_Y = 2,
  C_NOW = 4,
  C_SEEN = 5,
};

constexpr s16 U(float units)
{
  return static_cast<s16>(units * 16.0f + (units < 0 ? -0.5f : 0.5f));
}
constexpr Rect R(float x0, float y0, float x1, float y1)
{
  return Rect{U(x0), U(y0), U(x1), U(y1)};
}

constexpr std::array<Tile, 7> kBrawlTiles{{
    {0, 0, R(-4.75f, 13.25f, 1.25f, 19.25f)},
    {1, 1, R(18.25f, -12.75f, 26.25f, -4.75f)},
    {2, 0, R(8.75f, 1.75f, 14.75f, 7.75f)},
    {3, 0, R(20.25f, -4.25f, 26.25f, 2.25f)},
    {4, 0, R(-4.25f, 1.75f, 1.75f, 7.75f)},
    {5, 0, R(1.75f, 13.25f, 7.25f, 19.25f)},
    {6, 0, R(7.75f, 13.25f, 13.75f, 19.25f)},
}};

constexpr std::array<Tile, 8> kBrawlRankedTiles{{
    kBrawlTiles[0], kBrawlTiles[1], kBrawlTiles[2], kBrawlTiles[3],
    kBrawlTiles[4], kBrawlTiles[5], kBrawlTiles[6],
    {7, 0, R(24.25f, 7.25f, 29.75f, 13.25f)},
}};

constexpr std::array<Tile, 9> kPPlusTiles{{
    {5, 0, R(-15.25f, -11.25f, -8.25f, -3.25f)},
    {6, 0, R(-6.75f, -11.25f, 0.75f, -3.25f)},
    {7, 0, R(1.75f, -11.25f, 9.25f, -3.25f)},
    {8, 0, R(10.25f, -11.25f, 17.75f, -3.25f)},
    {4, 0, R(-19.75f, -17.75f, -12.25f, -11.25f)},
    {2, 0, R(-11.25f, -17.75f, -3.75f, -11.25f)},
    {0, 0, R(-2.75f, -17.75f, 4.75f, -11.25f)},
    {1, 0, R(5.75f, -17.75f, 13.25f, -11.25f)},
    {3, 0, R(14.25f, -17.75f, 21.75f, -11.25f)},
}};

void Map(Layout* l)
{
  l->u0 = (639.8f - 15.0f) / 1250.0f;
  l->du = 15.18f / 1250.0f;
  l->v0 = 358.4f / 720.0f;
  l->dv = -14.42f / 720.0f;
}

Layout MakeBrawl()
{
  Layout l;
  l.game = GAME_BRAWL;
  l.bounds = R(-30.0f, -19.5f, 30.0f, 19.5f);
  l.speed = U(1.6f);
  l.tiles = kBrawlTiles;
  l.pages = true;
  l.page_button = R(15.25f, -19.75f, 30.25f, -17.25f);
  l.random_button = R(-5.25f, -21.0f, 15.25f, -17.25f);
  l.random_art = R(-4.9f, -20.7f, 15.0f, -17.4f);
  l.start = {Point{U(-2.0f), U(-16.5f)}, Point{U(9.0f), U(-16.5f)}};
  Map(&l);
  return l;
}

Layout MakePPlus()
{
  Layout l;
  l.game = GAME_PPLUS;
  l.bounds = R(-31.0f, -23.5f, 31.0f, 20.5f);
  l.speed = U(1.6f);
  l.tiles = kPPlusTiles;
  l.pages = false;
  l.random_button = R(-16.25f, -23.75f, 3.0f, -19.0f);
  l.random_art = R(-16.0f, -22.6f, 4.0f, -19.3f);
  l.start = {Point{U(-6.0f), U(-18.5f)}, Point{U(6.0f), U(-18.5f)}};
  Map(&l);
  return l;
}

void Put8(GuestMemory& m, u32 a, u8 v, int* changed)
{
  if (m.Read8(a) != v)
  {
    m.Write8(a, v);
    ++*changed;
  }
}
void Put16(GuestMemory& m, u32 a, u16 v, int* changed)
{
  Put8(m, a, static_cast<u8>(v >> 8), changed);
  Put8(m, a + 1, static_cast<u8>(v), changed);
}

bool Mapped(const GuestMemory& m)
{
  return m.Valid(MatchBlock::MAGIC) && m.Read32(MatchBlock::MAGIC) == MatchBlock::MAGIC_VALUE &&
         m.Valid(SHARED) && m.Valid(SHARED + SHARED_SIZE - 1) && m.Valid(CURSORS) &&
         m.Valid(CURSORS + CURSORS_SIZE - 1);
}

int Velocity(u8 axis, const Layout& l)
{
  int d = static_cast<int>(axis) - 128;
  if (d > -l.dead_zone && d < l.dead_zone)
    return 0;
  d = std::clamp(d, -100, 100);
  return d * l.speed / 100;
}

std::mutex s_view_lock;
View s_view;
View s_next;
}

u8 ButtonsOf(u16 pad)
{
  u8 out = 0;
  if (pad & PAD_BUTTON_A)
    out |= BTN_A;
  if (pad & PAD_BUTTON_B)
    out |= BTN_B;
  if (pad & PAD_BUTTON_X)
    out |= BTN_X;
  if (pad & PAD_BUTTON_Y)
    out |= BTN_Y;
  if (pad & PAD_BUTTON_START)
    out |= BTN_START;
  if (pad & PAD_TRIGGER_Z)
    out |= BTN_Z;
  return out;
}

const Layout& BrawlLayout()
{
  static const Layout l = MakeBrawl();
  return l;
}

const Layout& BrawlRankedLayout()
{
  static const Layout l = [] {
    Layout ranked = MakeBrawl();
    ranked.game = GAME_BRAWL_RANKED;
    ranked.tiles = kBrawlRankedTiles;
    return ranked;
  }();
  return l;
}

const Layout& PPlusLayout()
{
  static const Layout l = MakePPlus();
  return l;
}

const Layout* LayoutFor(u8 game)
{
  if (game == GAME_BRAWL)
    return &BrawlLayout();
  if (game == GAME_BRAWL_RANKED)
    return &BrawlRankedLayout();
  if (game == GAME_PPLUS)
    return &PPlusLayout();
  return nullptr;
}

int Hovered(const Layout& l, u32 page, int x, int y)
{
  for (const Tile& t : l.tiles)
  {
    if (t.page == page && t.rect.Contains(x, y))
      return t.stage;
  }
  if (l.pages && l.page_button.Contains(x, y))
    return PAGE_BUTTON;
  if (l.random_button.Contains(x, y))
    return RANDOM_BUTTON;
  return NOTHING;
}

const Tile* TileOf(const Layout& l, int stage, u32 page)
{
  for (const Tile& t : l.tiles)
  {
    if (t.stage == stage && t.page == page)
      return &t;
  }
  return nullptr;
}

float MidX(const Rect& r)
{
  return static_cast<float>(r.x0 + r.x1) * (1.0f / 32.0f);
}

float MidY(const Rect& r)
{
  return static_cast<float>(r.y0 + r.y1) * (1.0f / 32.0f);
}

std::optional<State> Read(const GuestMemory& m)
{
  if (!Mapped(m) || m.Read8(SHARED + S_TAG) != TAG_ON)
    return std::nullopt;
  State s;
  s.game = m.Read8(SHARED + S_GAME);
  s.follower = m.Read8(SHARED + S_FOLLOWER) & 1;
  s.page_turn = m.Read8(SHARED + S_PAGE_TURN);
  s.page_frames = m.Read8(SHARED + S_PAGE_FRAMES);
  for (u32 p = 0; p < 2; ++p)
  {
    const u32 c = CURSORS + p * CURSOR_SIZE;
    s.cursors[p].x = static_cast<s16>(m.Read16(c + C_X));
    s.cursors[p].y = static_cast<s16>(m.Read16(c + C_Y));
    s.cursors[p].now = m.Read8(c + C_NOW);
    s.cursors[p].seen = m.Read8(c + C_SEEN);
  }
  return s;
}

int Write(GuestMemory& m, const State& s)
{
  if (!Mapped(m))
    return 0;
  int changed = 0;
  Put8(m, SHARED + S_GAME, s.game, &changed);
  Put8(m, SHARED + S_FOLLOWER, s.follower & 1, &changed);
  Put8(m, SHARED + S_PAGE_TURN, s.page_turn, &changed);
  Put8(m, SHARED + S_PAGE_FRAMES, s.page_frames, &changed);
  for (u32 p = 0; p < 2; ++p)
  {
    const u32 c = CURSORS + p * CURSOR_SIZE;
    Put16(m, c + C_X, static_cast<u16>(s.cursors[p].x), &changed);
    Put16(m, c + C_Y, static_cast<u16>(s.cursors[p].y), &changed);
    Put8(m, c + C_NOW, s.cursors[p].now, &changed);
    Put8(m, c + C_SEEN, s.cursors[p].seen, &changed);
  }
  Put8(m, SHARED + S_TAG, TAG_ON, &changed);
  return changed;
}

int Clear(GuestMemory& m)
{
  int changed = 0;
  if (Mapped(m) && m.Read8(SHARED + S_TAG) == TAG_ON)
    Put8(m, SHARED + S_TAG, 0, &changed);
  return changed;
}

State Start(const Layout& l)
{
  State s;
  s.game = l.game;
  for (int p = 0; p < 2; ++p)
  {
    s.cursors[p].x = l.start[p].x;
    s.cursors[p].y = l.start[p].y;
    s.cursors[p].now = HELD_ALL;
    s.cursors[p].seen = HELD_ALL;
  }
  return s;
}

u8 TakePresses(State* s, int port)
{
  if (port < 0 || port > 1)
    return 0;
  Cursor& c = s->cursors[port];
  const u8 pressed = static_cast<u8>(c.now & ~c.seen);
  c.seen = c.now;
  return pressed;
}

int Latch(GuestMemory& m, const std::array<std::optional<GCPadStatus>, 4>& raw)
{
  std::optional<State> s = Read(m);
  if (!s)
    return 0;
  const Layout* l = LayoutFor(s->game);
  if (!l || ReadSceneName(m) != "scSelStage")
    return 0;
  std::array<bool, 2> moved{};
  for (int p = 0; p < 2; ++p)
  {
    Cursor& c = s->cursors[p];
    const GCPadStatus* pad = raw[p] && raw[p]->isConnected ? &*raw[p] : nullptr;
    c.now = pad ? ButtonsOf(pad->button) : 0;
    if (!pad)
      continue;
    const int vx = Velocity(pad->stickX, *l);
    const int vy = Velocity(pad->stickY, *l);
    const int x = std::clamp(c.x + vx, int{l->bounds.x0}, int{l->bounds.x1});
    const int y = std::clamp(c.y + vy, int{l->bounds.y0}, int{l->bounds.y1});
    moved[p] = vx != 0 || vy != 0;
    c.x = static_cast<s16>(x);
    c.y = static_cast<s16>(y);
  }
  if (moved[0] != moved[1])
    s->follower = moved[0] ? 0 : 1;
  return Write(m, *s);
}

void LatchFrame(const Core::CPUThreadGuard& guard,
                const std::array<std::optional<GCPadStatus>, 4>& raw)
{
  if (Rules::ProfileRuleset() == Rules::Ruleset::None)
    return;
  GuardMemory m(guard);
  Latch(m, raw);
}

namespace
{
std::string UpperName(const View& v, int port)
{
  std::string out = v.players[port].name.empty() ? fmt::format("P{}", port + 1) :
                                                   v.players[port].name;
  for (char& c : out)
  {
    if (c >= 'a' && c <= 'z')
      c = static_cast<char>(c - 'a' + 'A');
  }
  return out;
}
}

TurnCue TurnText(const View& v, int port)
{
  TurnCue cue;
  if (port < 0 || port > 1 || !v.on || v.picked >= 0 || v.kind == 0)
    return cue;
  const View::Player& p = v.players[port];
  const View::Player& other = v.players[1 - port];
  if (!p.present)
    return cue;
  cue.local = p.local;
  const std::string name = UpperName(v, port);
  const std::string them = UpperName(v, 1 - port);
  if (v.both)
  {
    if (!Ranked::Named(p.proposal))
    {
      cue.mode = TurnCue::Mode::Acting;
      cue.title = p.local ? "YOUR PICK" : fmt::format("{} PICKS", name);
      cue.detail = p.local ? "A PICKS A STAGE OR RANDOM" : "";
      cue.seconds = v.seconds;
    }
    else if (p.local && other.present && !Ranked::Named(other.proposal))
    {
      cue.mode = TurnCue::Mode::Waiting;
      cue.title = fmt::format("WAITING FOR {}", them);
    }
    return cue;
  }
  if (v.turn != port)
  {
    if (p.local && v.turn == 1 - port && other.present)
    {
      cue.mode = TurnCue::Mode::Waiting;
      cue.title = fmt::format("WAITING FOR {}", them);
    }
    return cue;
  }
  cue.mode = TurnCue::Mode::Acting;
  cue.seconds = v.seconds;
  const int left = std::max<int>(1, v.left);
  switch (static_cast<Ranked::StepKind>(v.kind))
  {
  case Ranked::StepKind::Strike:
    cue.title = p.local ? std::string("YOUR TURN") : fmt::format("{}'S TURN", name);
    cue.detail = p.local ? fmt::format("X STRIKES A STAGE \u00b7 {} LEFT", left) :
                           fmt::format("STRIKING {}", left);
    break;
  case Ranked::StepKind::Ban:
    if (v.optional)
    {
      cue.title = p.local ? std::string("YOUR TURN") : fmt::format("{} MAY BAN {}", name, left);
      cue.detail = p.local ? fmt::format("X BANS {} \u00b7 Y SKIPS", left) : "";
    }
    else
    {
      cue.title = p.local ? std::string("YOUR TURN") : fmt::format("{}'S TURN", name);
      cue.detail = p.local ? fmt::format("X BANS A STAGE \u00b7 {} LEFT", left) :
                             fmt::format("BANNING {}", left);
    }
    break;
  case Ranked::StepKind::Pick:
  case Ranked::StepKind::Prefer:
    if (v.mk_clause)
    {
      cue.title = p.local ? std::string("YOUR PICK") : fmt::format("{} PICKS ANY STAGE", name);
      cue.detail = p.local ? "ANY STAGE \u00b7 META KNIGHT CLAUSE" : "META KNIGHT CLAUSE";
    }
    else
    {
      cue.title = p.local ? std::string("YOUR PICK") : fmt::format("{} PICKS", name);
      cue.detail = p.local ? "A PICKS THE STAGE" : "";
    }
    break;
  default:
    return TurnCue{};
  }
  return cue;
}

std::string MkClauseNote(const View& v)
{
  if (!v.on || !v.mk_clause || v.picked >= 0 || v.turn < 0 || v.turn > 1 ||
      !v.players[v.turn].present)
  {
    return "";
  }
  if (v.players[v.turn].local)
    return "YOU PICK ANY STAGE \u00b7 NO STRIKES";
  return fmt::format("{} PICKS ANY STAGE \u00b7 NO STRIKES", UpperName(v, v.turn));
}

void Publish(View view)
{
  std::lock_guard lk(s_view_lock);
  s_view = std::move(s_next);
  s_next = std::move(view);
}

View Current()
{
  std::lock_guard lk(s_view_lock);
  return s_view;
}
}
