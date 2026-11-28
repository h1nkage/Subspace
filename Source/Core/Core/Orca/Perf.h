// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

namespace Config
{
struct Location;
}
namespace Core
{
class CPUThreadGuard;
}

namespace Orca::Perf
{
struct Rules
{
  double window_s = 15.0;
  int warm_frames = 900;
  int count_frames = 600;
  double auto_cap = 58.0;
  int auto_windows = 4;
  double rerun_ref = 0.1;
  int rerun_min = 30;
};

constexpr std::size_t LINE_BYTES = 300;
constexpr std::size_t NAME_CHARS = 40;
constexpr double CAP_MAX = 999.9;

enum class Mode : u8
{
  Solo,
  OneVsOne,
  Multi,
};
Mode ModeFor(int peers);
const char* ModeName(Mode mode);

struct Frame
{
  double interval_ms = 0;
  double sleep_ms = 0;
  double wait_ms = 0;
  double gpu_ms = 0;
  double rerun_ms = 0;
  int reruns = 0;
};
double Work(const Frame& frame);
double ForwardWork(const Frame& frame);

double Percentile(std::vector<double> values, double p);

u64 SceneCode(std::string_view name);
std::string SceneCodeName(u64 code);
struct SceneTransition
{
  u64 from = 0;
  u64 to = 0;
  double ms = 0;
  bool headless = false;
};
struct PaceWindow
{
  u64 scene = 0;
  u64 n = 0;
  std::array<double, 4> p{};
  std::array<u64, 3> h{};
  std::array<u64, 4> rb{};
  u64 rm = 0;
  u64 sc = 0;
  double sm = 0;
  u64 dropped = 0;
  bool headless = false;
};
class PaceMeter
{
public:
  static constexpr std::size_t SAMPLE_CAPACITY = 16384;
  std::optional<SceneTransition> Present(std::chrono::nanoseconds now, u64 scene, bool headless);
  void Rollback(int depth);
  void BreakInterval();
  PaceWindow Take();

private:
  PaceWindow m_window;
  std::array<double, SAMPLE_CAPACITY> m_samples{};
  std::size_t m_size = 0;
  std::optional<std::chrono::nanoseconds> m_last;
  std::optional<std::chrono::nanoseconds> m_scene_last;
  u64 m_scene = 0;
  bool m_headless = false;
};
std::string PaceJson(const PaceWindow& window);
std::string SceneJson(const SceneTransition& scene);

struct Window
{
  Mode mode = Mode::Solo;
  int n = 0;
  double cap = 0;
  double fc = 0;
  double rw = 0;
  int rv = 0;
  double w50 = 0;
  double w95 = 0;
  double g95 = 0;
  double fps = 0;
  int rbf = 0;
  int res = 0;
  bool pf = false;
  bool as = false;
  bool test = false;
  bool Counts(const Rules& rules) const;
};

class WindowMeter
{
public:
  void AddValid(double work_ms, double gpu_ms, double rerun_ms = 0, int reruns = 0);
  void AddShown(int frames);
  void AddRunning(double ms);
  void AddRerun(int frames);
  void SeePeers(int peers);
  double RunningMs() const { return m_running_ms; }
  bool Due(const Rules& rules) const { return m_running_ms >= rules.window_s * 1000; }
  Window Take();

private:
  std::vector<double> m_work;
  std::vector<double> m_gpu;
  double m_work_ms = 0;
  double m_forward_ms = 0;
  double m_rerun_ms = 0;
  int m_reruns = 0;
  double m_running_ms = 0;
  int m_shown = 0;
  int m_rerun = 0;
  int m_peers = 0;
};

std::optional<double> RerunCost(const Window& window, const Rules& rules);
double CapAtRef(const Window& window, const Rules& rules,
                std::optional<double> known_rw = std::nullopt);

class AutoSwitch
{
public:
  explicit AutoSwitch(Rules rules = {}) : m_rules(rules) {}
  bool Feed(const Window& window);
  void Restart() { m_run.clear(); }
  bool Fired() const { return m_fired; }
  double TriggerCap() const { return m_trigger_cap; }

private:
  Rules m_rules;
  std::vector<double> m_run;
  std::optional<double> m_rerun_cost;
  bool m_fired = false;
  double m_trigger_cap = 0;
};

struct Boundary
{
  bool first_run = false;
  bool excluded = false;
  bool hidden = false;
  bool held = false;
  int held_frames = 0;
  int peers = 0;
  u64 rerun_total = 0;
  std::chrono::nanoseconds paused_total{};
};

class Meter
{
public:
  struct Totals
  {
    std::chrono::nanoseconds sleep{};
    std::chrono::nanoseconds present{};
    std::chrono::nanoseconds wait{};
  };
  explicit Meter(Rules rules = {}) : m_rules(rules) {}
  std::optional<Window> OnBoundary(const Boundary& boundary, std::chrono::nanoseconds now,
                                   const Totals& totals);
  void NoteScene(std::optional<bool> in_match);
  const Rules& GetRules() const { return m_rules; }

private:
  Rules m_rules;
  std::optional<std::chrono::nanoseconds> m_last_forward;
  Totals m_totals;
  std::optional<std::chrono::nanoseconds> m_last_rerun;
  Totals m_rerun_totals;
  int m_reruns = 0;
  std::chrono::nanoseconds m_paused{};
  u64 m_rerun_total = 0;
  bool m_spoiled = false;
  bool m_scene_match = false;
  int m_match_frames = 0;
  WindowMeter m_window;
};

struct Info
{
  std::string backend;
  std::string gpu;
  std::string driver;
  std::vector<std::string> adapters;
  int adapter = 0;
  std::string cpu;
  int threads = 0;
  std::string os;
  std::string rev;
};
std::string CleanName(std::string_view name);
std::string InfoJson(const Info& info);
std::string WindowJson(const Window& window);
std::string AutoJson(double cap);

enum class GfxKey : u8
{
  Res,
  Aniso,
  Filter,
  Resample,
  Efb,
  GpuDecode,
  CpuCull,
};
struct GfxChange
{
  GfxKey key = GfxKey::Res;
  int value = 0;
  bool operator==(const GfxChange&) const = default;
};
std::optional<GfxChange> ParseGfx(std::string_view line);
Config::Location GfxLocation(GfxKey key);
std::vector<Config::Location> GfxLocations();
std::array<GfxChange, 5> PerformancePreset();
void ApplyGfx(const GfxChange& change);
bool PerformancePicture();
bool PerformanceNow();

void Ready();
bool Measuring();
void NewBoot();
bool TestRulesActive();
const Rules& ActiveRules();
void AddSleep(std::chrono::nanoseconds time);
void AddPresent(std::chrono::nanoseconds time);
void Presented(bool headless = false);
void NotePaceScene(const Core::CPUThreadGuard& guard);
void NoteRollback(int depth);
class CompileScope
{
public:
  CompileScope();
  ~CompileScope();
  CompileScope(const CompileScope&) = delete;
  CompileScope& operator=(const CompileScope&) = delete;

private:
  std::optional<std::chrono::steady_clock::time_point> m_start;
};
void AddWait(std::chrono::nanoseconds time);
class StepScope
{
public:
  StepScope();
  ~StepScope();
  StepScope(const StepScope&) = delete;
  StepScope& operator=(const StepScope&) = delete;
};
void NoteScene(std::optional<bool> in_match);
void QueueGfx(const GfxChange& change);

void OnBoundary(const Boundary& boundary);
}
