// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Core/Orca/Engine/Module.h"

namespace Orca::UX
{
class BrawlModule final : public Orca::Engine::Module
{
public:
  std::string_view Id() const override { return "brawl-builtin"; }
  Orca::Engine::Manifest Describe() override;
  bool Active() override;
  std::span<const Orca::Engine::Region> Regions() override;
  void OnBoundary(const Core::CPUThreadGuard& guard, int frame, bool resimulating,
                  const std::vector<Orca::Events::PortInfo>& ports, bool alone) override;
  Rollback::InputGate::Masks Masks(const Core::CPUThreadGuard& guard) override;
  void Latch(const Core::CPUThreadGuard& guard,
             const std::array<std::optional<GCPadStatus>, Rollback::InputGate::PORTS>& raw) override;
  Orca::Engine::Verdicts& Results() override;
  Orca::Engine::HeaderHost& Header() override;
  Orca::Engine::MenuReads& Read() override;
  Orca::Engine::CssAdapter* Css() override;
  void LoadOwnControls() override;
  void SetLocalPort(int port) override;
  Orca::Engine::Display Show() override;
  Orca::Engine::Ruleset ProfileRuleset() override;
  bool ScenesReadable(std::string_view running_game, u16 running_revision) override;
  std::string SceneName(const Core::CPUThreadGuard& guard) override;
};

BrawlModule& Builtin();
}
