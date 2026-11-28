// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Core/Orca/Engine/Display.h"
#include "Core/Orca/Engine/MatchTypes.h"
#include "Core/Orca/Engine/Regions.h"

#include "Core/Orca/Session/Events.h"
#include "Core/Orca/UX/MenuEvents.h"
#include "Core/Orca/UX/QueueApp.h"
#include "Core/Rollback/InputGate.h"
#include "InputCommon/GCPadStatus.h"

namespace Core
{
class CPUThreadGuard;
}

namespace Orca::Engine
{
class Verdicts
{
public:
  virtual ~Verdicts() = default;
  virtual std::vector<GameResult> Confirm(int confirmed, int plug_frame,
                                          std::vector<int>* starts = nullptr) = 0;
  virtual void Reset() = 0;
  virtual std::optional<SceneKind> FinalScene() const = 0;
  virtual std::optional<SceneKind> LatestScene() const = 0;
  virtual std::optional<int> ConfirmNoShow(int confirmed) = 0;
};

class HeaderHost
{
public:
  virtual ~HeaderHost() = default;
  virtual bool SetWanted(Mode mode, u8 flags = 0, std::string_view code = {}) = 0;
  virtual u64 WantedGeneration() = 0;
  virtual bool HeaderInPlace() = 0;
  virtual void MemoryReplaced() = 0;
  virtual void SetHeaderFree(bool free) = 0;
  virtual void ExpectHeader(Mode mode, std::string_view code = {}, u8 flags = 0,
                            int known_games = -1) = 0;
  virtual std::optional<std::string> TakeMismatch() = 0;
};

class MenuReads
{
public:
  virtual ~MenuReads() = default;
  virtual bool DropInHeld() = 0;
  virtual bool FightInProgress() { return false; }
  virtual void ResetFightHold() {}
  virtual bool OnTheMenus() = 0;
  virtual std::optional<UX::OnlinePick> CssPick() = 0;
  virtual bool PickStands(UX::OnlinePick pick) = 0;
  virtual std::optional<UX::OnlinePick> TakeLobbyPick() = 0;
  virtual bool FreshSupported() = 0;
  virtual FreshSeen LastSeen() = 0;
};

class CssAdapter
{
public:
  virtual ~CssAdapter() = default;
  virtual void ArmOwnPick(const UX::QueueApp::Identity& pick) = 0;
  virtual void DisarmOwnPick() = 0;
  virtual bool OwnPickArmed() = 0;
  virtual std::optional<GCPadStatus> OwnPickPad() = 0;
};

struct Manifest
{
  std::string_view id;
  Ruleset ruleset = Ruleset::None;
  bool results = false;
  bool music = false;
  int compat = 0;
};

class Module
{
public:
  virtual ~Module() = default;
  virtual std::string_view Id() const = 0;
  virtual Manifest Describe() = 0;
  virtual bool Active() = 0;
  virtual std::span<const Region> Regions() = 0;
  virtual void OnBoundary(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
                          const std::vector<Events::PortInfo>& ports, bool alone) = 0;
  virtual Rollback::InputGate::Masks Masks(const Core::CPUThreadGuard& guard) = 0;
  virtual void Latch(const Core::CPUThreadGuard& guard,
                     const std::array<std::optional<GCPadStatus>, Rollback::InputGate::PORTS>& raw) = 0;

  virtual Verdicts& Results() = 0;
  virtual HeaderHost& Header() = 0;
  virtual MenuReads& Read() = 0;
  virtual CssAdapter* Css() = 0;
  virtual void LoadOwnControls() = 0;
  virtual void SetLocalPort(int port) = 0;
  virtual Display Show() = 0;
  virtual Ruleset ProfileRuleset() = 0;
  virtual bool ScenesReadable(std::string_view running_game, u16 running_revision) = 0;
  virtual std::string SceneName(const Core::CPUThreadGuard& guard) = 0;
};

CssAdapter& NoCss();

void SetModule(Module* module);
Module* ActiveModule();

void MergeMasks(Rollback::InputGate::Masks& masks, const Rollback::InputGate::Masks& more);
}
