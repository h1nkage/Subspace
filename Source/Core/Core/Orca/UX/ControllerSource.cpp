// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Orca/UX/ControllerSource.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <curl/curl.h>
#include <fmt/format.h>

#include "Common/HttpRequest.h"
#include "Common/Logging/Log.h"
#include "Common/Network.h"
#include "Common/Config/Config.h"
#include "Core/Config/MainSettings.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/Orca/Bridge.h"
#include "Core/Orca/Profile.h"
#include "Core/Orca/Status.h"
#include "Core/Orca/Session/PadCodec.h"
#include "Core/Orca/UX/Controllers.h"
#include "InputCommon/ControlReference/ControlReference.h"
#include "InputCommon/GCAdapter.h"
#include "InputCommon/LocalInput.h"

namespace Orca::UX
{
namespace
{
using Clock = std::chrono::steady_clock;

constexpr auto DETACHED_FALLBACK = std::chrono::seconds(1);
constexpr size_t TIMING_SAMPLES = 4096;
constexpr auto AGE_LOG_PERIOD = std::chrono::seconds(10);
constexpr auto HOP_FLOOR_PERIOD = std::chrono::seconds(1);

double EpochMs()
{
  return std::chrono::duration<double, std::milli>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool LoopbackBridge(std::string_view bridge)
{
  constexpr std::string_view prefix = "http://127.0.0.1:";
  if (bridge.ends_with("/"))
    bridge.remove_suffix(1);
  if (!bridge.starts_with(prefix))
    return false;
  const std::string_view port = bridge.substr(prefix.size());
  return !port.empty() && port.size() <= 5 &&
         std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::string Env(const char* name)
{
  const char* v = std::getenv(name);
  return v ? v : "";
}

class Samples
{
public:
  void Add(double value)
  {
    if (m_values.size() < TIMING_SAMPLES)
      m_values.push_back(value);
    else
      m_values[m_next++ % TIMING_SAMPLES] = value;
  }
  void Clear()
  {
    m_values.clear();
    m_next = 0;
  }
  const std::vector<double>& Values() const { return m_values; }

private:
  std::vector<double> m_values;
  size_t m_next = 0;
};

struct Percentiles
{
  double p50 = 0, p95 = 0, p99 = 0, max = 0;
};

Percentiles Summarize(std::vector<double> values)
{
  Percentiles p;
  if (values.empty())
    return p;
  std::sort(values.begin(), values.end());
  const auto at = [&values](double q) {
    return values[std::min(values.size() - 1, static_cast<size_t>(q * values.size()))];
  };
  p.p50 = at(0.50);
  p.p95 = at(0.95);
  p.p99 = at(0.99);
  p.max = values.back();
  return p;
}

struct AgeWindow
{
  Clock::time_point start{};
  u64 frames = 0;
  u64 reports = 0;
  std::vector<double> age, helper, cache, hop;

  void Add(double helper_ms, double cache_ms, double hop_ms)
  {
    if (age.size() >= TIMING_SAMPLES)
      return;
    age.push_back(helper_ms + cache_ms);
    helper.push_back(helper_ms);
    cache.push_back(cache_ms);
    if (hop_ms >= 0)
      hop.push_back(hop_ms);
  }

  std::string Describe(std::string_view over) const
  {
    std::string line = fmt::format("Orca: controller input age {}: {} frames read the adapter, {} "
                                   "with a new pad",
                                   over, frames, age.size());
    if (!age.empty())
    {
      double sum = 0;
      for (const double a : age)
        sum += a;
      const Percentiles p = Summarize(age);
      line += fmt::format("; newest report at the read: mean {:.2f}, p50 {:.1f}, p95 {:.1f}, p99 "
                          "{:.1f}, max {:.1f} ms (p50 helper {:.2f}, in Orca's cache {:.2f})",
                          sum / age.size(), p.p50, p.p95, p.p99, p.max, Summarize(helper).p50,
                          Summarize(cache).p50);
    }
    if (!hop.empty())
    {
      const Percentiles h = Summarize(hop);
      line += fmt::format("; the hop through the app, not in the age, above its fastest in the "
                          "last second: p50 {:.2f}, p95 {:.2f}, max {:.2f} ms",
                          h.p50, h.p95, h.max);
    }
    if (frames > 0)
      line += fmt::format("; {:.2f} reports per frame", static_cast<double>(reports) / frames);
    return line;
  }
};

class Stream
{
public:
  Stream(std::string bridge, std::string token)
      : m_url((bridge.ends_with("/") ? bridge.substr(0, bridge.size() - 1) : bridge) +
              "/controllers/events"),
        m_token(std::move(token))
  {
    m_thread = std::thread([this] { Run(); });
  }
  Stream() = default;
  ~Stream()
  {
    m_done = true;
    if (m_thread.joinable())
      m_thread.join();
  }
  Stream(const Stream&) = delete;
  Stream& operator=(const Stream&) = delete;

  struct Cached
  {
    std::shared_ptr<const Snapshot> snapshot;
    double age_ms = 0;
    double hop_ms = -1;
  };

  std::optional<Cached> Latest() const
  {
    std::lock_guard lk(m_lock);
    const auto now = Clock::now();
    if (!m_latest || (!m_attached && now - m_detached_at > DETACHED_FALLBACK))
      return std::nullopt;
    return Cached{m_latest, std::chrono::duration<double, std::milli>(now - m_arrived).count(),
                  m_latest_hop};
  }

  std::string NoteFrame(const Cached& cached, const LocalChoice* choice)
  {
    const auto now = Clock::now();
    std::lock_guard lk(m_lock);
    const u64 reports = m_reports - m_reports_at_frame;
    m_reports_at_frame = m_reports;
    const Snapshot& snapshot = *cached.snapshot;
    if (!choice || choice->source != PadSource::Adapter || choice->neutral ||
        snapshot.received_at <= 0)
    {
      m_last_pad.reset();
      return {};
    }
    if (m_window.frames == 0)
      m_window.start = now;
    ++m_input.frames;
    m_input.reports += reports;
    ++m_window.frames;
    m_window.reports += reports;
    const Orca::Net::Pad pad = Orca::Net::EncodePad(choice->pad);
    if (m_last_pad && *m_last_pad != pad)
    {
      const double helper = std::max(0.0, snapshot.sent_at - snapshot.received_at);
      const double cache = cached.age_ms;
      ++m_input.changes;
      m_input.age_ms += helper + cache;
      m_age.Add(helper + cache);
      m_helper.Add(helper);
      m_cache.Add(cache);
      if (cached.hop_ms >= 0)
        m_hop.Add(cached.hop_ms);
      m_window.Add(helper, cache, cached.hop_ms);
    }
    m_last_pad = pad;
    if (now - m_window.start < AGE_LOG_PERIOD)
      return {};
    const std::string line = m_window.Describe(fmt::format(
        "over {:.0f} s", std::chrono::duration<double>(now - m_window.start).count()));
    m_window = {};
    return line;
  }

  Orca::Events::InputAge Input() const
  {
    std::lock_guard lk(m_lock);
    return m_input;
  }

  std::string RunSummary() const
  {
    AgeWindow run;
    {
      std::lock_guard lk(m_lock);
      run.frames = m_input.frames;
      run.reports = m_input.reports;
      run.age = m_age.Values();
      run.helper = m_helper.Values();
      run.cache = m_cache.Values();
      run.hop = m_hop.Values();
    }
    if (run.frames == 0)
      return {};
    return run.Describe(run.age.size() < TIMING_SAMPLES ? "over the run" :
                                                          "over the run (newest 4096 changes)");
  }

  ControllerTiming Timing() const
  {
    ControllerTiming t;
    std::vector<double> transport, age, helper, cache, hop;
    {
      std::lock_guard lk(m_lock);
      t = m_timing;
      t.attached = m_attached;
      t.input = m_input;
      transport = m_samples.Values();
      age = m_age.Values();
      helper = m_helper.Values();
      cache = m_cache.Values();
      hop = m_hop.Values();
    }
    t.samples = transport.size();
    const Percentiles tp = Summarize(std::move(transport));
    t.p50_ms = tp.p50;
    t.p95_ms = tp.p95;
    t.p99_ms = tp.p99;
    t.max_ms = tp.max;
    t.age_samples = age.size();
    const Percentiles ap = Summarize(std::move(age));
    t.age_p50_ms = ap.p50;
    t.age_p95_ms = ap.p95;
    t.age_p99_ms = ap.p99;
    t.age_max_ms = ap.max;
    t.helper_p50_ms = Summarize(std::move(helper)).p50;
    t.cache_p50_ms = Summarize(std::move(cache)).p50;
    t.hop_samples = hop.size();
    const Percentiles hp = Summarize(std::move(hop));
    t.hop_p50_ms = hp.p50;
    t.hop_p95_ms = hp.p95;
    return t;
  }

  void ResetTiming()
  {
    std::lock_guard lk(m_lock);
    m_timing = {};
    m_samples.Clear();
    m_input = {};
    m_age.Clear();
    m_helper.Clear();
    m_cache.Clear();
    m_hop.Clear();
    m_window = {};
    m_last_pad.reset();
    m_reports_at_frame = m_reports;
  }

private:
  static size_t OnData(char* data, size_t size, size_t count, void* user)
  {
    auto* self = static_cast<Stream*>(user);
    if (self->m_done)
      return 0;
    for (size_t i = 0; i < size * count; ++i)
    {
      const char c = data[i];
      if (c == '\r')
      {
        self->m_after_cr = true;
        self->m_pending.push_back('\n');
        continue;
      }
      if (c == '\n' && self->m_after_cr)
      {
        self->m_after_cr = false;
        continue;
      }
      self->m_after_cr = false;
      self->m_pending.push_back(c);
    }
    if (self->m_pending.size() > 256 * 1024)
      return 0;
    size_t end;
    while ((end = self->m_pending.find("\n\n")) != std::string::npos)
    {
      std::string_view block(self->m_pending.data(), end);
      std::string payload;
      bool has_data = false;
      while (!block.empty())
      {
        const size_t nl = block.find('\n');
        const std::string_view line = block.substr(0, nl);
        block = nl == std::string_view::npos ? std::string_view{} : block.substr(nl + 1);
        if (!line.starts_with("data:"))
          continue;
        if (has_data)
          payload.push_back('\n');
        payload.append(line.substr(line.starts_with("data: ") ? 6 : 5));
        has_data = true;
      }
      if (has_data)
        self->OnEvent(payload);
      self->m_pending.erase(0, end + 2);
    }
    return size * count;
  }

  static int OnProgress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
  {
    return static_cast<Stream*>(user)->m_done ? 1 : 0;
  }

  void OnEvent(std::string_view json) { Accept(ParseSnapshotEvent(json)); }

public:
  void Accept(std::optional<Snapshot> parsed)
  {
    const auto arrived = Clock::now();
    const double arrived_epoch = EpochMs();
    std::lock_guard lk(m_lock);
    if (!parsed)
    {
      ++m_timing.refused;
      return;
    }
    if (m_latest && m_latest->session == parsed->session && parsed->sequence != 0 &&
        parsed->sequence < m_latest->sequence)
    {
      return;
    }
    if (parsed->received_at > 0 && parsed->received_at != m_last_report)
    {
      m_last_report = parsed->received_at;
      ++m_reports;
      m_samples.Add(arrived_epoch - parsed->received_at);
    }
    m_latest_hop =
        parsed->received_at > 0 ? HopAboveFloor(arrived, arrived_epoch - parsed->sent_at) : -1;
    ++m_timing.snapshots;
    m_latest = std::make_shared<const Snapshot>(std::move(*parsed));
    m_arrived = arrived;
    m_attached = true;
  }

private:
  double HopAboveFloor(Clock::time_point arrived, double trip)
  {
    while (!m_hop_floor.empty() && m_hop_floor.back().second >= trip)
      m_hop_floor.pop_back();
    m_hop_floor.emplace_back(arrived, trip);
    while (arrived - m_hop_floor.front().first > HOP_FLOOR_PERIOD)
      m_hop_floor.pop_front();
    return trip - m_hop_floor.front().second;
  }

  void Detach()
  {
    std::lock_guard lk(m_lock);
    if (m_attached)
      m_detached_at = Clock::now();
    m_attached = false;
  }

  void Run()
  {
    Orca::PrioritizeThread(Orca::LatencyThread::Controllers);
    int backoff_ms = 500;
    while (!m_done)
    {
      CURL* curl = curl_easy_init();
      if (!curl)
        return;
      m_pending.clear();
      m_after_cr = false;
      curl_slist* headers = curl_slist_append(nullptr, ("Authorization: Bearer " + m_token).c_str());
      curl_easy_setopt(curl, CURLOPT_URL, m_url.c_str());
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &OnData);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);
      curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &OnProgress);
      curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
      curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
      curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
      curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
      curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
      curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
      const CURLcode result = curl_easy_perform(curl);
      long status = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      Detach();
      if (m_done)
        return;
      if (status == 401 || status == 403 || status == 404)
      {
        ERROR_LOG_FMT(CONTROLLERINTERFACE,
                      "Orca: the desktop app refused its controller stream (HTTP {}); using "
                      "Dolphin's controller mapping",
                      status);
        return;
      }
      int wait_ms;
      if (status == 409)
        wait_ms = 1000;
      else if (status == 503)
        wait_ms = 5000;
      else if (status == 200)
        wait_ms = backoff_ms = 500;
      else
        wait_ms = backoff_ms = std::min(backoff_ms * 2, 5000);
      if (status != m_last_status || result != m_last_result)
      {
        WARN_LOG_FMT(CONTROLLERINTERFACE, "Orca: controller stream ended ({}, HTTP {})",
                     curl_easy_strerror(result), status);
        m_last_status = status;
        m_last_result = result;
      }
      for (int waited = 0; waited < wait_ms && !m_done; waited += 50)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }

  const std::string m_url;
  const std::string m_token;
  std::atomic<bool> m_done{false};
  std::string m_pending;
  bool m_after_cr = false;
  long m_last_status = -1;
  CURLcode m_last_result = CURLE_OK;

  mutable std::mutex m_lock;
  std::shared_ptr<const Snapshot> m_latest;
  Clock::time_point m_arrived{};
  double m_latest_hop = -1;
  std::deque<std::pair<Clock::time_point, double>> m_hop_floor;
  Clock::time_point m_detached_at{};
  bool m_attached = false;
  double m_last_report = 0;
  ControllerTiming m_timing;
  Samples m_samples;
  u64 m_reports = 0;
  u64 m_reports_at_frame = 0;
  Orca::Events::InputAge m_input;
  Samples m_age, m_helper, m_cache, m_hop;
  AgeWindow m_window;
  std::optional<Orca::Net::Pad> m_last_pad;

  std::thread m_thread;
};

std::mutex s_stream_lock;
std::unique_ptr<Stream> s_stream;
u64 s_bridge_token = 0;

void StopStream(bool log)
{
  Orca::Bridge::ClearControllersHandler(std::exchange(s_bridge_token, 0));
  std::unique_ptr<Stream> stream;
  {
    std::lock_guard lk(s_stream_lock);
    stream.swap(s_stream);
  }
  const std::string summary = stream && log ? stream->RunSummary() : std::string();
  stream.reset();
  if (!summary.empty())
    NOTICE_LOG_FMT(ROLLBACK, "{}", summary);
}

struct Teardown
{
  ~Teardown() { StopStream(false); }
} s_teardown;

constexpr int NO_DEVICE = -1;
std::array<std::atomic<int>, MAX_LOCAL> s_local_devices{NO_DEVICE, NO_DEVICE};
std::atomic<int> s_local_count{1};
std::array<std::atomic<bool>, MAX_LOCAL> s_assignment_known{false, false};

int PackDevice(PadSource source, int index)
{
  return (source == PadSource::Gamepad ? 1 << 16 : 0) | (index & 0xffff);
}

std::optional<LocalChoice> Choose(int ordinal, bool for_frame)
{
  std::optional<LocalChoice> choice;
  std::string age_line;
  {
    std::lock_guard lk(s_stream_lock);
    if (!s_stream)
      return std::nullopt;
    const std::optional<Stream::Cached> cached = s_stream->Latest();
    if (!cached)
      return std::nullopt;
    if (ordinal < 0 || ordinal >= MAX_LOCAL)
      return std::nullopt;
    choice = ChooseLocalPads(*cached->snapshot, cached->age_ms,
                             s_local_count.load(std::memory_order_relaxed))[ordinal];
    if (for_frame && ordinal == 0)
      age_line = s_stream->NoteFrame(*cached, choice ? &*choice : nullptr);
  }
  if (!age_line.empty())
    NOTICE_LOG_FMT(ROLLBACK, "{}", age_line);
  return choice;
}
}

void StartControllerStream(const std::string& bridge, const std::string& token)
{
  std::unique_ptr<Stream> next;
  if (!Common::OfflineReplay() && LoopbackBridge(bridge) && !token.empty())
  {
    { Common::HttpRequest init; }
    next = std::make_unique<Stream>(bridge, token);
  }
  {
    std::lock_guard lk(s_stream_lock);
    next.swap(s_stream);
  }
  next.reset();
}

void StopControllerStream()
{
  StopStream(true);
}

std::optional<GCPadStatus> LatestPad()
{
  if (const auto choice = Choose(0, false))
    return choice->pad;
  return std::nullopt;
}

std::shared_ptr<const Snapshot> LatestSnapshot(double* age_ms)
{
  std::lock_guard lk(s_stream_lock);
  if (!s_stream)
    return nullptr;
  const std::optional<Stream::Cached> cached = s_stream->Latest();
  if (!cached)
    return nullptr;
  *age_ms = cached->age_ms;
  return cached->snapshot;
}

void StartControllerStreamFromStdio()
{
  StopStream(false);
  {
    std::lock_guard lk(s_stream_lock);
    s_stream = std::make_unique<Stream>();
  }
  s_bridge_token = Orca::Bridge::SetControllersHandler([](const picojson::value& event) {
    std::optional<Snapshot> parsed = ParseSnapshotEvent(event);
    std::lock_guard lk(s_stream_lock);
    if (s_stream)
      s_stream->Accept(std::move(parsed));
  });
}

void StartControllerStreamFromEnvironment()
{
  if (Orca::Bridge::Enabled())
  {
    StartControllerStreamFromStdio();
    return;
  }
  const std::string bridge = Env("SUBSPACE_BRIDGE"), token = Env("SUBSPACE_TOKEN");
  if (bridge.empty() || token.empty())
    return;
  if (!LoopbackBridge(bridge))
  {
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "Orca: SUBSPACE_BRIDGE is not a loopback address");
    return;
  }
  StartControllerStream(bridge, token);
}

void SetLocalPadCount(int n)
{
  if (s_local_count.exchange(std::clamp(n, 1, MAX_LOCAL), std::memory_order_relaxed) !=
      std::clamp(n, 1, MAX_LOCAL))
    for (int ordinal = 0; ordinal < MAX_LOCAL; ++ordinal)
    {
      s_local_devices[ordinal].store(NO_DEVICE, std::memory_order_relaxed);
      s_assignment_known[ordinal].store(false, std::memory_order_relaxed);
    }
}

u32 OwnAdapterPorts()
{
  if (Orca::Bridge::Enabled() || !Env("SUBSPACE_BRIDGE").empty())
    return 0;
  u32 ports = 0;
  for (int i = 0; i < 4; ++i)
  {
    if (Config::GetBase(Config::GetInfoForSIDevice(i)) ==
        SerialInterface::SIDevices::SIDEVICE_WIIU_ADAPTER)
    {
      ports |= 1u << i;
    }
  }
  return ports;
}

std::optional<GCPadStatus> OwnAdapterPad()
{
  static const u32 ports = OwnAdapterPorts();
  if (ports == 0)
    return std::nullopt;
  static std::array<std::array<u8, 6>, 4> origins = [] {
    std::array<std::array<u8, 6>, 4> o;
    o.fill({128, 128, 128, 128, 0, 0});
    return o;
  }();
  int chan = -1;
  for (int i = 0; i < 4 && chan < 0; ++i)
  {
    if ((ports & (1u << i)) && GCAdapter::DeviceConnected(i))
      chan = i;
  }
  if (chan < 0)
    return NeutralPad();
  GCPadStatus raw = GCAdapter::Input(chan);
  if (raw.button & PAD_GET_ORIGIN)
  {
    origins[chan] = {raw.stickX,    raw.stickY,      raw.substickX,
                     raw.substickY, raw.triggerLeft, raw.triggerRight};
    NOTICE_LOG_FMT(CONTROLLERINTERFACE, "Subspace: GameCube adapter port {} in use", chan + 1);
  }
  AdapterPort port;
  port.port = chan;
  port.connected = true;
  port.buttons = static_cast<u16>(
      ((raw.button & PAD_BUTTON_A) ? 0x01 : 0) | ((raw.button & PAD_BUTTON_B) ? 0x02 : 0) |
      ((raw.button & PAD_BUTTON_X) ? 0x04 : 0) | ((raw.button & PAD_BUTTON_Y) ? 0x08 : 0) |
      ((raw.button & PAD_BUTTON_LEFT) ? 0x10 : 0) | ((raw.button & PAD_BUTTON_RIGHT) ? 0x20 : 0) |
      ((raw.button & PAD_BUTTON_DOWN) ? 0x40 : 0) | ((raw.button & PAD_BUTTON_UP) ? 0x80 : 0) |
      ((raw.button & PAD_BUTTON_START) ? 0x100 : 0) | ((raw.button & PAD_TRIGGER_Z) ? 0x200 : 0) |
      ((raw.button & PAD_TRIGGER_R) ? 0x400 : 0) | ((raw.button & PAD_TRIGGER_L) ? 0x800 : 0));
  port.axes = {raw.stickX, raw.stickY, raw.substickX, raw.substickY};
  port.triggers = {raw.triggerLeft, raw.triggerRight};
  port.origin = origins[chan];
  return MapAdapterPort(port);
}

std::optional<GCPadStatus> LocalPad(bool for_frame)
{
  return LocalPad(0, for_frame);
}

std::optional<GCPadStatus> LocalPad(int ordinal, bool for_frame)
{
  if (ordinal < 0 || ordinal >= MAX_LOCAL)
    return NeutralPad();
  const auto choice = Choose(ordinal, for_frame);
  if (for_frame)
  {
    const bool multiple = s_local_count.load(std::memory_order_relaxed) > 1;
    bool changed = false;
    for (int index = 0; index < (multiple ? MAX_LOCAL : 1); ++index)
    {
      const auto selected = index == ordinal ? choice : Choose(index, false);
      const int device = selected ? PackDevice(selected->source, selected->index) : NO_DEVICE;
      const int previous = s_local_devices[index].exchange(device, std::memory_order_relaxed);
      const bool known = s_assignment_known[index].exchange(true, std::memory_order_relaxed);
      changed |= !known || previous != device;
    }
    if (multiple && changed)
    {
      std::string line = "orca seats";
      for (int index = 0; index < Net::MAX_SEATS; ++index)
      {
        const auto selected = LastLocalDevice(index);
        line += fmt::format(" {}={}", index + 1, selected ?
            fmt::format("{}:{}", selected->source == PadSource::Adapter ? "adapter" : "pad",
                        selected->index) : "none");
      }
      Status::Line(line);
    }
  }
  if (!choice)
    return s_local_count.load(std::memory_order_relaxed) > 1 ?
               std::optional<GCPadStatus>(NeutralPad()) : std::nullopt;
  if (!ControlReference::GetInputGate() && !InputCommon::VirtualPadOnly())
    return NeutralPad();
  return choice->pad;
}

std::optional<LocalDevice> LastLocalDevice()
{
  return LastLocalDevice(0);
}

std::optional<LocalDevice> LastLocalDevice(int ordinal)
{
  if (ordinal < 0 || ordinal >= MAX_LOCAL)
    return std::nullopt;
  const int packed = s_local_devices[ordinal].load(std::memory_order_relaxed);
  if (packed == NO_DEVICE)
    return std::nullopt;
  return LocalDevice{(packed >> 16) ? PadSource::Gamepad : PadSource::Adapter, packed & 0xffff};
}

ControllerTiming GetControllerTiming()
{
  std::lock_guard lk(s_stream_lock);
  return s_stream ? s_stream->Timing() : ControllerTiming{};
}

void ResetControllerTiming()
{
  std::lock_guard lk(s_stream_lock);
  if (s_stream)
    s_stream->ResetTiming();
}

Orca::Events::InputAge GetInputAge()
{
  std::lock_guard lk(s_stream_lock);
  return s_stream ? s_stream->Input() : Orca::Events::InputAge{};
}
}
