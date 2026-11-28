// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/Games/Brawl/BrawlModule.h"

#include <cstdlib>
#include <vector>

#include "Core/Orca/Engine/GuestMemory.h"
#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Games/Brawl/BrawlGame.h"
#include "Core/Orca/Games/Brawl/BrawlStages.h"
#include "Core/Orca/Games/Brawl/CharOrder.h"
#include "Core/Orca/Games/Brawl/CssTitle.h"
#include "Core/Orca/Games/Brawl/MenuText.h"
#include "Core/Orca/Games/Brawl/NameTags.h"
#include "Core/Orca/Games/Brawl/Rulesets.h"
#include "Core/Orca/Games/Brawl/NativeText.h"
#include "Core/Orca/Games/Brawl/OnlineMenu.h"
#include "Core/Orca/Games/Brawl/OnlineRules.h"
#include "Core/Orca/Games/Brawl/OnlineSeats.h"
#include "Core/Orca/Games/Brawl/PPlus/RankedPPlus.h"
#include "Core/Orca/Games/Brawl/Queue.h"
#include "Core/Orca/Games/Brawl/RankedSet.h"
#include "Core/Orca/Games/Brawl/Relabel.h"
#include "Core/Orca/Games/Brawl/Results.h"
#include "Core/Orca/Games/Brawl/StageCursors.h"
#include "Core/Orca/PanicDiag.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/UX/Kit.h"
#include "Core/Orca/UX/Probe.h"
#include "Core/Orca/UX/Rumble.h"
#include "Core/Orca/UX/RankedSteps.h"
#include "Core/PowerPC/MMU.h"

namespace Orca::UX
{
namespace
{
class BrawlVerdicts final : public Orca::Engine::Verdicts
{
public:
  std::vector<GameResult> Confirm(int confirmed, int plug_frame, std::vector<int>* starts) override
  {
    return Tracker().Confirm(confirmed, plug_frame, starts);
  }
  void Reset() override { Tracker().Reset(); }
  std::optional<Orca::Engine::SceneKind> FinalScene() const override
  {
    return Tracker().FinalScene();
  }
  std::optional<Orca::Engine::SceneKind> LatestScene() const override
  {
    return Tracker().LatestScene();
  }
  std::optional<int> ConfirmNoShow(int confirmed) override
  {
    return Rules::ConfirmNoShow(confirmed);
  }
};

class BrawlHeader final : public Orca::Engine::HeaderHost
{
public:
  bool SetWanted(Rules::Mode mode, u8 flags, std::string_view code) override
  {
    return Rules::SetWanted(mode, flags, code,
                            Rules::RoomTeamsMask(Orca::Online::RoomDoubles() &&
                                                     !Orca::Online::RoomEnded(),
                                                 Orca::Online::RoomTeams()));
  }
  u64 WantedGeneration() override { return Rules::WantedGeneration(); }
  bool HeaderInPlace() override { return Rules::HeaderInPlace(); }
  void MemoryReplaced() override { Rules::MemoryReplaced(); }
  void SetHeaderFree(bool free) override { Rules::SetHeaderFree(free); }
  void ExpectHeader(Rules::Mode mode, std::string_view code, u8 flags, int known_games) override
  {
    Rules::ExpectHeader(mode, code, flags, known_games);
  }
  std::optional<std::string> TakeMismatch() override { return Rules::TakeMismatch(); }
};

class BrawlMenuReads final : public Orca::Engine::MenuReads
{
public:
  bool DropInHeld() override { return UX::DropInHeld(); }
  bool FightInProgress() override { return UX::FightInProgress(); }
  void ResetFightHold() override { UX::ResetFightHold(); }
  bool OnTheMenus() override { return UX::OnTheMenus(); }
  std::optional<OnlinePick> CssPick() override { return UX::CssPick(); }
  bool PickStands(OnlinePick pick) override { return UX::PickStands(pick); }
  std::optional<OnlinePick> TakeLobbyPick() override { return UX::TakeLobbyPick(); }
  bool FreshSupported() override { return FreshMove::Supported(); }
  Orca::Engine::FreshSeen LastSeen() override { return FreshMove::LastSeen(); }
};

class BrawlCss final : public Orca::Engine::CssAdapter
{
public:
  void ArmOwnPick(const QueueApp::Identity& pick) override { Queue::ArmOwnPick(pick); }
  void DisarmOwnPick() override { Queue::DisarmOwnPick(); }
  bool OwnPickArmed() override { return Queue::OwnPickArmed(); }
  std::optional<GCPadStatus> OwnPickPad() override { return Queue::OwnPickPad(); }
};
}

BrawlModule& Builtin()
{
  static BrawlModule s_module;
  return s_module;
}

void BrawlModule::OnBoundary(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
                             const std::vector<Orca::Events::PortInfo>& ports, bool alone)
{
  WriteMenuText(guard);
  ReadOnlineMenu(guard, resimulating, alone);
  FriendsMove::Frame(guard, frame, resimulating, ports);
  FreshMove::Frame(guard, frame);
  RankedSet::Frame(guard, frame, resimulating);
  RankedSet::HudFrame(guard, resimulating);
  ReadResultsFrame(guard, frame);
  RankedPPlus::Frame(guard, frame, resimulating, ports, alone);
  BrawlStages::FrameHook(guard, resimulating, ports);
  if (!resimulating && Rules::ProfileRuleset() != Rules::Ruleset::None)
  {
    GuardMemory memory(guard);
    StageCursors::View view = Rules::ProfileRuleset() == Rules::Ruleset::Brawl ?
                                  BrawlStages::CursorView(memory, ports) :
                                  RankedPPlus::CursorView(memory, ports, frame);
    StageCursors::Publish(std::move(view));
  }
  ProbeFrame(guard, frame);
  if (!resimulating)
    Orca::PanicDiag::NoteScene(guard, frame);
  if (!resimulating)
  {
    for (const Orca::Events::PortInfo& p : ports)
    {
      if (!p.remote)
        Rumble::Frame(guard, p.port);
    }
  }
  Kit::SetLook(Rules::ProfileRuleset() == Rules::Ruleset::PPlus ? Kit::Look::ProjectPlus :
                                                                  Kit::Look::Brawl);
  static const int s_late = [] {
    const char* v = std::getenv("ORCA_UX_TEST_LATE_NAME");
    return v ? std::atoi(v) : -1;
  }();
  std::vector<Orca::Events::PortInfo> early;
  const bool late = s_late >= 0 && !resimulating && frame < s_late && TestKnobsAllowed();
  if (late)
  {
    early = ports;
    for (Orca::Events::PortInfo& p : early)
    {
      if (p.port != 0)
      {
        p.name.clear();
        p.controls.clear();
      }
    }
  }
  if (!Orca::Online::LocalRoom())
    NameTagsFrameHook(guard, late ? early : ports, ports, alone && !resimulating);
  u8 plugged = 0;
  for (const Orca::Events::PortInfo& p : ports)
  {
    if (p.port >= 0 && p.port < 4)
      plugged |= static_cast<u8>(1 << p.port);
  }
  Rules::OnFrame(guard, frame, resimulating, alone, plugged);
  OnlineSeats::ClearFrame(guard, frame, resimulating, plugged);
  CharOrder::Frame(guard, frame, resimulating, ports);
  Queue::Frame(guard, frame, resimulating, ports, alone);
  CssTitle::Frame(guard, resimulating, ports);
  const Relabel::Shown labels = Relabel::Frame(guard, resimulating, ports);
  NativeText::Frame(guard, resimulating, ports, Relabel::ClockInBand(labels));
  OnlineSeats::SeatsFrame(guard, frame, resimulating, plugged);
}

Rollback::InputGate::Masks BrawlModule::Masks(const Core::CPUThreadGuard& guard)
{
  Rollback::InputGate::Masks masks{};
  if (Rules::ProfileRuleset() == Rules::Ruleset::None)
    return masks;
  GuardMemory memory(guard);
  Orca::Engine::MergeMasks(masks, Rules::SessionMasks(memory));
  Orca::Engine::MergeMasks(masks, Rules::GateMasks(memory));
  Orca::Engine::MergeMasks(masks, OnlineSeats::GateMasks(memory));
  Orca::Engine::MergeMasks(masks, RankedPPlus::Masks(memory));
  BrawlStages::GateMasks(guard, &masks);
  Rollback::InputGate::Masks order = CharOrder::GateFrame(guard);
  if (Queue::OwnsStart(guard))
  {
    for (Rollback::InputGate::Mask& mask : order)
      mask.press &= static_cast<u16>(~PAD_BUTTON_START);
  }
  Orca::Engine::MergeMasks(masks, order);
  Orca::Engine::MergeMasks(masks, Queue::GateFrame(guard));
  return masks;
}

Orca::Engine::Verdicts& BrawlModule::Results()
{
  static BrawlVerdicts s_verdicts;
  return s_verdicts;
}

Orca::Engine::HeaderHost& BrawlModule::Header()
{
  static BrawlHeader s_header;
  return s_header;
}

Orca::Engine::MenuReads& BrawlModule::Read()
{
  static BrawlMenuReads s_reads;
  return s_reads;
}

Orca::Engine::CssAdapter* BrawlModule::Css()
{
  static BrawlCss s_css;
  return &s_css;
}

void BrawlModule::LoadOwnControls()
{
  UX::LoadOwnControls();
}

void BrawlModule::SetLocalPort(int port)
{
  RankedSet::SetLocalPort(port);
}

Orca::Engine::Display BrawlModule::Show()
{
  Orca::Engine::Display d;
  const u8 native = NativeText::Shown();
  d.plates = native & 0x0F;
  d.rules_bar = (native & NativeText::SHOWN_RULES) != 0;
  const Relabel::Shown words = Relabel::Current();
  d.band_clock = words.band != Relabel::Band::Game && words.band_up;
  d.line_relabelled = words.line != Relabel::LINE_GAME;
  d.legend = words.legend;
  d.tip = Queue::Tip();
  const RankedPPlus::Lines ranked = RankedPPlus::CurrentLines();
  d.ranked_set = ranked.set;
  d.ranked_turn = ranked.turn;
  d.score_line = RankedSet::CurrentScoreLine();
  d.cursors = StageCursors::Current();
  const StageCursors::View& v = d.cursors;
  d.layout = StageCursors::LayoutFor(v.game);
  if (d.layout)
  {
    for (int port = 0; port < 2; ++port)
    {
      const StageCursors::View::Player& p = v.players[port];
      if (!p.present || !Ranked::Named(p.proposal))
        continue;
      const StageCursors::Tile* tile = StageCursors::TileOf(*d.layout, p.proposal, v.page);
      d.proposals[port] = tile ? &tile->rect : nullptr;
    }
    for (int port = 0; port < 2; ++port)
      d.cues[port] = StageCursors::TurnText(v, port);
    d.mk_note = StageCursors::MkClauseNote(v);
  }
  d.picker = CharOrder::CurrentPicker();
  if (d.picker >= 0)
    d.picker_ribbon = CharOrder::CurrentPickerRibbon();
  d.dots = RankedSet::CurrentDots();
  d.hud_ports = RankedSet::HudPorts();
  d.hud_fade = RankedSet::HudFade();
  return d;
}

Orca::Engine::Manifest BrawlModule::Describe()
{
  return BrawlGame::Identify(Orca::ActiveProfile());
}

bool BrawlModule::Active()
{
  return BrawlGame::On(Describe());
}

std::span<const Orca::Engine::Region> BrawlModule::Regions()
{
  return BrawlGame::Regions();
}

Orca::Engine::Ruleset BrawlModule::ProfileRuleset()
{
  if (!Rulesets::Loaded())
    return Orca::Engine::Ruleset::None;
  const Orca::Engine::Manifest game = Describe();
  return BrawlGame::On(game) ? game.ruleset : Orca::Engine::Ruleset::None;
}

bool BrawlModule::ScenesReadable(std::string_view running_game, u16 running_revision)
{
  return BrawlGame::ScenesReadable(Orca::ActiveProfile(), running_game, running_revision);
}

std::string BrawlModule::SceneName(const Core::CPUThreadGuard& guard)
{
  const u32 manager = PowerPC::MMU::HostRead<u32>(guard, 0x805a0060);
  if (manager < 0x80000000u)
    return {};
  const u32 scene = PowerPC::MMU::HostRead<u32>(guard, manager + 0x4);
  if (scene < 0x80000000u)
    return {};
  const u32 name_ptr = PowerPC::MMU::HostRead<u32>(guard, scene);
  if (name_ptr < 0x80000000u)
    return {};
  std::string name;
  for (u32 i = 0; i < 32; ++i)
  {
    const char c = static_cast<char>(PowerPC::MMU::HostRead<u8>(guard, name_ptr + i));
    if (c == '\0')
      break;
    name.push_back(c);
  }
  return name;
}

void BrawlModule::Latch(
    const Core::CPUThreadGuard& guard,
    const std::array<std::optional<GCPadStatus>, Rollback::InputGate::PORTS>& raw)
{
  Queue::LatchFrame(guard, raw);
  StageCursors::LatchFrame(guard, raw);
}
}
