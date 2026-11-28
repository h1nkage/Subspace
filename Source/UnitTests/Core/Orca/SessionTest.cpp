// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <fmt/format.h>
#include <gtest/gtest.h>
#include <picojson.h>

#include "Common/FPURoundMode.h"
#include "Common/HttpRequest.h"
#include "Common/WebSocket.h"

#include "Core/Orca/Session/Online.h"
#include "Core/Orca/Session/PadCodec.h"
#include "Core/Orca/Session/RoomResilience.h"
#include "Core/Orca/Session/Session.h"
#include "Core/Orca/Session/RelayRoom.h"
#include "Core/Rollback/OnlineMatch.h"
#include "Core/Rollback/Loopback.h"

namespace Rollback
{
namespace OnlineMatch
{
class StallBudget;
}
Orca::Net::Step StepSession(Orca::Net::Session& session, Orca::Net::Game& game, int completed_frame,
                            const std::function<Orca::Net::Pad()>& sample_local,
                            const std::function<bool()>& stopping,
                            std::chrono::microseconds stall_wait,
                            std::chrono::milliseconds give_up_after = {}, bool* gave_up = nullptr,
                            std::chrono::nanoseconds* waited = nullptr,
                            const std::function<bool(std::chrono::steady_clock::time_point)>&
                                hold_silence = {},
                            OnlineMatch::StallBudget* stall_budget = nullptr,
                            const std::function<bool(std::chrono::steady_clock::time_point)>&
                                room_recovering = {});
Orca::Net::Step StepSession(Orca::Net::Session& session, Orca::Net::Game& game, int completed_frame,
                            const std::function<Orca::Net::Pad(int)>& sample_local,
                            const std::function<bool()>& stopping,
                            std::chrono::microseconds stall_wait,
                            std::chrono::milliseconds give_up_after = {}, bool* gave_up = nullptr,
                            std::chrono::nanoseconds* waited = nullptr,
                            const std::function<bool(std::chrono::steady_clock::time_point)>&
                                hold_silence = {},
                            OnlineMatch::StallBudget* stall_budget = nullptr,
                            const std::function<bool(std::chrono::steady_clock::time_point)>&
                                room_recovering = {});
}

using namespace Orca::Net;

namespace
{
template <typename T>
T Uniform(std::mt19937& rng, T lo, T hi)
{
  const u64 span = static_cast<u64>(hi - lo) + 1;
  constexpr u64 RANGE = u64{1} << 32;
  const u64 limit = RANGE - RANGE % span;
  u64 draw;
  do
  {
    draw = static_cast<u64>(rng());
  } while (draw >= limit);
  return static_cast<T>(lo + static_cast<T>(draw % span));
}

u64 Mix(u64 h, u64 v)
{
  h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  return h * 0xff51afd7ed558ccdull;
}

class FakeGame final : public Game
{
public:
  bool Save(int frame) override
  {
    if (!m_saved_since_load.insert(frame).second)
      ++m_double_saves;
    ++m_saves;
    if (ring_slots > 0 && !m_snapshots.contains(frame))
    {
      m_ring.resize(ring_slots, -1);
      m_snapshots.erase(m_ring[m_next]);
      m_ring[m_next] = frame;
      m_next = (m_next + 1) % ring_slots;
    }
    m_snapshots[frame] = m_state;
    return true;
  }
  bool Load(int frame) override
  {
    m_saved_since_load.clear();
    const auto it = m_snapshots.find(frame);
    if (it == m_snapshots.end())
      return false;
    m_state = it->second;
    m_loads.emplace_back(frame, m_state);
    m_snapshots.erase(m_snapshots.upper_bound(frame), m_snapshots.end());
    if (ring_slots > 0)
    {
      const auto slot = std::find(m_ring.begin(), m_ring.end(), frame);
      for (int& f : m_ring)
      {
        if (f > frame)
          f = -1;
      }
      m_next = static_cast<size_t>(slot - m_ring.begin() + 1) % ring_slots;
    }
    return true;
  }
  void SetPads(int frame, const Pads& pads) override
  {
    m_history[frame] = m_state;
    m_ran_pads[frame] = pads;
    m_pads_frame = frame;
    m_pads = pads;
  }
  void SetResimulating(bool resimulating) override { m_resimulating = resimulating; }
  std::optional<u64> SnapshotChecksum(int frame) override
  {
    const auto it = m_snapshots.find(frame);
    if (it == m_snapshots.end())
      return std::nullopt;
    return it->second;
  }
  void ResetPacing() override { ++m_pacing_resets; }
  double SaveMs() const override { return save_ms; }
  double NowMs() const override { return now_ms; }

  int Run()
  {
    u64 h = Mix(m_state, static_cast<u64>(m_pads_frame));
    for (const Pad& pad : m_pads)
      for (u8 b : pad)
        h = Mix(h, b);
    m_state = h;
    if (m_corrupt_at == m_pads_frame)
      m_state ^= 1;
    return m_pads_frame;
  }

  size_t ring_slots = 0;
  double save_ms = 0;
  double now_ms = 0;
  std::vector<int> m_ring;
  size_t m_next = 0;
  std::map<int, u64> m_snapshots;
  std::set<int> m_saved_since_load;
  int m_saves = 0;
  int m_double_saves = 0;
  std::vector<std::pair<int, u64>> m_loads;
  std::map<int, u64> m_history;
  std::map<int, Pads> m_ran_pads;
  u64 m_state = 1;
  int m_pads_frame = -1;
  Pads m_pads{};
  bool m_resimulating = false;
  int m_pacing_resets = 0;
  std::optional<int> m_corrupt_at;
};

struct FakeNet
{
  struct InFlight
  {
    int deliver_at;
    Packet packet;
  };
  std::deque<InFlight> queues[2];
  int now = 0;
  int latency = 0;
  int jitter = 0;
  std::array<int, 2> extra{0, 0};
  std::mt19937 rng{1234};
  int direct_latency = -1;
  int direct_jitter = 0;
  int direct_loss_percent = 0;
  std::array<std::vector<InFlight>, 2> direct;
  int direct_delivered = 0;
  bool relay_open = true;
  std::function<void(int from, Packet* packet)> tamper;

  class End final : public Transport
  {
  public:
    End(FakeNet& net, int index) : m_net(net), m_index(index) {}
    void Send(const Packet& sent) override
    {
      Packet packet = sent;
      if (m_net.tamper)
        m_net.tamper(m_index, &packet);
      int delay = m_net.latency + m_net.extra[1 - m_index];
      if (m_net.jitter > 0)
        delay += Uniform(m_net.rng, 0, m_net.jitter);
      auto& queue = m_net.queues[1 - m_index];
      int at = m_net.now + delay;
      if (!queue.empty())
        at = std::max(at, queue.back().deliver_at);
      if (m_net.relay_open)
        queue.push_back({at, packet});
      if (m_net.direct_latency >= 0 &&
          Uniform(m_net.rng, 0, 99) >= m_net.direct_loss_percent)
      {
        Packet copy = packet;
        copy.history_seat = -1;
        copy.history_first = 0;
        copy.history.clear();
        const int wobble =
            m_net.direct_jitter > 0 ? Uniform(m_net.rng, 0, m_net.direct_jitter) : 0;
        m_net.direct[1 - m_index].push_back(
            {m_net.now + m_net.direct_latency + wobble, std::move(copy)});
      }
    }
    std::vector<Packet> Receive() override
    {
      std::vector<Packet> out;
      auto& fast = m_net.direct[m_index];
      for (auto it = fast.begin(); it != fast.end();)
      {
        if (it->deliver_at > m_net.now)
        {
          ++it;
          continue;
        }
        out.push_back(std::move(it->packet));
        it = fast.erase(it);
        ++m_net.direct_delivered;
      }
      auto& queue = m_net.queues[m_index];
      while (!queue.empty() && queue.front().deliver_at <= m_net.now)
      {
        out.push_back(queue.front().packet);
        queue.pop_front();
      }
      return out;
    }
    bool Connected() const override { return true; }

  private:
    FakeNet& m_net;
    int m_index;
  };
};

struct Player
{
  Player(const Config& config, FakeNet& net, int index)
      : game(RingGame(config)), transport(net, index), session(config, game, transport),
        seat(index)
  {
  }

  static FakeGame RingGame(const Config& config)
  {
    FakeGame game;
    game.ring_slots = static_cast<size_t>(config.max_rollback) + 2;
    return game;
  }

  Pad LocalPad(int tick) const
  {
    Pad pad{};
    if (still)
      return pad;
    const int phase = tick + seat * 37;
    pad[0] = static_cast<u8>((phase / 7) % 4);
    pad[1] = static_cast<u8>((phase / 11) % 3 == 0 ? 0x80 : 0);
    if (tag_ticks)
    {
      pad[2] = static_cast<u8>(tick & 0xff);
      pad[3] = static_cast<u8>(tick >> 8);
      pad[4] = 1;
    }
    return pad;
  }

  void Expect(const Pad& pad, int input_delay, int frame, int delay)
  {
    if (expected.empty())
    {
      for (int f = 0; f < input_delay; ++f)
        expected[f] = Pad{};
    }
    const int target = frame + delay;
    const int newest = expected.empty() ? -1 : expected.rbegin()->first;
    if (target <= newest)
    {
      if (delay < last_delay)
        ++dropped_after_lower;
      last_delay = delay;
      return;
    }
    if (target > newest + 1 && newest >= 0)
      ++filled;
    const Pad held = newest >= 0 ? expected.rbegin()->second : Pad{};
    for (int f = newest + 1; f < target; ++f)
      expected[f] = held;
    expected[target] = pad;
    last_delay = delay;
  }

  static int TickOf(const Pad& pad) { return pad[4] ? pad[2] | pad[3] << 8 : -1; }

  void Tick(int tick)
  {
    game.now_ms = tick * 1000.0 / 60;
    const int frame = session.CurrentFrame();
    const int delay = session.Delay();
    for (int guard = 0; guard < 64; ++guard)
    {
      const Step step = session.OnBoundary(completed, LocalPad(tick));
      if (guard == 0)
      {
        first_steps.push_back({tick, frame, step.kind});
        if (model_input_delay >= 0 && step.kind != StepKind::Wait)
          Expect(LocalPad(tick), model_input_delay, frame, delay);
      }
      if (step.kind == StepKind::Ended || step.kind == StepKind::Stall ||
          step.kind == StepKind::Wait)
        return;
      const bool new_frame = step.kind == StepKind::Run && !session.Resimulating();
      completed = game.Run();
      if (new_frame)
        return;
    }
    FAIL() << "too many boundaries in one tick";
  }

  FakeGame game;
  FakeNet::End transport;
  Session session;
  int seat;
  int completed = -1;
  bool tag_ticks = false;
  bool still = false;
  int model_input_delay = -1;
  std::map<int, Pad> expected;
  int last_delay = -1;
  int filled = 0;
  int dropped_after_lower = 0;
  struct First
  {
    int tick;
    int frame;
    StepKind kind;
  };
  std::vector<First> first_steps;
};

struct Match
{
  explicit Match(Config config, int latency, int jitter)
  {
    net.latency = latency;
    net.jitter = jitter;
    config.local_seat = 0;
    a = std::make_unique<Player>(config, net, 0);
    config.local_seat = 1;
    b = std::make_unique<Player>(config, net, 1);
  }
  void Play(int ticks)
  {
    const int end = played + ticks;
    for (int t = played; t < end; ++t)
    {
      net.now = t;
      if (rtt_samples && t % 30 == 0)
      {
        for (int i = 0; i < 2; ++i)
        {
          int rtt = net.latency;
          if (net.jitter > 0)
            rtt += Uniform(rtt_rng, 0, net.jitter);
          (i == 0 ? a : b)->session.OnRoundTrip(relay_rtt_ms[i] >= 0 ? relay_rtt_ms[i] :
                                                                        rtt * 1000 / 60);
        }
      }
      a->Tick(t);
      b->Tick(t);
      if (a_extra_every > 0 && t % a_extra_every == a_extra_every - 1)
        a->Tick(t);
    }
    played = end;
  }
  int CompareConfirmed() const
  {
    const int confirmed = std::min(a->session.GetStats().confirmed_frame,
                                   b->session.GetStats().confirmed_frame);
    int compared = 0;
    for (const auto& [frame, state] : a->game.m_history)
    {
      if (frame > confirmed)
        continue;
      const auto other = b->game.m_history.find(frame);
      if (other == b->game.m_history.end())
        continue;
      EXPECT_EQ(state, other->second) << "frame " << frame;
      ++compared;
    }
    return compared;
  }

  static int CheckSamples(const Player& from, const Player& to)
  {
    const int confirmed = to.session.GetStats().confirmed_frame;
    int checked = 0;
    for (const auto& [frame, pad] : from.expected)
    {
      if (frame > confirmed)
        break;
      const auto ran = to.game.m_ran_pads.find(frame);
      if (ran == to.game.m_ran_pads.end())
        continue;
      EXPECT_EQ(ran->second[from.seat], pad) << "seat " << from.seat << " frame " << frame;
      ++checked;
    }
    return checked;
  }

  FakeNet net;
  std::unique_ptr<Player> a, b;
  bool rtt_samples = false;
  std::array<int, 2> relay_rtt_ms{-1, -1};
  std::mt19937 rtt_rng{99};
  int a_extra_every = 0;
  int played = 0;
};

int Settled(const Player& p)
{
  return std::min(p.session.GetStats().confirmed_frame, p.session.CurrentFrame() - 1);
}

void ExpectClean(const Match& match)
{
  for (const Player* p : {match.a.get(), match.b.get()})
    EXPECT_TRUE(p->session.Error().empty()) << p->session.Error();
}

class RelayMatch
{
public:
  using Micros = s64;
  static constexpr Micros MS = 1000;
  static constexpr Micros FRAME_US = 16667;
  static constexpr int CATCH_UP_BEHIND = 12;

  struct Side
  {
    int hold_out_ms = 0;
    int hold_in_ms = 0;
    int jitter_out_ms = 0;
    int jitter_in_ms = 0;
    int hold_from_s = -1'000'000;
    int hold_to_s = 1'000'000;
    int relay_ms = 8;
    double speed = 1.0;
    Micros rerun_us = 0;
    int input_every = 7;
    int send_gap_ms = 10;
    int hitch_every_s = 0;
    int hitch_burst = 0;
    bool report_hitches = true;
    int spike_every_s = 0;
    int spike_ms = 0;
    int direct_ms = -1;
    int direct_jitter_ms = 0;
    int direct_loss_percent = 0;
    std::array<int, 2> spike_gap_ms{0, 0};
    std::array<int, 2> spike_len_ms{0, 0};
    int spike_mean_gap_ms = 0;
    int spike_from_s = 0;
    int spike_to_s = 1'000'000;
    bool spike_in_only = false;
  };

  struct DropIn
  {
    int join_s = 30;
    int keyframe_frames = 261;
    int load_ms = 1900;
    Micros catch_up_frame_us = 3000;
  };

  RelayMatch(Config config, const Side& a, const Side& b,
             std::optional<DropIn> drop_in = std::nullopt, u32 seed = 0)
      : m_drop_in(drop_in), m_rng(4321 + seed)
  {
    for (int i = 0; i < 2; ++i)
    {
      config.local_seat = i;
      m_peers[i] = std::make_unique<Peer>(config, i == 0 ? a : b, i, seed);
      if (!m_drop_in)
        m_peers[i]->session.emplace(config, m_peers[i]->game, m_peers[i]->link);
    }
  }

  void Play(int frames)
  {
    for (; m_now < END; m_now += MS)
    {
      if (Running(0) && Running(1) && SessionOf(0).CurrentFrame() >= frames &&
          SessionOf(1).CurrentFrame() >= frames)
      {
        return;
      }
      if (!TickOnce())
        return;
    }
  }

  void PlaySeconds(int seconds)
  {
    const Micros until = m_now + seconds * 1000 * MS;
    for (; m_now < until && m_now < END; m_now += MS)
    {
      if (!TickOnce())
        return;
    }
  }

  bool Running(int i) const { return m_peers[i]->session.has_value(); }
  const Session& SessionOf(int i) const { return *m_peers[i]->session; }
  const FakeGame& GameOf(int i) const { return m_peers[i]->game; }
  int HitchesOf(int i) const { return m_peers[i]->hitches_reported; }
  int DirectReceivedBy(int i) const { return m_peers[i]->direct_received; }
  int MaxDelayOf(int i) const { return m_peers[i]->max_delay; }
  void MarkDelays()
  {
    for (const auto& p : m_peers)
      p->max_delay = p->session ? p->session->Delay() : 0;
  }
  const std::vector<std::string>& NotesOf(int i) const { return m_peers[i]->notes; }
  int PluggedFrame() const { return m_plugged_frame; }

  int CompareConfirmed() const
  {
    const int confirmed = std::min(SessionOf(0).GetStats().confirmed_frame,
                                   SessionOf(1).GetStats().confirmed_frame);
    int compared = 0;
    for (const auto& [frame, state] : GameOf(0).m_history)
    {
      const auto other = GameOf(1).m_history.find(frame);
      if (frame > confirmed || other == GameOf(1).m_history.end())
        continue;
      EXPECT_EQ(state, other->second) << "frame " << frame;
      ++compared;
    }
    return compared;
  }

  void ExpectClean() const
  {
    for (int i = 0; i < 2; ++i)
    {
      ASSERT_TRUE(Running(i)) << "side " << i << " never started";
      EXPECT_TRUE(SessionOf(i).Error().empty()) << SessionOf(i).Error();
    }
  }

  void Print(const char* name) const
  {
    for (int i = 0; i < 2; ++i)
    {
      if (!Running(i))
        continue;
      const Stats& s = SessionOf(i).GetStats();
      std::printf("[ %s %c ] rollbacks %d, re-run %d, deepest %d, stalls %d (%d counted, %d a "
                  "frame would spare), waits %d, delay %d (+%d -%d reverted %d, most %d), "
                  "advantage %.2f, round trip %d ms, hitches %d\n",
                  name, 'A' + i, s.rollbacks, s.resimulated_frames, s.deepest_rollback, s.stalls,
                  s.counted_stalls, s.spared_stalls, s.waits, s.delay, s.delay_raises,
                  s.delay_lowers, s.delay_reverts, m_peers[i]->max_delay, s.frame_advantage,
                  m_peers[i]->rtt, m_peers[i]->hitches_reported);
      if (std::getenv("ORCA_TEST_DELAY_NOTES"))
      {
        for (const std::string& note : m_peers[i]->notes)
          std::printf("    %c: %s\n", 'A' + i, note.c_str());
      }
    }
  }

private:
  static constexpr Micros END = 900'000 * MS;

  struct Wire
  {
    Micros due;
    int from;
    int to;
    Micros ping_sent;
    std::string text;
  };

  struct Link final : Transport
  {
    void Send(const Packet& packet) override
    {
      CoalescePacket(&outgoing, packet);
      if (direct)
        direct_out.push_back(packet);
    }
    std::vector<Packet> Receive() override { return std::exchange(inbox, {}); }
    bool Connected() const override { return true; }
    std::optional<Packet> outgoing;
    std::vector<Packet> inbox;
    bool direct = false;
    std::vector<Packet> direct_out;
  };

  struct Peer
  {
    Peer(const Config& config, const Side& s, int index, u32 seed)
        : side(s), seat(index), period(static_cast<Micros>(std::llround(FRAME_US / s.speed))),
          start(static_cast<Micros>(s.relay_ms + s.hold_in_ms) * MS)
    {
      game.ring_slots = static_cast<size_t>(config.max_rollback) + 2;
      link.direct = s.direct_ms >= 0;
      next_call = start;
      pace = start;
      std::mt19937 rng(77 + index + seed * 13);
      const auto hitch = [&rng](Micros at) {
        return std::pair{at, Uniform(rng, 5, 20) * FRAME_US};
      };
      for (int i = 0; i < s.hitch_burst; ++i)
      {
        const Micros at = start + Uniform<Micros>(rng, 0, 10'000 * MS);
        hitches.push_back(hitch(at));
      }
      for (Micros at = start; s.hitch_every_s > 0 && at < END;)
      {
        at += Uniform<Micros>(rng, s.hitch_every_s * 500 * MS, s.hitch_every_s * 1500 * MS);
        hitches.push_back(hitch(at));
      }
      std::sort(hitches.begin(), hitches.end());
      for (Micros at = start; s.spike_every_s > 0 && at < END;)
      {
        at += Uniform<Micros>(rng, s.spike_every_s * 900 * MS, s.spike_every_s * 1100 * MS);
        spikes.emplace_back(at, at + s.spike_ms * MS);
      }
      for (Micros at = start; s.spike_gap_ms[1] > 0 && at < END;)
      {
        at += Uniform<Micros>(rng, s.spike_gap_ms[0], s.spike_gap_ms[1]) * MS;
        const Micros length = Uniform<Micros>(rng, s.spike_len_ms[0], s.spike_len_ms[1]) * MS;
        if (at >= s.spike_from_s * 1000 * MS && at < s.spike_to_s * 1000 * MS)
          spikes.emplace_back(at, at + length);
      }
      for (Micros at = start; s.spike_mean_gap_ms > 0 && at < END;)
      {
        do
          at += MS;
        while (Uniform(rng, 0, s.spike_mean_gap_ms - 1) != 0);
        if (!spikes.empty())
          at = std::max(at, spikes.back().second);
        const Micros length = Uniform<Micros>(rng, s.spike_len_ms[0], s.spike_len_ms[1]) * MS;
        if (at >= s.spike_from_s * 1000 * MS && at < s.spike_to_s * 1000 * MS)
          spikes.emplace_back(at, at + length);
      }
    }
    Side side;
    FakeGame game;
    Link link;
    std::optional<Session> session;
    int seat;
    Micros period;
    Micros start;
    int completed = -1;
    Micros next_call;
    Micros pace;
    bool in_step = false;
    bool waited = false;
    int resets_before = 0;
    Micros last_send = -1'000'000 * MS;
    Micros last_ping = -1'000'000 * MS;
    int rtt = -1;
    u32 rtt_sequence = 0;
    u32 fed_sequence = 0;
    std::vector<int> rtt_history;
    bool seeded = false;
    std::deque<Wire> held_out, uplink, downlink, held_in;
    std::vector<Wire> direct_in;
    int direct_received = 0;
    std::vector<std::pair<Micros, Micros>> hitches;
    size_t next_hitch = 0;
    std::optional<Micros> last_return;
    int hitches_reported = 0;
    std::vector<std::pair<Micros, Micros>> spikes;
    int solo_frame = 0;
    std::vector<Pads> solo_log;
    bool catching_up = false;
    int max_delay = 0;
    std::vector<std::string> notes;
  };

  Peer& At(int i) { return *m_peers[i]; }

  bool TickOnce()
  {
    if (::testing::Test::HasFatalFailure())
      return false;
    StartDropIn();
    MoveMessages();
    for (int i = 0; i < 2; ++i)
    {
      RoomThread(i);
      Emulate(i);
    }
    return true;
  }

  Micros Hold(const Peer& p, int ms, int jitter_ms)
  {
    if (m_now < p.side.hold_from_s * 1000 * MS || m_now >= p.side.hold_to_s * 1000 * MS)
      return 0;
    const int extra = jitter_ms > 0 ? Uniform(m_rng, 0, jitter_ms) : 0;
    return static_cast<Micros>(ms + extra) * MS;
  }

  static Micros Radio(const Peer& p, Micros now)
  {
    for (const auto& [from, to] : p.spikes)
    {
      if (now < from)
        break;
      if (now < to)
        return to;
    }
    return now;
  }

  static void Enter(std::deque<Wire>& queue, Wire wire, Micros due)
  {
    wire.due = queue.empty() ? due : std::max(due, queue.back().due);
    queue.push_back(std::move(wire));
  }

  void MoveDirect()
  {
    for (int i = 0; i < 2; ++i)
    {
      Peer& from = At(i);
      for (Packet& packet : std::exchange(from.link.direct_out, {}))
      {
        if (Uniform(m_direct_rng, 0, 99) < from.side.direct_loss_percent)
          continue;
        packet.history_seat = -1;
        packet.history_first = 0;
        packet.history.clear();
        const int extra = from.side.direct_jitter_ms > 0 ?
                              Uniform(m_direct_rng, 0, from.side.direct_jitter_ms) :
                              0;
        At(1 - i).direct_in.push_back({Radio(from, m_now) + (from.side.direct_ms + extra) * MS, i,
                                       1 - i, 0, EncodePacket(packet)});
      }
    }
    for (int i = 0; i < 2; ++i)
    {
      Peer& p = At(i);
      if (Radio(p, m_now) != m_now)
        continue;
      for (auto it = p.direct_in.begin(); it != p.direct_in.end();)
      {
        if (it->due > m_now)
        {
          ++it;
          continue;
        }
        Packet packet;
        ASSERT_TRUE(DecodePacket(it->text, &packet)) << it->text;
        p.link.inbox.push_back(std::move(packet));
        ++p.direct_received;
        it = p.direct_in.erase(it);
      }
    }
  }

  static Pad PadAt(const Peer& p, Micros at)
  {
    const int tick = static_cast<int>(at / FRAME_US) + p.seat * 37;
    const int every = p.side.input_every;
    Pad pad{};
    pad[0] = static_cast<u8>((tick / every) % 4);
    pad[1] = static_cast<u8>((tick * 7 / (every * 11)) % 3 == 0 ? 0x80 : 0);
    return pad;
  }

  static Config DropInConfig(const Peer& p, int start_frame)
  {
    Config config;
    config.seats = MAX_SEATS;
    config.local_seat = p.seat;
    config.authority_seat = 0;
    config.start_frame = start_frame;
    std::array<SeatPlan, MAX_SEATS> plan{};
    plan[0] = {0, NEVER, 0};
    config.plan = plan;
    return config;
  }

  void StartDropIn()
  {
    if (!m_drop_in)
      return;
    Peer& host = At(0);
    Peer& friend_ = At(1);
    const Micros arrive = host.start + m_drop_in->join_s * 1000 * MS;
    if (m_keyframe < 0 && m_now >= arrive)
      m_keyframe = host.solo_frame;
    const Micros host_starts = arrive + m_drop_in->keyframe_frames * FRAME_US;
    if (!host.session && m_now >= host_starts)
    {
      const int next = host.solo_frame;
      Config config = DropInConfig(host, next);
      config.start_pad = host.solo_log.empty() ? Pad{} : host.solo_log.back()[0];
      host.session.emplace(config, host.game, host.link);
      host.session->SetLog(m_keyframe, std::vector<Pads>(host.solo_log.begin() + m_keyframe,
                                                         host.solo_log.end()));
      host.session->AddPeer(1, next, m_keyframe);
      host.completed = next - 1;
      host.in_step = false;
    }
    if (host.session && !friend_.session &&
        m_now >= host_starts + static_cast<Micros>(m_drop_in->load_ms) * MS)
    {
      const auto keyframe = host.game.m_history.find(m_keyframe);
      ASSERT_NE(keyframe, host.game.m_history.end()) << "no keyframe " << m_keyframe;
      friend_.game.m_state = keyframe->second;
      friend_.session.emplace(DropInConfig(friend_, m_keyframe), friend_.game, friend_.link);
      friend_.completed = m_keyframe - 1;
      friend_.next_call = friend_.pace = m_now;
      friend_.catching_up = true;
    }
  }

  void RunSolo(Peer& p)
  {
    while (p.next_call <= m_now)
    {
      const int f = p.solo_frame++;
      Pads pads;
      pads.fill(UNPLUGGED_PAD);
      pads[0] = PadAt(p, p.next_call);
      p.game.SetPads(f, pads);
      p.game.Run();
      p.solo_log.push_back(pads);
      p.next_call += p.period;
      p.pace = p.next_call;
    }
  }

  void MoveMessages()
  {
    MoveDirect();
    const auto due = [this](std::deque<Wire>& q) { return !q.empty() && q.front().due <= m_now; };
    const auto pop = [](std::deque<Wire>& q) {
      Wire w = std::move(q.front());
      q.pop_front();
      return w;
    };
    for (int i = 0; i < 2; ++i)
    {
      while (due(At(i).held_out))
      {
        const Micros sent = At(i).side.spike_in_only ? m_now : Radio(At(i), m_now);
        Enter(At(i).uplink, pop(At(i).held_out), sent + At(i).side.relay_ms * MS);
      }
    }
    for (int i = 0; i < 2; ++i)
    {
      while (due(At(i).uplink))
      {
        Wire w = pop(At(i).uplink);
        const int to = w.to;
        Enter(At(to).downlink, std::move(w), m_now + At(to).side.relay_ms * MS);
      }
    }
    for (int i = 0; i < 2; ++i)
    {
      Peer& p = At(i);
      while (due(p.downlink))
      {
        Enter(p.held_in, pop(p.downlink),
              Radio(p, m_now) + Hold(p, p.side.hold_in_ms, p.side.jitter_in_ms));
      }
      while (due(p.held_in))
      {
        const Wire w = pop(p.held_in);
        if (w.from == i)
        {
          p.rtt = static_cast<int>((m_now - w.ping_sent) / MS);
          ++p.rtt_sequence;
          p.rtt_history.push_back(p.rtt);
          continue;
        }
        if (!p.session)
          continue;
        Packet packet;
        ASSERT_TRUE(DecodePacket(w.text, &packet)) << w.text;
        p.link.inbox.push_back(std::move(packet));
      }
    }
  }

  void RoomThread(int i)
  {
    Peer& p = At(i);
    if (m_now - p.last_ping >= 1000 * MS)
    {
      p.last_ping = m_now;
      Enter(p.held_out, {0, i, i, m_now, {}},
            m_now + Hold(p, p.side.hold_out_ms, p.side.jitter_out_ms));
    }
    if (p.link.outgoing && m_now - p.last_send >= p.side.send_gap_ms * MS)
    {
      p.last_send = m_now;
      Enter(p.held_out, {0, i, 1 - i, 0, EncodePacket(*p.link.outgoing)},
            m_now + Hold(p, p.side.hold_out_ms, p.side.jitter_out_ms));
      p.link.outgoing.reset();
    }
  }

  void Emulate(int i)
  {
    Peer& p = At(i);
    if (!p.session)
    {
      if (m_drop_in && i == 0)
        RunSolo(p);
      return;
    }
    Session& session = *p.session;
    for (int guard = 0; p.next_call <= m_now; ++guard)
    {
      ASSERT_LT(guard, 64) << "side " << i << " ran away";
      const Micros at = p.next_call;
      if (!p.in_step)
      {
        p.in_step = true;
        p.waited = false;
        p.resets_before = p.game.m_pacing_resets;
        if (p.side.report_hitches && !p.catching_up && p.last_return &&
            at - *p.last_return > 2 * FRAME_US)
        {
          session.OnLocalHitch();
          ++p.hitches_reported;
        }
        if (!p.seeded)
        {
          p.seeded = true;
          const size_t n = p.rtt_history.size();
          const size_t keep = static_cast<size_t>(RTT_SAMPLES);
          for (size_t k = n > keep ? n - keep : 0; k < n; ++k)
            session.OnRoundTrip(p.rtt_history[k]);
          p.fed_sequence = p.rtt_sequence;
        }
        else if (p.rtt >= 0 && p.rtt_sequence != p.fed_sequence)
        {
          p.fed_sequence = p.rtt_sequence;
          session.OnRoundTrip(p.rtt);
        }
      }
      p.game.now_ms = static_cast<double>(at) / MS;
      const Step step = session.OnBoundary(p.completed, PadAt(p, at));
      for (std::string& note : session.TakeDelayNotes())
        p.notes.push_back(std::move(note));
      if (step.kind == StepKind::Wait)
      {
        p.waited = true;
        p.next_call = at + FRAME_US;
        continue;
      }
      if (step.kind == StepKind::Stall)
      {
        p.next_call = at + MS;
        continue;
      }
      if (step.kind == StepKind::Ended)
      {
        p.next_call = std::numeric_limits<Micros>::max();
        return;
      }
      p.in_step = false;
      p.last_return = at;
      bool reset = p.waited || p.game.m_pacing_resets != p.resets_before;
      p.completed = p.game.Run();
      p.max_delay = std::max(p.max_delay, session.Delay());
      if (session.Resimulating())
      {
        p.next_call = at + p.side.rerun_us;
        continue;
      }
      if (m_drop_in && i == 1)
      {
        const bool was = p.catching_up;
        p.catching_up = session.CatchingUp() ||
                        session.AuthorityFrame() - step.frame > CATCH_UP_BEHIND;
        reset = reset || (was && !p.catching_up);
        if (m_plugged_frame < 0 && session.Plugged(1, step.frame))
          m_plugged_frame = step.frame;
        if (p.catching_up)
        {
          p.next_call = at + m_drop_in->catch_up_frame_us;
          continue;
        }
      }
      p.pace = std::max((reset ? at : p.pace) + p.period, at - 100 * MS);
      p.next_call = std::max(p.pace, at);
      if (p.next_hitch < p.hitches.size() && p.hitches[p.next_hitch].first <= at)
        p.next_call += p.hitches[p.next_hitch++].second;
    }
  }

  std::array<std::unique_ptr<Peer>, 2> m_peers;
  std::optional<DropIn> m_drop_in;
  int m_keyframe = -1;
  int m_plugged_frame = -1;
  Micros m_now = -3000 * MS;
  std::mt19937 m_rng;
  std::mt19937 m_direct_rng{8765};
};
}

TEST(OrcaSession, NoLatencyNeverRollsBack)
{
  Match match(Config{}, 0, 0);
  match.Play(600);
  EXPECT_TRUE(match.a->session.Error().empty()) << match.a->session.Error();
  EXPECT_TRUE(match.b->session.Error().empty()) << match.b->session.Error();
  EXPECT_EQ(match.a->session.GetStats().rollbacks, 0);
  EXPECT_EQ(match.b->session.GetStats().rollbacks, 0);
  EXPECT_GT(match.CompareConfirmed(), 500);
  EXPECT_GT(match.a->session.GetStats().checksums_matched, 5);
}

TEST(OrcaSession, SavesOnlyFramesItMayRollBackTo)
{
  Match match(Config{}, 1, 0);
  match.Play(600);
  EXPECT_TRUE(match.a->session.Error().empty()) << match.a->session.Error();
  EXPECT_EQ(match.a->session.GetStats().rollbacks, 0);
  EXPECT_LE(match.a->game.m_snapshots.size(), 20u);
  EXPECT_GT(match.a->session.GetStats().checksums_matched, 5);
}

TEST(OrcaSession, LatencyRollsBackAndStaysInSync)
{
  Match match(Config{}, 4, 3);
  match.Play(1200);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_TRUE(p->session.Error().empty()) << p->session.Error();
    EXPECT_GT(p->session.GetStats().rollbacks, 10);
    EXPECT_LE(p->session.GetStats().deepest_rollback, Config{}.max_rollback);
    EXPECT_GT(p->session.GetStats().checksums_matched, 10);
  }
  EXPECT_GT(match.CompareConfirmed(), 1000);
}

TEST(OrcaSession, LatencyBeyondTheWindowStalls)
{
  Config config;
  config.max_rollback = 4;
  Match match(config, 10, 2);
  match.Play(900);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_TRUE(p->session.Error().empty()) << p->session.Error();
    EXPECT_GT(p->session.GetStats().stalls, 0);
    EXPECT_GT(p->game.m_pacing_resets, 0);
    EXPECT_LE(p->session.GetStats().deepest_rollback, config.max_rollback);
  }
  EXPECT_GT(match.CompareConfirmed(), 300);
}

TEST(OrcaSession, InputsHeldBackAreFoundLate)
{
  constexpr int HELD = 5;
  Match match(Config{}, 1, 0);
  match.net.tamper = [](int from, Packet* packet) {
    if (from != 1 || packet->pads.empty())
      return;
    const int last = packet->current_frame - HELD;
    const int keep = last - packet->first_frame + 1;
    if (keep <= 0)
    {
      packet->pads.clear();
      packet->first_frame = 0;
    }
    else if (keep < static_cast<int>(packet->pads.size()))
    {
      packet->pads.resize(static_cast<size_t>(keep));
    }
  };
  const auto guessing = [&match](int ticks) {
    int most = 0;
    for (int i = 0; i < ticks; ++i)
    {
      match.Play(1);
      most = std::max(most, match.a->session.CurrentFrame() - 1 -
                                match.a->session.GetStats().confirmed_frame);
    }
    return most;
  };
  const int early = guessing(150);
  EXPECT_GE(early, HELD) << "the late player's inputs come HELD frames late";
  EXPECT_FALSE(match.a->session.LateSeat(1));
  match.Play(600);
  ExpectClean(match);
  EXPECT_TRUE(match.a->session.LateSeat(1));
  EXPECT_FALSE(match.b->session.LateSeat(0));
  EXPECT_EQ(match.a->session.GetStats().late_seats, 1);
  const int late = guessing(600);
  EXPECT_LE(late, 3) << "before it was found late: " << early;
  ExpectClean(match);
  EXPECT_GT(match.CompareConfirmed(), 1000);

  for (const auto& [latency, jitter, direct_loss] :
       {std::tuple{4, 3, -1}, std::tuple{12, 6, -1}, std::tuple{6, 4, 25}})
  {
    SCOPED_TRACE(fmt::format("latency {} jitter {} direct loss {}", latency, jitter, direct_loss));
    Match honest(Config{}, latency, jitter);
    if (direct_loss >= 0)
    {
      honest.net.direct_latency = 1;
      honest.net.direct_jitter = 2;
      honest.net.direct_loss_percent = direct_loss;
    }
    honest.Play(1800);
    ExpectClean(honest);
    EXPECT_EQ(honest.a->session.GetStats().late_seats, 0);
    EXPECT_EQ(honest.b->session.GetStats().late_seats, 0);
  }
}

TEST(OrcaSession, RerunOnlyWithASnapshotInReach)
{
  {
    Match match(Config{}, 1, 0);
    match.Play(100);
    Session& session = match.a->session;
    ASSERT_GT(session.CurrentFrame() % 60, Config{}.max_rollback + MAX_SNAPSHOT_EVERY);
    EXPECT_FALSE(session.RerunFrom(session.CurrentFrame() - 2));
    EXPECT_FALSE(session.RerunFrom(session.CurrentFrame() - Config{}.max_rollback - 1));
    EXPECT_TRUE(session.RerunFrom(session.CurrentFrame()));
    match.Play(200);
    EXPECT_TRUE(session.Error().empty()) << session.Error();
    EXPECT_EQ(session.GetStats().rollbacks, 0);
    EXPECT_GT(match.CompareConfirmed(), 250);
  }
  {
    Match match(Config{}, 4, 0);
    match.Play(300);
    Session& session = match.a->session;
    const int rollbacks = session.GetStats().rollbacks;
    EXPECT_TRUE(session.RerunFrom(session.CurrentFrame() - 2));
    match.Play(1);
    EXPECT_GT(session.GetStats().rollbacks, rollbacks);
    match.Play(300);
    EXPECT_TRUE(session.Error().empty()) << session.Error();
    EXPECT_TRUE(match.b->session.Error().empty()) << match.b->session.Error();
    EXPECT_GT(match.CompareConfirmed(), 500);
  }
}

TEST(OrcaSession, DetectsDesync)
{
  Match match(Config{}, 3, 1);
  match.b->game.m_corrupt_at = 100;
  match.Play(600);
  const bool detected = match.a->session.Error().find("Desync") != std::string::npos ||
                        match.b->session.Error().find("Desync") != std::string::npos;
  EXPECT_TRUE(detected) << match.a->session.Error() << " / " << match.b->session.Error();
}

TEST(OrcaPadCodec, NeutralIsZeroAndRoundTrips)
{
  EXPECT_EQ(EncodePad(GCPadStatus{}), Pad{});

  GCPadStatus status;
  status.button = PAD_BUTTON_A | PAD_TRIGGER_Z | PAD_GET_ORIGIN | PAD_USE_ORIGIN;
  status.stickX = 0;
  status.stickY = 255;
  status.substickX = 37;
  status.substickY = 200;
  status.triggerLeft = 255;
  status.triggerRight = 12;
  status.analogA = 99;
  const GCPadStatus back = DecodePad(EncodePad(status));
  EXPECT_EQ(back.button, PAD_BUTTON_A | PAD_TRIGGER_Z);
  EXPECT_EQ(back.stickX, 0);
  EXPECT_EQ(back.stickY, 255);
  EXPECT_EQ(back.substickX, 37);
  EXPECT_EQ(back.substickY, 200);
  EXPECT_EQ(back.triggerLeft, 255);
  EXPECT_EQ(back.triggerRight, 12);
  EXPECT_EQ(back.analogA, 0);
  EXPECT_TRUE(back.isConnected);
}

TEST(OrcaSession, RejectsHostilePackets)
{
  struct Inject final : Transport
  {
    void Send(const Packet&) override {}
    std::vector<Packet> Receive() override { return std::exchange(queue, {}); }
    bool Connected() const override { return true; }
    std::vector<Packet> queue;
  };
  const auto run = [](Packet bad) {
    FakeGame game;
    Inject net;
    Config config;
    Session session(config, game, net);
    int completed = -1;
    for (int f = 0; f < 3; ++f)
    {
      session.OnBoundary(completed, Pad{});
      completed = game.Run();
    }
    bad.seat = 1;
    net.queue.push_back(bad);
    session.OnBoundary(completed, Pad{});
    return session.Error();
  };
  Packet huge_frame;
  huge_frame.first_frame = 2147483000;
  huge_frame.pads.resize(1);
  EXPECT_NE(run(huge_frame).find("invalid packet"), std::string::npos);

  Packet many_pads;
  many_pads.first_frame = 0;
  many_pads.pads.resize(100000);
  EXPECT_NE(run(many_pads).find("invalid packet"), std::string::npos);

  Packet negative;
  negative.first_frame = -5;
  negative.pads.resize(1);
  EXPECT_NE(run(negative).find("invalid packet"), std::string::npos);

  Packet fine;
  fine.first_frame = 0;
  fine.pads.resize(4);
  EXPECT_TRUE(run(fine).empty());
}

TEST(OrcaLive, WebSocketReachesARoom)
{
  const char* live = std::getenv("ORCA_LIVE_TEST");
  if (!live || std::string(live) != "1")
    GTEST_SKIP() << "set ORCA_LIVE_TEST=1";
  if (!std::getenv("ORCA_SITE"))
    GTEST_SKIP() << "set ORCA_SITE to the room service to test against";
  const std::string site = std::getenv("ORCA_SITE");

  Common::HttpRequest http(std::chrono::seconds(15));
  const std::string body = R"({"dev":"orca-live-test","player_id":"orca-live-0000000001","name":"Orca","players":2,"mode":"orca-v1","room":"orcalive1",)"
                           R"("lobby":{"version":1,"minPlayers":2,"maxLocalPlayers":1,"compatibility":"orca-test"}})";
  const auto response = http.Post(site + "/api/multiplayer/ticket", body,
                                  {{"Content-Type", "application/json"}},
                                  Common::HttpRequest::AllowedReturnCodes::All);
  ASSERT_TRUE(response.has_value()) << "no response from " << site;
  const std::string text(response->begin(), response->end());
  ASSERT_EQ(http.GetLastResponseCode(), 200) << text;
  picojson::value json;
  ASSERT_TRUE(picojson::parse(json, text).empty()) << text;
  const std::string url = json.get("url").to_str();
  const std::string ticket = json.get("ticket").to_str();
  std::string ws_url = url;
  if (ws_url.starts_with("https://"))
    ws_url.replace(0, 8, "wss://");
  else if (ws_url.starts_with("http://"))
    ws_url.replace(0, 7, "ws://");
  ws_url += "/room/orcalive1?ticket=" + http.EscapeComponent(ticket);

  Common::WebSocket ws;
  std::string error;
  ASSERT_TRUE(ws.Connect(ws_url, std::chrono::seconds(10), &error)) << error;
  std::optional<std::string> first;
  for (int i = 0; i < 20 && !first; ++i)
    first = ws.ReceiveText(std::chrono::milliseconds(500));
  ASSERT_TRUE(first.has_value()) << ws.Error();
  picojson::value welcome;
  ASSERT_TRUE(picojson::parse(welcome, *first).empty()) << *first;
  EXPECT_EQ(welcome.get("t").to_str(), "welcome") << *first;
  EXPECT_TRUE(ws.SendText(R"({"t":"ping","at":1})", std::chrono::seconds(2))) << ws.Error();
  bool pong = false;
  for (int i = 0; i < 20 && !pong; ++i)
  {
    if (auto m = ws.ReceiveText(std::chrono::milliseconds(500)))
      pong = m->find("\"pong\"") != std::string::npos;
  }
  EXPECT_TRUE(pong);
  ws.Close();
}

TEST(OrcaSessionDelay, DeepRollbacksAloneRaiseNothing)
{
  Match match(Config{}, 6, 2);
  match.Play(1200);
  ExpectClean(match);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    const Stats& stats = p->session.GetStats();
    EXPECT_GT(stats.rollbacks, 100);
    EXPECT_GE(stats.deepest_rollback, 4);
    EXPECT_EQ(stats.counted_stalls, 0);
    EXPECT_EQ(stats.delay_raises, 0);
    EXPECT_EQ(p->session.Delay(), 2);
  }
  EXPECT_GT(match.CompareConfirmed(), 1000);
}

TEST(OrcaSessionDelay, StaysPutWhenQuietOrDisabled)
{
  Match quiet(Config{}, 1, 0);
  quiet.Play(1200);
  ExpectClean(quiet);
  EXPECT_EQ(quiet.a->session.Delay(), 2);
  EXPECT_EQ(quiet.a->session.GetStats().delay_raises, 0);

  Config fixed;
  fixed.max_delay = fixed.input_delay;
  Match off(fixed, 6, 2);
  off.Play(1200);
  ExpectClean(off);
  EXPECT_EQ(off.a->session.Delay(), 2);
  EXPECT_EQ(off.a->session.GetStats().delay_raises, 0);
  EXPECT_GT(off.CompareConfirmed(), 1000);
}

TEST(OrcaSessionDelay, SteadyHighRoundTripRaises)
{
  Match near_match(Config{}, 3, 0);
  near_match.rtt_samples = true;
  near_match.Play(600);
  ExpectClean(near_match);
  for (const Player* p : {near_match.a.get(), near_match.b.get()})
  {
    EXPECT_EQ(p->session.Delay(), 2);
    EXPECT_EQ(p->session.GetStats().delay_raises, 0);
  }

  Match far_match(Config{}, 9, 0);
  far_match.rtt_samples = true;
  far_match.Play(50);
  for (const Player* p : {far_match.a.get(), far_match.b.get()})
    EXPECT_EQ(p->session.Delay(), 2);
  far_match.Play(130);
  for (const Player* p : {far_match.a.get(), far_match.b.get()})
  {
    EXPECT_EQ(p->session.Delay(), 4);
    EXPECT_EQ(p->session.GetStats().delay_raises, 1);
  }
  far_match.Play(1200);
  ExpectClean(far_match);
  for (const Player* p : {far_match.a.get(), far_match.b.get()})
  {
    const Stats& stats = p->session.GetStats();
    EXPECT_EQ(p->session.Delay(), 4);
    EXPECT_EQ(stats.delay_raises, 1);
    EXPECT_EQ(stats.delay_lowers, 0);
    EXPECT_LE(stats.stalls, 1);
    EXPECT_GT(stats.rollbacks, 0);
    EXPECT_LE(stats.deepest_rollback, Config{}.max_rollback);
  }
  EXPECT_GT(far_match.CompareConfirmed(), 1200);

  Match huge(Config{}, 0, 0);
  huge.a->session.OnRoundTrip(1000);
  huge.Play(120);
  EXPECT_EQ(huge.a->session.Delay(), Config{}.input_delay);
  huge.a->session.OnRoundTrip(1000);
  huge.a->session.OnRoundTrip(1000);
  huge.Play(60);
  EXPECT_EQ(huge.a->session.Delay(), Config{}.input_delay);
  for (int i = 0; i < 3; ++i)
    huge.b->session.OnRoundTrip(1000);
  huge.Play(2);
  EXPECT_EQ(huge.a->session.Delay(), Config{}.max_delay);
  EXPECT_EQ(huge.b->session.Delay(), Config{}.max_delay);
}

TEST(OrcaSessionDelay, LinkRaisesAtOnceAndFallsAFrameAtATime)
{
  Match match(Config{}, 5, 1);
  match.rtt_samples = true;
  match.relay_rtt_ms = {250, 250};
  for (Player* p : {match.a.get(), match.b.get()})
  {
    p->tag_ticks = true;
    p->model_input_delay = Config{}.input_delay;
  }
  match.Play(180);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_EQ(p->session.Delay(), Config{}.max_delay);
    EXPECT_EQ(p->session.GetStats().delay_raises, 1);
    EXPECT_EQ(p->filled, 1);
  }
  match.relay_rtt_ms = {16, 16};
  match.Play(3000);
  ExpectClean(match);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    const Stats& stats = p->session.GetStats();
    EXPECT_EQ(p->session.Delay(), 2);
    EXPECT_EQ(stats.delay_raises, 1);
    EXPECT_EQ(stats.delay_lowers, 4);
    EXPECT_EQ(stats.delay_reverts, 0);
    EXPECT_EQ(stats.stalls, 0);
    EXPECT_EQ(p->filled, 1);
    EXPECT_EQ(p->dropped_after_lower, 4);
  }
  EXPECT_GT(Match::CheckSamples(*match.a, *match.b), 2900);
  EXPECT_GT(Match::CheckSamples(*match.b, *match.a), 2900);
  EXPECT_GT(match.CompareConfirmed(), 2900);
}

TEST(OrcaSessionDelay, StepsDownOnlyWithHeadroomAndUndoesAWrongOne)
{
  Match match(Config{}, 10, 0);
  match.Play(1800);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    const Stats& stats = p->session.GetStats();
    EXPECT_EQ(p->session.Delay(), 3);
    EXPECT_EQ(stats.delay_raises, 1);
    EXPECT_EQ(stats.delay_lowers, 0);
  }
  const int stalls = match.a->session.GetStats().stalls;

  match.net.latency = 8;
  match.Play(60 * 15);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_EQ(p->session.Delay(), 3);
    EXPECT_EQ(p->session.GetStats().delay_lowers, 0);
  }

  match.net.latency = 7;
  for (int i = 0; i < 20 && (match.a->session.GetStats().delay_lowers == 0 ||
                             match.b->session.GetStats().delay_lowers == 0);
       ++i)
  {
    match.Play(60);
  }
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_EQ(p->session.Delay(), 2);
    EXPECT_EQ(p->session.GetStats().delay_lowers, 1);
  }
  EXPECT_EQ(match.a->session.GetStats().stalls, stalls);

  match.net.latency = 10;
  for (int i = 0; i < 9 && (match.a->session.GetStats().delay_reverts == 0 ||
                            match.b->session.GetStats().delay_reverts == 0);
       ++i)
  {
    match.Play(60);
  }
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    const Stats& stats = p->session.GetStats();
    EXPECT_EQ(p->session.Delay(), 3);
    EXPECT_EQ(stats.delay_raises, 1);
    EXPECT_EQ(stats.delay_lowers, 1);
    EXPECT_EQ(stats.delay_reverts, 1);
  }
  match.net.latency = 7;
  match.Play(60 * 8);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_EQ(p->session.Delay(), 3);
    EXPECT_EQ(p->session.GetStats().delay_lowers, 1);
  }
  match.Play(60 * 5);
  ExpectClean(match);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_EQ(p->session.Delay(), 2);
    EXPECT_EQ(p->session.GetStats().delay_lowers, 2);
    EXPECT_EQ(p->session.GetStats().delay_reverts, 1);
  }
  EXPECT_GT(match.CompareConfirmed(), 3000);
}

TEST(OrcaSessionTimeSync, FastSideWaits)
{
  Config config;
  config.max_delay = config.input_delay;
  const auto play = [&config](bool time_sync, int extra_every, bool tagged = false) {
    config.time_sync = time_sync;
    auto match = std::make_unique<Match>(config, 3, 1);
    match->rtt_samples = true;
    match->a_extra_every = extra_every;
    for (Player* p : {match->a.get(), match->b.get()})
    {
      p->tag_ticks = tagged;
      p->model_input_delay = tagged ? config.input_delay : -1;
    }
    match->Play(2400);
    ExpectClean(*match);
    EXPECT_GT(match->CompareConfirmed(), 2000);
    return match;
  };
  const auto synced = play(true, 40);
  const auto unsynced = play(false, 40);
  const Stats& a = synced->a->session.GetStats();
  const Stats& b = synced->b->session.GetStats();
  EXPECT_GT(a.waits, 30);
  EXPECT_LE(a.waits, 2500 / 30);
  EXPECT_EQ(b.waits, 0);
  EXPECT_EQ(unsynced->a->session.GetStats().waits, 0);
  const Stats& ua = unsynced->a->session.GetStats();
  const Stats& ub = unsynced->b->session.GetStats();
  EXPECT_GT(ua.stalls, 30);
  EXPECT_EQ(a.stalls, 0);
  EXPECT_EQ(b.stalls, 0);
  EXPECT_LT(a.frame_advantage, 2.0);
  EXPECT_LT((a.resimulated_frames + b.resimulated_frames) * 3,
            (ua.resimulated_frames + ub.resimulated_frames) * 2);

  const auto even = play(true, 0);
  EXPECT_EQ(even->a->session.GetStats().waits, 0);
  EXPECT_EQ(even->b->session.GetStats().waits, 0);

  const auto tagged = play(true, 40, true);
  const Player& ta = *tagged->a;
  const Player& tb = *tagged->b;
  const int delay = config.input_delay;
  EXPECT_GT(ta.session.GetStats().waits, 30);
  EXPECT_GE(ta.game.m_pacing_resets, ta.session.GetStats().waits);
  EXPECT_EQ(ta.session.GetStats().stalls, 0);
  EXPECT_GT(Match::CheckSamples(ta, tb), 2000);
  EXPECT_GT(Match::CheckSamples(tb, ta), 2000);
  int waits_checked = 0;
  for (size_t i = 0; i + 1 < ta.first_steps.size(); ++i)
  {
    const Player::First& wait = ta.first_steps[i];
    const Player::First& after = ta.first_steps[i + 1];
    if (wait.kind != StepKind::Wait || wait.frame + delay > Settled(tb))
      continue;
    ASSERT_EQ(after.frame, wait.frame);
    EXPECT_EQ(tb.game.m_ran_pads.at(wait.frame + delay)[0], ta.LocalPad(after.tick))
        << "wait at tick " << wait.tick;
    ++waits_checked;
  }
  EXPECT_GT(waits_checked, 30);
  for (int f = delay + 1; f <= Settled(ta); ++f)
  {
    EXPECT_EQ(Player::TickOf(ta.game.m_ran_pads.at(f)[1]),
              Player::TickOf(ta.game.m_ran_pads.at(f - 1)[1]) + 1)
        << "frame " << f;
  }
}

TEST(OrcaSessionTimeSync, StepSessionReadsThePadAgainAfterAWait)
{
  Config config;
  config.max_delay = config.input_delay;
  config.time_sync = true;
  Match match(config, 3, 1);
  for (Player* p : {match.a.get(), match.b.get()})
    p->tag_ticks = true;
  Player& a = *match.a;
  Player& b = *match.b;
  struct Held
  {
    int frame;
    int tick_after;
  };
  std::vector<Held> held;
  bool skip_next = false;
  const auto step_a = [&](int tick) {
    if (std::exchange(skip_next, false))
      return;
    for (int guard = 0; guard < 64; ++guard)
    {
      const int frame = a.session.CurrentFrame();
      const int waits_before = a.session.GetStats().waits;
      int reads = 0;
      int asked = 0;
      const Step step = Rollback::StepSession(
          a.session, a.game, a.completed, [&] { return a.LocalPad(tick + reads++); },
          [&] { return ++asked > a.session.GetStats().waits - waits_before; },
          std::chrono::microseconds{0});
      if (a.session.GetStats().waits > waits_before)
      {
        held.push_back({frame, tick + 1});
        skip_next = true;
      }
      if (step.kind == StepKind::Ended)
      {
        ASSERT_TRUE(a.session.Error().empty()) << a.session.Error();
        return;
      }
      const bool new_frame = step.kind == StepKind::Run && !a.session.Resimulating();
      a.completed = a.game.Run();
      if (new_frame)
        return;
    }
    FAIL() << "too many boundaries in one tick";
  };
  std::mt19937 rtt_rng{99};
  for (int t = 0; t < 1200; ++t)
  {
    match.net.now = t;
    if (t % 30 == 0)
    {
      for (Player* p : {&a, &b})
      {
        const int rtt = match.net.latency + Uniform(rtt_rng, 0, match.net.jitter);
        p->session.OnRoundTrip(rtt * 1000 / 60);
      }
    }
    step_a(t);
    b.Tick(t);
    if (t % 40 == 39)
      step_a(t);
  }
  ExpectClean(match);
  EXPECT_GT(match.CompareConfirmed(), 1000);
  EXPECT_GT(a.session.GetStats().waits, 5);
  EXPECT_EQ(static_cast<int>(held.size()), a.session.GetStats().waits);
  int checked = 0;
  const int delay = config.input_delay;
  for (const Held& h : held)
  {
    if (h.frame + delay > Settled(b))
      continue;
    const Pad ran = b.game.m_ran_pads.at(h.frame + delay)[0];
    EXPECT_EQ(Player::TickOf(ran), h.tick_after) << "wait at frame " << h.frame;
    EXPECT_EQ(ran, a.LocalPad(h.tick_after)) << "wait at frame " << h.frame;
    ++checked;
  }
  EXPECT_GT(checked, 5);
}

TEST(OrcaSessionTimeSync, UnequalRelayRoundTrips)
{
  Config config;
  Match match(config, 7, 0);
  match.rtt_samples = true;
  match.relay_rtt_ms = {17, 227};
  match.Play(30);
  EXPECT_EQ(match.a->session.Delay(), 2);
  EXPECT_EQ(match.b->session.Delay(), 2);
  match.Play(150);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_EQ(p->session.Delay(), 3);
    EXPECT_EQ(p->session.GetStats().delay_raises, 1);
  }
  match.Play(1620);
  ExpectClean(match);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_EQ(p->session.GetStats().waits, 0);
    EXPECT_LT(std::abs(p->session.GetStats().frame_advantage), 1.0);
  }
  EXPECT_GT(match.CompareConfirmed(), 1600);
}

namespace
{
void ExpectNoFrameSavedTwice(int snapshot_every)
{
  SCOPED_TRACE(::testing::Message() << "snapshot every " << snapshot_every);
  Config stalling;
  stalling.snapshot_every = snapshot_every;
  stalling.max_rollback = 4;
  Match stalls(stalling, 10, 2);
  stalls.Play(900);
  Config waiting;
  waiting.snapshot_every = snapshot_every;
  waiting.max_delay = waiting.input_delay;
  Match waits(waiting, 3, 1);
  waits.rtt_samples = true;
  waits.a_extra_every = 40;
  waits.Play(1200);
  for (const Match* match : {&stalls, &waits})
  {
    ExpectClean(*match);
    for (const Player* p : {match->a.get(), match->b.get()})
    {
      EXPECT_GT(p->game.m_saves, 100 / snapshot_every);
      EXPECT_EQ(p->game.m_double_saves, 0);
      EXPECT_EQ(p->session.GetStats().saves, p->game.m_saves);
    }
    EXPECT_GT(match->CompareConfirmed(), 300);
  }
  EXPECT_GT(stalls.a->session.GetStats().stalls, 0);
  EXPECT_GT(waits.a->session.GetStats().waits, 0);
}
}

TEST(OrcaSession, NeverSavesAFrameTwice)
{
  for (int k = 1; k <= MAX_SNAPSHOT_EVERY; ++k)
    ExpectNoFrameSavedTwice(k);
}

TEST(OrcaSessionDelay, FixedDelaySamplesLandDelayFramesLater)
{
  for (const int delay : {2, 0})
  {
    Config config;
    config.input_delay = delay;
    config.max_delay = delay;
    Match match(config, 1, 0);
    for (Player* p : {match.a.get(), match.b.get()})
      p->tag_ticks = true;
    match.Play(300);
    ExpectClean(match);
    for (const Player* from : {match.a.get(), match.b.get()})
    {
      const Player* to = from == match.a.get() ? match.b.get() : match.a.get();
      EXPECT_EQ(from->session.GetStats().stalls, 0);
      EXPECT_EQ(from->session.GetStats().waits, 0);
      const int confirmed = Settled(*to);
      ASSERT_GT(confirmed, 250);
      for (int f = 0; f <= confirmed; ++f)
      {
        const Pad& ran = to->game.m_ran_pads.at(f)[from->seat];
        EXPECT_EQ(ran, f < delay ? Pad{} : from->LocalPad(f - delay))
            << "delay " << delay << " seat " << from->seat << " frame " << f;
      }
    }
    EXPECT_GT(match.CompareConfirmed(), 250);
  }
}

TEST(OrcaSessionDelay, RaiseDuringAStall)
{
  Config config;
  config.max_rollback = 2;
  config.time_sync = false;
  Match match(config, 6, 0);
  for (Player* p : {match.a.get(), match.b.get()})
    p->tag_ticks = true;
  Player& a = *match.a;
  while (a.first_steps.empty() || a.first_steps.back().kind != StepKind::Stall)
  {
    match.Play(1);
    ASSERT_LT(match.played, 10);
  }
  const Player::First stall = a.first_steps.back();
  a.session.SetFixedDelay(6);
  ASSERT_EQ(a.session.Delay(), 6);
  match.Play(1);
  const Player::First next = a.first_steps.back();
  ASSERT_EQ(next.kind, StepKind::Stall);
  ASSERT_EQ(next.frame, stall.frame);
  match.Play(300);
  ExpectClean(match);
  const auto ran = [&](int f) { return match.b->game.m_ran_pads.at(f)[0]; };
  EXPECT_EQ(ran(stall.frame + 2), a.LocalPad(stall.tick));
  for (int f = stall.frame + 3; f < stall.frame + 6; ++f)
    EXPECT_EQ(ran(f), a.LocalPad(stall.tick)) << "frame " << f;
  EXPECT_EQ(ran(stall.frame + 6), a.LocalPad(next.tick));
  EXPECT_GT(match.CompareConfirmed(), 200);
}

TEST(OrcaSession, HostilePeerTelemetryIsBounded)
{
  struct Hostile final : Transport
  {
    void Send(const Packet&) override {}
    std::vector<Packet> Receive() override
    {
      Packet packet;
      packet.seat = 1;
      packet.first_frame = next;
      const int up_to = session->CurrentFrame() + 4;
      for (; next <= up_to; ++next)
        packet.pads.push_back(Pad{});
      packet.ack = {session->CurrentFrame(), -1, -1, -1};
      packet.current_frame = flip ? 0 : session->CurrentFrame() + MAX_FRAMES_AHEAD;
      flip = !flip;
      packet.resimulated = INT_MAX;
      packet.advantage = INT_MIN;
      packet.rtt = INT_MAX;
      packet.stalls = INT_MAX;
      packet.hitches = INT_MAX;
      return {packet};
    }
    bool Connected() const override { return true; }
    Session* session = nullptr;
    int next = 0;
    bool flip = false;
  };
  FakeGame game;
  Hostile net;
  Config config;
  Session session(config, game, net);
  net.session = &session;
  session.OnRoundTrip(30);
  int completed = -1;
  int frames = 0;
  for (int i = 0; i < 1200; ++i)
  {
    const Step step = session.OnBoundary(completed, Pad{});
    ASSERT_NE(step.kind, StepKind::Ended) << session.Error();
    if (step.kind == StepKind::Run || step.kind == StepKind::Rollback)
    {
      completed = game.Run();
      ++frames;
    }
  }
  const Stats& stats = session.GetStats();
  EXPECT_TRUE(session.Error().empty()) << session.Error();
  EXPECT_EQ(stats.stalls, 0);
  EXPECT_LE(stats.delay, config.max_delay);
  EXPECT_LE(session.Delay(), config.max_delay);
  EXPECT_GT(stats.waits, 0);
  EXPECT_LE(stats.waits, frames / 30 + 1);
}

TEST(OrcaSession, DepthOneRollbacksLoadTheCorrectedState)
{
  const auto remote = [](int f) { return Pad{static_cast<u8>(f % 5), static_cast<u8>(f % 3)}; };
  struct Late final : Transport
  {
    explicit Late(std::function<Pad(int)> remote_pad) : pad(std::move(remote_pad)) {}
    void Send(const Packet&) override {}
    std::vector<Packet> Receive() override
    {
      Packet packet;
      packet.seat = 1;
      packet.first_frame = next;
      for (; next < session->CurrentFrame(); ++next)
        packet.pads.push_back(pad(next));
      packet.current_frame = session->CurrentFrame();
      return {packet};
    }
    bool Connected() const override { return true; }
    std::function<Pad(int)> pad;
    Session* session = nullptr;
    int next = 0;
  };
  FakeGame game;
  Late net(remote);
  Config config;
  config.time_sync = false;
  Session session(config, game, net);
  net.session = &session;
  int completed = -1;
  for (int i = 0; i < 1000; ++i)
  {
    const Step step = session.OnBoundary(completed, Pad{});
    ASSERT_NE(step.kind, StepKind::Ended) << session.Error();
    if (step.kind == StepKind::Run || step.kind == StepKind::Rollback)
      completed = game.Run();
  }
  const Stats& stats = session.GetStats();
  EXPECT_GT(stats.rollbacks, 300);
  EXPECT_EQ(stats.deepest_rollback, 1);

  FakeGame straight;
  for (int f = 0; f <= stats.confirmed_frame; ++f)
  {
    straight.SetPads(f, Pads{Pad{}, remote(f)});
    straight.Run();
  }
  for (const auto& [frame, state] : game.m_loads)
  {
    if (frame <= stats.confirmed_frame)
      EXPECT_EQ(state, straight.m_history.at(frame)) << "load of frame " << frame;
  }
  int compared = 0;
  for (int f = 0; f <= stats.confirmed_frame; ++f, ++compared)
    EXPECT_EQ(game.m_history.at(f), straight.m_history.at(f)) << "frame " << f;
  EXPECT_GT(compared, 300);
}

namespace
{
void LateBurst(Match& match, bool hitch = false, int late = 10)
{
  if (hitch)
    match.b->session.OnLocalHitch();
  match.net.extra[0] = late;
  match.Play(1);
  match.net.extra[0] = 0;
}
}

TEST(OrcaSessionDelay, StallsARaiseWouldSpareRaiseThenDecay)
{
  {
    Match match(Config{}, 1, 0);
    match.Play(700);
    for (int i = 0; i < 20; ++i)
    {
      LateBurst(match, false, 14);
      match.Play(299);
    }
    ExpectClean(match);
    EXPECT_EQ(match.a->session.GetStats().counted_stalls, 20);
    EXPECT_EQ(match.a->session.GetStats().spared_stalls, 0);
    for (const Player* p : {match.a.get(), match.b.get()})
    {
      EXPECT_EQ(p->session.Delay(), 2);
      EXPECT_EQ(p->session.GetStats().delay_raises, 0);
    }
  }
  Match match(Config{}, 1, 0);
  match.Play(700);
  for (int i = 0; i < 15; ++i)
  {
    LateBurst(match, false, 10);
    match.Play(119);
  }
  EXPECT_EQ(match.a->session.GetStats().counted_stalls, 15);
  EXPECT_EQ(match.a->session.GetStats().spared_stalls, 0);
  EXPECT_EQ(match.b->session.GetStats().stalls, 0);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_EQ(p->session.Delay(), 2);
    EXPECT_EQ(p->session.GetStats().delay_raises, 0);
  }
  LateBurst(match, false, 10);
  match.Play(59);
  EXPECT_EQ(match.b->session.Delay(), 3);
  match.Play(60);
  EXPECT_EQ(match.b->session.Delay(), 4);
  EXPECT_EQ(match.b->session.GetStats().delay_raises, 2);
  EXPECT_EQ(match.a->session.Delay(), 2);
  EXPECT_EQ(match.a->session.GetStats().delay_raises, 0);
  match.Play(60 * 3);
  EXPECT_EQ(match.b->session.Delay(), 4);
  match.Play(60 * 5);
  ExpectClean(match);
  EXPECT_EQ(match.b->session.Delay(), 2);
  EXPECT_EQ(match.b->session.GetStats().delay_lowers, 2);
  EXPECT_EQ(match.b->session.GetStats().delay_reverts, 0);
  EXPECT_EQ(match.a->session.GetStats().delay_lowers, 0);
  EXPECT_GT(match.CompareConfirmed(), 2800);
}

TEST(OrcaSessionDelay, OneHiccupBothWaysRaisesNothing)
{
  Match match(Config{}, 1, 0);
  match.Play(700);
  match.net.extra = {10, 10};
  match.Play(1);
  match.net.extra = {0, 0};
  match.Play(299);
  ExpectClean(match);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_EQ(p->session.GetStats().stalls, 1);
    EXPECT_EQ(p->session.GetStats().delay_raises, 0);
  }
}

TEST(OrcaSessionDelay, StallsOnAPeerHitchRaiseNothing)
{
  Match match(Config{}, 1, 0);
  match.Play(700);
  for (int i = 0; i < 4; ++i)
  {
    LateBurst(match, true);
    match.Play(59);
  }
  match.Play(120);
  ExpectClean(match);
  EXPECT_EQ(match.a->session.GetStats().stalls, 4);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_EQ(p->session.Delay(), 2);
    EXPECT_EQ(p->session.GetStats().delay_raises, 0);
  }
  EXPECT_GT(match.CompareConfirmed(), 800);
}

TEST(OrcaSessionDelay, ALoadHitchCountsApartAndItsStallsDont)
{
  Match match(Config{}, 1, 0);
  match.Play(700);
  match.a->session.OnLoadHitch();
  LateBurst(match, false, 14);
  match.Play(120);
  ExpectClean(match);
  const auto a = match.a->session.GetStats();
  EXPECT_EQ(a.stalls, 1);
  EXPECT_EQ(a.counted_stalls, 0);
  EXPECT_EQ(a.hitches, 0);
  EXPECT_EQ(a.load_hitches, 1);
  EXPECT_EQ(match.b->session.GetStats().peer_hitches, 0);
}

TEST(OrcaSessionDelay, NoStallCountsWhileSettling)
{
  Match match(Config{}, 1, 0);
  match.Play(60);
  for (int i = 0; i < 5; ++i)
  {
    LateBurst(match, false, 14);
    match.Play(59);
  }
  match.Play(60);
  ExpectClean(match);
  ASSERT_LT(match.b->session.CurrentFrame(), 600);
  EXPECT_EQ(match.a->session.GetStats().stalls, 5);
  EXPECT_EQ(match.a->session.GetStats().counted_stalls, 0);
  for (const Player* p : {match.a.get(), match.b.get()})
    EXPECT_EQ(p->session.GetStats().delay_raises, 0);
  match.Play(300);
  ASSERT_GT(match.b->session.CurrentFrame(), 600);
  for (int i = 0; i < 16; ++i)
  {
    LateBurst(match, false, 10);
    match.Play(119);
  }
  ExpectClean(match);
  EXPECT_EQ(match.a->session.GetStats().counted_stalls, 16);
  EXPECT_EQ(match.a->session.GetStats().delay_raises, 0);
  EXPECT_GE(match.b->session.GetStats().delay_raises, 1);
}

TEST(OrcaSessionDelay, RoundTripsSetTheFloor)
{
  Match match(Config{}, 1, 0);
  match.rtt_samples = true;
  match.relay_rtt_ms = {150, 150};
  match.Play(60 * 60);
  ExpectClean(match);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_EQ(p->session.Delay(), 4);
    EXPECT_EQ(p->session.GetStats().delay_raises, 1);
    EXPECT_EQ(p->session.GetStats().delay_lowers, 0);
  }
  EXPECT_GT(match.CompareConfirmed(), 3500);
}

TEST(OrcaSessionDelay, WifiSpikesKeepTheDelayAtTwo)
{
  for (const int spike_ms : {90, 250})
  {
    SCOPED_TRACE(::testing::Message() << "spikes of " << spike_ms << " ms");
    RelayMatch::Side a, b;
    a.relay_ms = 17;
    b.relay_ms = 11;
    a.spike_every_s = 11;
    a.spike_ms = spike_ms;
    b.hitch_every_s = spike_ms == 90 ? 11 : 0;
    a.input_every = b.input_every = 3;
    a.speed = 1.002;
    RelayMatch match(Config{}, a, b);
    match.Play(12000);
    match.Print(spike_ms == 90 ? "Wi-Fi spikes, friend hitching" : "Wi-Fi spikes past the window");
    match.ExpectClean();
    for (int i = 0; i < 2; ++i)
    {
      const Stats& stats = match.SessionOf(i).GetStats();
      EXPECT_EQ(match.SessionOf(i).Delay(), 2) << "side " << i;
      EXPECT_EQ(stats.delay_raises, 0) << "side " << i;
      EXPECT_EQ(stats.delay_reverts, 0) << "side " << i;
      EXPECT_GT(stats.rollbacks, 10) << "side " << i;
      EXPECT_LE(stats.deepest_rollback, Config{}.max_rollback) << "side " << i;
    }
    if (spike_ms == 250)
      EXPECT_GE(match.SessionOf(1).GetStats().counted_stalls, 10);
    EXPECT_GT(match.CompareConfirmed(), 11000);
  }
}

TEST(OrcaSessionDelay, FixedDelayNeverAdapts)
{
  Config two;
  two.fixed_delay = 2;
  Match slow(two, 10, 0);
  slow.rtt_samples = true;
  slow.relay_rtt_ms = {300, 300};
  slow.Play(1800);
  ExpectClean(slow);
  for (const Player* p : {slow.a.get(), slow.b.get()})
  {
    const Stats& stats = p->session.GetStats();
    EXPECT_EQ(p->session.Delay(), 2);
    EXPECT_EQ(p->session.FixedDelay().value_or(-1), 2);
    EXPECT_TRUE(stats.delay_fixed);
    EXPECT_GT(stats.counted_stalls, 15);
    EXPECT_EQ(stats.delay_raises + stats.delay_lowers + stats.delay_reverts, 0);
  }
  EXPECT_GT(slow.CompareConfirmed(), 1000);

  Config five;
  five.fixed_delay = 5;
  Match quiet(five, 1, 0);
  quiet.rtt_samples = true;
  quiet.b->session.SetFixedDelay(1);
  quiet.Play(1800);
  EXPECT_EQ(quiet.a->session.Delay(), 5);
  EXPECT_EQ(quiet.b->session.Delay(), 1);
  quiet.a->session.SetFixedDelay(std::nullopt);
  EXPECT_EQ(quiet.a->session.Delay(), 2);
  EXPECT_FALSE(quiet.a->session.GetStats().delay_fixed);
  quiet.Play(600);
  ExpectClean(quiet);
  EXPECT_EQ(quiet.a->session.Delay(), 2);
  EXPECT_EQ(quiet.b->session.Delay(), 1);
  for (const Player* p : {quiet.a.get(), quiet.b.get()})
  {
    EXPECT_EQ(p->session.GetStats().stalls, 0);
    EXPECT_EQ(p->session.GetStats().delay_raises + p->session.GetStats().delay_lowers, 0);
  }
  EXPECT_GT(quiet.CompareConfirmed(), 2300);

  Match bounds(Config{}, 1, 0);
  bounds.a->session.SetFixedDelay(40);
  EXPECT_EQ(bounds.a->session.Delay(), Config{}.max_delay);
  bounds.a->session.SetFixedDelay(-3);
  EXPECT_EQ(bounds.a->session.Delay(), 1);
}

TEST(OrcaSessionDelay, AnInflatedFirstMedianComesBackDown)
{
  Match match(Config{}, 1, 0);
  match.rtt_samples = true;
  match.relay_rtt_ms = {300, 300};
  match.Play(70);
  for (const Player* p : {match.a.get(), match.b.get()})
    EXPECT_EQ(p->session.Delay(), Config{}.max_delay);
  match.relay_rtt_ms = {16, 16};
  match.Play(600);
  ExpectClean(match);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    const Stats& stats = p->session.GetStats();
    EXPECT_EQ(p->session.Delay(), 2);
    EXPECT_EQ(stats.delay_raises, 1);
    EXPECT_EQ(stats.delay_lowers, 4);
    EXPECT_EQ(stats.delay_reverts, 0);
  }
}

TEST(OrcaSessionDelay, StallsCountOnlyFromPlayersWaitingOnUs)
{
  struct Scripted final : Transport
  {
    void Send(const Packet&) override {}
    std::vector<Packet> Receive() override
    {
      std::vector<Packet> out;
      const int now = session->CurrentFrame();
      for (const int seat : {1, 2})
      {
        Packet packet;
        packet.seat = seat;
        packet.first_frame = next[seat];
        for (; next[seat] <= now + 4; ++next[seat])
          packet.pads.push_back(Pad{});
        packet.current_frame = now;
        packet.ack = {std::max(now - lag[seat], 0), -1, -1, -1};
        packet.stalls = stalls[seat];
        out.push_back(std::move(packet));
      }
      return out;
    }
    bool Connected() const override { return true; }
    Session* session = nullptr;
    std::array<int, 3> next{0, 0, 0};
    std::array<int, 3> lag{0, 2, 2};
    std::array<int, 3> stalls{0, 0, 0};
  };
  FakeGame game;
  Scripted net;
  Config config;
  config.seats = 3;
  config.time_sync = false;
  Session session(config, game, net);
  net.session = &session;
  int completed = -1;
  const auto play = [&](int frames) {
    const int until = session.CurrentFrame() + frames;
    while (session.CurrentFrame() < until)
    {
      const Step step = session.OnBoundary(completed, Pad{});
      ASSERT_NE(step.kind, StepKind::Ended) << session.Error();
      ASSERT_NE(step.kind, StepKind::Stall);
      completed = game.Run();
    }
  };
  play(700);
  for (int i = 0; i < 5; ++i)
  {
    ++net.stalls[1];
    play(60);
  }
  EXPECT_EQ(session.Delay(), 2);
  EXPECT_EQ(session.GetStats().delay_raises, 0);
  net.lag[1] = config.max_rollback + 1;
  for (int i = 0; i < 7; ++i)
  {
    ++net.stalls[1];
    play(60);
  }
  EXPECT_EQ(session.Delay(), 2);
  ++net.stalls[1];
  play(60);
  EXPECT_EQ(session.Delay(), 3);
  EXPECT_EQ(session.GetStats().delay_raises, 1);
}

TEST(OrcaSessionDelay, StallsWeighByWhatARaiseWouldSpare)
{
  struct Scripted final : Transport
  {
    void Send(const Packet&) override {}
    std::vector<Packet> Receive() override
    {
      Packet packet;
      packet.seat = 1;
      packet.first_frame = next;
      const int now = session->CurrentFrame();
      for (; next <= now + 4; ++next)
        packet.pads.push_back(Pad{});
      packet.current_frame = now;
      packet.ack = {std::max(now - lag, 0), -1, -1, -1};
      packet.stalls = stalls;
      packet.spared = spared;
      return {packet};
    }
    bool Connected() const override { return true; }
    Session* session = nullptr;
    int next = 0;
    int lag = 0;
    int stalls = 0;
    std::array<int, SPARED_FRAMES> spared{};
  };
  FakeGame game;
  Scripted net;
  Config config;
  config.time_sync = false;
  Session session(config, game, net);
  net.session = &session;
  int completed = -1;
  const auto play = [&](int frames) {
    const int until = session.CurrentFrame() + frames;
    while (session.CurrentFrame() < until)
    {
      const Step step = session.OnBoundary(completed, Pad{});
      ASSERT_NE(step.kind, StepKind::Ended) << session.Error();
      ASSERT_NE(step.kind, StepKind::Stall);
      completed = game.Run();
    }
  };
  const auto stall = [&](int frames) {
    net.lag = config.max_rollback + 1;
    ++net.stalls;
    for (int k = 0; k < SPARED_FRAMES; ++k)
      net.spared[k] += frames > 0 && frames <= k + 1 ? 1 : 0;
    play(60);
  };
  play(700);
  for (int i = 0; i < 30; ++i)
    stall(0);
  EXPECT_EQ(session.Delay(), 2);
  for (int i = 0; i < 15; ++i)
    stall(2);
  EXPECT_EQ(session.Delay(), 2);
  stall(2);
  EXPECT_EQ(session.Delay(), 3);
  play(60);
  EXPECT_EQ(session.Delay(), 4);
  EXPECT_EQ(session.GetStats().delay_raises, 2);
  EXPECT_TRUE(session.Error().empty()) << session.Error();
}

TEST(OrcaSessionDelay, ChosenDelayChangesMidMatchStayInSync)
{
  Config config;
  Match match(config, 3, 1);
  for (Player* p : {match.a.get(), match.b.get()})
    p->tag_ticks = true;
  match.a->model_input_delay = config.input_delay;
  match.b->session.SetFixedDelay(4);
  match.b->model_input_delay = 4;
  match.Play(200);
  for (const std::optional<int> delay : {std::optional<int>(1), std::optional<int>(6),
                                         std::optional<int>(3), std::optional<int>()})
  {
    match.a->session.SetFixedDelay(delay);
    match.Play(150);
  }
  ExpectClean(match);
  EXPECT_EQ(match.a->session.Delay(), 2);
  EXPECT_EQ(match.b->session.Delay(), 4);
  EXPECT_EQ(match.a->filled, 1);
  EXPECT_EQ(match.a->dropped_after_lower, 3);
  EXPECT_EQ(match.b->filled, 0);
  EXPECT_GT(Match::CheckSamples(*match.a, *match.b), 700);
  EXPECT_GT(Match::CheckSamples(*match.b, *match.a), 700);
  EXPECT_GT(match.CompareConfirmed(), 700);
}

TEST(OrcaSessionDelay, StallBeforeFirstInputRaisesNothing)
{
  Config config;
  config.input_delay = 6;
  config.max_delay = 7;
  Match match(config, 8, 0);
  match.Play(600);
  ExpectClean(match);
  for (const Player* p : {match.a.get(), match.b.get()})
  {
    EXPECT_EQ(p->session.GetStats().stalls, 1);
    EXPECT_EQ(p->session.GetStats().delay_raises, 0);
    EXPECT_EQ(p->session.Delay(), 6);
  }
  EXPECT_GT(match.CompareConfirmed(), 500);
}

namespace
{
RelayMatch::Side T3SideA()
{
  RelayMatch::Side a;
  a.hold_out_ms = 60;
  a.hold_in_ms = 60;
  a.jitter_in_ms = 70;
  a.speed = 1.01;
  a.input_every = 40;
  return a;
}

RelayMatch::Side T3SideB()
{
  RelayMatch::Side b;
  b.input_every = 40;
  return b;
}
}

TEST(OrcaSessionRelay, SlowJitteryLinkStallsRarely)
{
  RelayMatch match(Config{}, T3SideA(), T3SideB());
  match.Play(1800);
  const int early_stalls = match.SessionOf(0).GetStats().stalls;
  match.Play(12000);
  match.Print("T3");
  match.ExpectClean();
  const Stats& a = match.SessionOf(0).GetStats();
  const Stats& b = match.SessionOf(1).GetStats();
  EXPECT_LE(a.stalls, 30);
  EXPECT_LE(a.stalls - early_stalls, 6);
  EXPECT_LE(b.stalls, 5);
  EXPECT_GE(b.delay_raises, 1);
  EXPECT_GE(match.SessionOf(1).Delay(), 3);
  EXPECT_EQ(b.delay_reverts, 0);
  EXPECT_EQ(a.delay_raises, 0);
  EXPECT_EQ(match.SessionOf(0).Delay(), 2);
  EXPECT_GT(match.CompareConfirmed(), 11000);
}

TEST(OrcaSessionRelay, JitterBothWaysEitherSideFast)
{
  for (const bool a_fast : {true, false})
  {
    RelayMatch::Side a = T3SideA();
    RelayMatch::Side b = T3SideB();
    a.jitter_out_ms = 70;
    a.speed = a_fast ? 1.01 : 1.0;
    b.speed = a_fast ? 1.0 : 1.01;
    RelayMatch match(Config{}, a, b);
    match.Play(1200);
    const std::array<int, 2> early{match.SessionOf(0).GetStats().stalls,
                                   match.SessionOf(1).GetStats().stalls};
    match.Play(12000);
    match.Print(a_fast ? "both ways, A fast" : "both ways, B fast");
    match.ExpectClean();
    for (int i = 0; i < 2; ++i)
    {
      const int stalls = match.SessionOf(i).GetStats().stalls;
      EXPECT_LE(early[i], 60) << "a_fast " << a_fast << " side " << i;
      EXPECT_LE(stalls - early[i], 20) << "a_fast " << a_fast << " side " << i;
    }
    EXPECT_GE(match.SessionOf(a_fast ? 1 : 0).Delay(), 4);
    EXPECT_GT(match.CompareConfirmed(), 11000);
  }
}

TEST(OrcaSessionRelay, LanWithHitchesTracksTheLink)
{
  RelayMatch::Side a, b;
  a.relay_ms = b.relay_ms = 14;
  a.speed = 1.01;
  a.input_every = b.input_every = 40;
  a.hitch_every_s = 12;
  b.hitch_every_s = 6;
  a.hitch_burst = b.hitch_burst = 4;
  {
    RelayMatch::Side quiet_a = a, quiet_b = b;
    quiet_a.report_hitches = quiet_b.report_hitches = false;
    RelayMatch unreported(Config{}, quiet_a, quiet_b);
    unreported.Play(12000);
    unreported.Print("LAN, hitches unreported");
    unreported.ExpectClean();
    EXPECT_GT(unreported.SessionOf(0).GetStats().counted_stalls, 15);
    for (int i = 0; i < 2; ++i)
      EXPECT_EQ(unreported.SessionOf(i).GetStats().delay_raises, 0) << "side " << i;
  }
  RelayMatch match(Config{}, a, b);
  match.Play(12000);
  match.Print("LAN, hitches");
  match.ExpectClean();
  const int hitches = match.HitchesOf(0) + match.HitchesOf(1);
  EXPECT_GT(match.HitchesOf(1), 25);
  for (int i = 0; i < 2; ++i)
  {
    const Stats& stats = match.SessionOf(i).GetStats();
    EXPECT_LE(match.SessionOf(i).Delay(), 3) << "side " << i;
    EXPECT_EQ(stats.delay_raises, 0) << "side " << i;
    EXPECT_EQ(stats.counted_stalls, 0) << "side " << i;
    EXPECT_LE(stats.stalls, hitches + 1) << "side " << i;
  }
  EXPECT_GT(match.CompareConfirmed(), 11000);
}

namespace
{
RelayMatch::Side CleanMac()
{
  RelayMatch::Side mac;
  mac.relay_ms = 9;
  mac.jitter_in_ms = mac.jitter_out_ms = 8;
  mac.speed = 1.002;
  mac.input_every = 5;
  return mac;
}

RelayMatch::Side CleanPc()
{
  RelayMatch::Side pc;
  pc.relay_ms = 8;
  pc.jitter_in_ms = pc.jitter_out_ms = 3;
  pc.input_every = 5;
  return pc;
}

RelayMatch::Side SpikyMac()
{
  RelayMatch::Side mac = CleanMac();
  mac.spike_gap_ms = {2000, 5000};
  mac.spike_len_ms = {80, 220};
  return mac;
}

std::string Notes(const RelayMatch& match)
{
  std::string out;
  for (int i = 0; i < 2; ++i)
  {
    for (const std::string& note : match.NotesOf(i))
      out += fmt::format("\n  {}: {}", static_cast<char>('A' + i), note);
  }
  return out;
}
}

TEST(OrcaSessionRelay, DropInOnACleanLinkKeepsTwo)
{
  for (const int join_s : {10, 30, 75})
  {
    SCOPED_TRACE(::testing::Message() << "join at " << join_s << " s");
    for (const bool mac_hosts : {true, false})
    {
      SCOPED_TRACE(::testing::Message() << (mac_hosts ? "Mac hosts" : "PC hosts"));
      RelayMatch::DropIn drop_in;
      drop_in.join_s = join_s;
      drop_in.catch_up_frame_us = mac_hosts ? 3000 : 9000;
      RelayMatch match(Config{}, mac_hosts ? CleanMac() : CleanPc(),
                       mac_hosts ? CleanPc() : CleanMac(), drop_in);
      match.PlaySeconds(join_s + 25);
      ASSERT_GE(match.PluggedFrame(), 0)
          << "the friend never plugged in: host "
          << (match.Running(0) ? match.SessionOf(0).Describe() : "not started") << "; friend "
          << (match.Running(1) ? match.SessionOf(1).Describe() + " " + match.SessionOf(1).Error() :
                                 "not started");
      match.MarkDelays();
      match.PlaySeconds(300);
      match.Print(fmt::format("drop-in at {} s, {}", join_s, mac_hosts ? "Mac hosts" : "PC hosts")
                      .c_str());
      match.ExpectClean();
      EXPECT_GT(match.SessionOf(1).GetStats().stalls, 0);
      for (int i = 0; i < 2; ++i)
      {
        const Stats& stats = match.SessionOf(i).GetStats();
        EXPECT_EQ(stats.counted_stalls, 0) << "side " << i;
        EXPECT_EQ(stats.delay_raises, 0) << "side " << i << Notes(match);
        EXPECT_EQ(match.MaxDelayOf(i), 2) << "side " << i << Notes(match);
        EXPECT_EQ(match.SessionOf(i).Delay(), 2) << "side " << i;
      }
      EXPECT_GT(match.SessionOf(0).GetStats().checksums_matched, 250);
      EXPECT_GT(match.CompareConfirmed(), 300 * 60);
    }
  }
}

TEST(OrcaSessionRelay, SpikesEveryFewSecondsKeepTwo)
{
  for (const bool mac_hosts : {true, false})
  {
    SCOPED_TRACE(::testing::Message() << (mac_hosts ? "Mac hosts" : "PC hosts"));
    RelayMatch::DropIn drop_in;
    drop_in.join_s = 45;
    RelayMatch match(Config{}, mac_hosts ? SpikyMac() : CleanPc(),
                     mac_hosts ? CleanPc() : SpikyMac(), drop_in);
    match.PlaySeconds(60);
    ASSERT_GE(match.PluggedFrame(), 0) << "the friend never plugged in";
    match.MarkDelays();
    match.PlaySeconds(300);
    match.Print(mac_hosts ? "Mac spikes, Mac hosts" : "Mac spikes, PC hosts");
    match.ExpectClean();
    for (int i = 0; i < 2; ++i)
    {
      const Stats& stats = match.SessionOf(i).GetStats();
      EXPECT_GT(stats.counted_stalls, 20) << "side " << i;
      EXPECT_EQ(stats.delay_raises, 0) << "side " << i << Notes(match);
      EXPECT_EQ(match.MaxDelayOf(i), 2) << "side " << i << Notes(match);
    }
    EXPECT_GT(match.CompareConfirmed(), 300 * 60);
  }
}

TEST(OrcaSessionRelay, SteadySmallWaitsRaiseNothing)
{
  RelayMatch::Side a = CleanMac();
  RelayMatch::Side b = CleanPc();
  a.speed = 1.015;
  a.jitter_in_ms = a.jitter_out_ms = 15;
  RelayMatch match(Config{}, a, b);
  match.Play(12000);
  match.Print("fast clock, jitter");
  match.ExpectClean();
  EXPECT_GT(match.SessionOf(0).GetStats().waits, 100);
  for (int i = 0; i < 2; ++i)
  {
    EXPECT_EQ(match.SessionOf(i).GetStats().delay_raises, 0) << "side " << i << Notes(match);
    EXPECT_EQ(match.MaxDelayOf(i), 2) << "side " << i;
  }
  EXPECT_GT(match.CompareConfirmed(), 11000);
}

TEST(OrcaSessionRelay, SpikesAtRandomKeepTwo)
{
  for (const int mean_gap_ms : {3500, 2700})
  {
    for (const bool mac_hosts : {true, false})
    {
      for (u32 seed = 0; seed < 4; ++seed)
      {
        SCOPED_TRACE(::testing::Message() << "a spike every " << mean_gap_ms << " ms on average, "
                                          << (mac_hosts ? "Mac hosts" : "PC hosts") << ", draw "
                                          << seed);
        RelayMatch::Side mac = CleanMac();
        mac.spike_mean_gap_ms = mean_gap_ms;
        mac.spike_len_ms = {80, 220};
        RelayMatch::DropIn drop_in;
        drop_in.join_s = 45;
        RelayMatch match(Config{}, mac_hosts ? mac : CleanPc(), mac_hosts ? CleanPc() : mac,
                         drop_in, seed);
        match.PlaySeconds(60);
        ASSERT_GE(match.PluggedFrame(), 0) << "the friend never plugged in";
        match.MarkDelays();
        match.PlaySeconds(300);
        match.ExpectClean();
        for (int i = 0; i < 2; ++i)
        {
          const Stats& stats = match.SessionOf(i).GetStats();
          EXPECT_GT(stats.counted_stalls, 20) << "side " << i;
          EXPECT_EQ(stats.delay_raises + stats.delay_reverts, 0) << "side " << i << Notes(match);
          EXPECT_EQ(match.MaxDelayOf(i), 2) << "side " << i << Notes(match);
        }
        EXPECT_GT(match.CompareConfirmed(), 300 * 60);
      }
    }
  }
}

TEST(OrcaSessionRelay, SpikesAtRandomEveryTwoSecondsRarelyMove)
{
  for (const bool mac_hosts : {true, false})
  {
    for (u32 seed = 0; seed < 4; ++seed)
    {
      SCOPED_TRACE(::testing::Message() << (mac_hosts ? "Mac hosts" : "PC hosts") << ", draw "
                                        << seed);
      RelayMatch::Side mac = CleanMac();
      mac.spike_mean_gap_ms = 2000;
      mac.spike_len_ms = {80, 220};
      RelayMatch::DropIn drop_in;
      drop_in.join_s = 45;
      RelayMatch match(Config{}, mac_hosts ? mac : CleanPc(), mac_hosts ? CleanPc() : mac,
                       drop_in, seed);
      match.PlaySeconds(60);
      ASSERT_GE(match.PluggedFrame(), 0) << "the friend never plugged in";
      match.MarkDelays();
      match.PlaySeconds(300);
      match.ExpectClean();
      for (int i = 0; i < 2; ++i)
      {
        const Stats& stats = match.SessionOf(i).GetStats();
        EXPECT_LE(stats.delay_raises + stats.delay_reverts, 1) << "side " << i << Notes(match);
        EXPECT_LE(stats.delay_lowers, 1) << "side " << i << Notes(match);
        EXPECT_LE(match.MaxDelayOf(i), 3) << "side " << i << Notes(match);
      }
      EXPECT_GT(match.CompareConfirmed(), 300 * 60);
    }
  }
}

namespace
{
RelayMatch::Side SlowUplinkMac()
{
  RelayMatch::Side mac = CleanMac();
  mac.hold_out_ms = 90;
  mac.jitter_out_ms = 120;
  return mac;
}
}

TEST(OrcaSessionRelay, ClusteredLatenessRaisesTheLateSide)
{
  for (const RelayMatch::Micros rerun_us : {0, 3000})
  {
    for (u32 seed = 0; seed < 4; ++seed)
    {
      SCOPED_TRACE(::testing::Message() << "draw " << seed << ", re-runs " << rerun_us << " us");
      RelayMatch::Side mac = SlowUplinkMac();
      RelayMatch::Side pc = CleanPc();
      mac.rerun_us = pc.rerun_us = rerun_us;
      RelayMatch match(Config{}, mac, pc, std::nullopt, seed);
      match.PlaySeconds(rerun_us ? 120 : 60);
      EXPECT_EQ(match.SessionOf(0).Delay(), 3) << Notes(match);
      match.MarkDelays();
      match.PlaySeconds(rerun_us ? 200 : 260);
      match.Print("slow, bursty uplink");
      match.ExpectClean();
      const Stats& mac_stats = match.SessionOf(0).GetStats();
      const Stats& pc_stats = match.SessionOf(1).GetStats();
      EXPECT_EQ(match.SessionOf(0).Delay(), 3) << Notes(match);
      EXPECT_EQ(mac_stats.delay_raises, 1) << Notes(match);
      EXPECT_EQ(mac_stats.delay_lowers + mac_stats.delay_reverts, 0) << Notes(match);
      EXPECT_EQ(pc_stats.delay_raises + pc_stats.delay_lowers + pc_stats.delay_reverts, 0)
          << Notes(match);
      EXPECT_EQ(match.MaxDelayOf(1), 2) << Notes(match);
      EXPECT_LE(pc_stats.stalls, 30);
      EXPECT_GE(pc_stats.spared_stalls * 4, pc_stats.counted_stalls * 3);
      EXPECT_GT(match.CompareConfirmed(), 300 * 60);
    }
  }
}

TEST(OrcaSessionRelay, SustainedLatenessRaisesThenComesBackDown)
{
  for (u32 seed = 0; seed < 4; ++seed)
  {
    SCOPED_TRACE(::testing::Message() << "draw " << seed);
    RelayMatch::Side mac = SlowUplinkMac();
    mac.hold_from_s = 60;
    mac.hold_to_s = 120;
    RelayMatch match(Config{}, mac, CleanPc(), std::nullopt, seed);
    match.PlaySeconds(60);
    match.MarkDelays();
    match.PlaySeconds(60);
    match.Print("a slow, bursty minute");
    EXPECT_EQ(match.MaxDelayOf(0), 3) << Notes(match);
    EXPECT_EQ(match.MaxDelayOf(1), 2) << Notes(match);
    match.PlaySeconds(12);
    match.ExpectClean();
    for (int i = 0; i < 2; ++i)
    {
      EXPECT_EQ(match.SessionOf(i).Delay(), 2) << "side " << i << Notes(match);
      EXPECT_EQ(match.SessionOf(i).GetStats().delay_reverts, 0) << "side " << i << Notes(match);
    }
    match.MarkDelays();
    match.PlaySeconds(60);
    for (int i = 0; i < 2; ++i)
      EXPECT_EQ(match.MaxDelayOf(i), 2) << "side " << i << Notes(match);
    EXPECT_LE(match.SessionOf(1).GetStats().stalls, 30);
    EXPECT_GT(match.CompareConfirmed(), 180 * 60);
  }
}

TEST(OrcaSessionRelay, DropoutsAFrameWouldSpareRaiseAndStay)
{
  for (u32 seed = 0; seed < 4; ++seed)
  {
    SCOPED_TRACE(::testing::Message() << "draw " << seed);
    RelayMatch::Side mac = CleanMac();
    mac.spike_gap_ms = {2100, 2100};
    mac.spike_len_ms = {130, 150};
    RelayMatch match(Config{}, mac, CleanPc(), std::nullopt, seed);
    match.PlaySeconds(300);
    match.Print("dropouts every 2.1 s");
    match.ExpectClean();
    for (int i = 0; i < 2; ++i)
    {
      const Stats& stats = match.SessionOf(i).GetStats();
      EXPECT_GE(match.SessionOf(i).Delay(), 3) << "side " << i << Notes(match);
      EXPECT_EQ(stats.delay_lowers + stats.delay_reverts, 0) << "side " << i << Notes(match);
      EXPECT_LE(stats.stalls, 40) << "side " << i;
    }
    EXPECT_GT(match.CompareConfirmed(), 290 * 60);
  }
}

TEST(OrcaSessionRelay, LongDropoutsEverySecondRaiseNothing)
{
  for (u32 seed = 0; seed < 4; ++seed)
  {
    SCOPED_TRACE(::testing::Message() << "draw " << seed);
    RelayMatch::Side mac = CleanMac();
    mac.spike_gap_ms = {600, 1200};
    mac.spike_len_ms = {150, 300};
    mac.spike_from_s = 60;
    mac.spike_to_s = 120;
    RelayMatch match(Config{}, mac, CleanPc(), std::nullopt, seed);
    match.PlaySeconds(60);
    match.MarkDelays();
    match.PlaySeconds(75);
    match.Print("a minute of long dropouts");
    match.ExpectClean();
    for (int i = 0; i < 2; ++i)
    {
      EXPECT_GT(match.SessionOf(i).GetStats().counted_stalls, 30) << "side " << i;
      EXPECT_EQ(match.MaxDelayOf(i), 2) << "side " << i << Notes(match);
    }
    EXPECT_GT(match.CompareConfirmed(), 120 * 60);
  }
}

TEST(OrcaSessionRelay, StallsSetOffByTheOtherSidesStallCountForNothing)
{
  RelayMatch::Side mac = CleanMac();
  mac.spike_gap_ms = {800, 1200};
  mac.spike_len_ms = {250, 300};
  mac.spike_in_only = true;
  mac.spike_from_s = 20;
  mac.spike_to_s = 80;
  RelayMatch match(Config{}, mac, CleanPc());
  match.PlaySeconds(90);
  match.Print("Mac download spikes");
  match.ExpectClean();
  const Stats& mac_stats = match.SessionOf(0).GetStats();
  const Stats& pc_stats = match.SessionOf(1).GetStats();
  EXPECT_GT(mac_stats.counted_stalls, 20);
  EXPECT_GT(pc_stats.stalls, 5);
  EXPECT_EQ(pc_stats.counted_stalls, 0);
  EXPECT_GE(match.MaxDelayOf(1), 3) << Notes(match);
  EXPECT_EQ(match.MaxDelayOf(0), 2) << Notes(match);
  EXPECT_EQ(mac_stats.delay_raises, 0) << Notes(match);
  for (const std::string& note : match.NotesOf(0))
    EXPECT_EQ(note.find("stalled"), std::string::npos) << note;
  EXPECT_GT(match.CompareConfirmed(), 80 * 60);
}

namespace
{
void FuzzDelaySwings(int snapshot_every)
{
  for (u32 seed = 1; seed <= 60; ++seed)
  {
    std::mt19937 rng(seed);
    const auto pick = [&rng](int lo, int hi) {
      return Uniform(rng, lo, hi);
    };
    Config config;
    config.snapshot_every = snapshot_every;
    config.input_delay = pick(0, 3);
    config.max_delay = config.input_delay + pick(0, 5);
    Match match(config, pick(0, 8), pick(0, 6));
    match.net.extra = {pick(0, 4), pick(0, 4)};
    match.a_extra_every = pick(0, 1) ? pick(20, 200) : 0;
    match.a->tag_ticks = true;
    match.b->tag_ticks = true;
    for (int chunk = 0; chunk < 60; ++chunk)
    {
      for (Player* p : {match.a.get(), match.b.get()})
      {
        if (pick(0, 3) == 0)
          p->session.OnRoundTrip(pick(5, 450));
        if (snapshot_every == 0 && seed % 2 == 0 && pick(0, 3) == 0)
          p->game.save_ms = pick(0, 14);
      }
      match.Play(pick(5, 60));
    }
    match.Play(400);

    SCOPED_TRACE(::testing::Message() << "seed " << seed << ", snapshot every " << snapshot_every);
    ExpectClean(match);
    for (const Player* p : {match.a.get(), match.b.get()})
    {
      EXPECT_LE(p->session.GetStats().deepest_rollback,
                config.max_rollback +
                    (snapshot_every > 0 ? snapshot_every : MAX_SNAPSHOT_EVERY) - 1);
    }
    const int confirmed = std::min(Settled(*match.a), Settled(*match.b));
    ASSERT_GT(confirmed, 500);
    int checked = 0;
    for (int f = 0; f <= confirmed; ++f)
    {
      const auto ra = match.a->game.m_ran_pads.find(f);
      const auto rb = match.b->game.m_ran_pads.find(f);
      if (ra == match.a->game.m_ran_pads.end() || rb == match.b->game.m_ran_pads.end())
        continue;
      ASSERT_EQ(ra->second, rb->second) << "frame " << f;
      ++checked;
    }
    EXPECT_GT(checked, 300);
    EXPECT_GT(match.CompareConfirmed(), 300);
  }
}
}

TEST(OrcaSessionFuzz, BothSidesRunIdenticalInputsUnderDelaySwings)
{
  FuzzDelaySwings(0);
}

TEST(OrcaSessionFuzz, SparseSnapshotsKeepBothSidesIdentical)
{
  for (int k = 2; k <= MAX_SNAPSHOT_EVERY; ++k)
    FuzzDelaySwings(k);
}

TEST(OrcaSessionSparse, SavesDropByKUnderConstantPrediction)
{
  for (const bool still : {true, false})
  {
    std::array<double, MAX_SNAPSHOT_EVERY + 1> per_frame{};
    std::array<u64, MAX_SNAPSHOT_EVERY + 1> final_state{};
    for (int k = 1; k <= MAX_SNAPSHOT_EVERY; ++k)
    {
      SCOPED_TRACE(::testing::Message() << "still " << still << ", snapshot every " << k);
      Config config;
      config.snapshot_every = k;
      config.max_delay = config.input_delay;
      Match match(config, 5, 0);
      match.a->still = still;
      match.b->still = still;
      match.Play(3000);
      ExpectClean(match);
      EXPECT_GT(match.CompareConfirmed(), 2900);
      const Stats& stats = match.a->session.GetStats();
      EXPECT_GT(stats.checksums_matched, 40);
      EXPECT_EQ(stats.snapshot_every, k);
      EXPECT_LE(stats.deepest_rollback, config.max_rollback + k - 1);
      EXPECT_EQ(stats.rollbacks > 0, !still);
      per_frame[k] = static_cast<double>(stats.saves) / match.a->session.CurrentFrame();
      const auto late = match.a->game.m_history.find(2800);
      ASSERT_NE(late, match.a->game.m_history.end());
      final_state[k] = late->second;
      EXPECT_EQ(final_state[k], final_state[1]);
      std::printf("[ sparse %s k=%d ] %.3f saves per frame, %d rollbacks, %d re-run frames, "
                  "deepest %d\n",
                  still ? "still" : "moving", k, per_frame[k], stats.rollbacks,
                  stats.resimulated_frames, stats.deepest_rollback);
    }
    for (int k = 2; k <= MAX_SNAPSHOT_EVERY; ++k)
    {
      const double ratio = per_frame[1] / per_frame[k];
      EXPECT_GT(ratio, still ? 0.9 * k : 0.7 * k) << "k " << k;
    }
  }
}

TEST(OrcaSessionSparse, RollbackBetweenSnapshotsReRunsFromTheOlderOne)
{
  constexpr int CHANGE_EVERY = 17;
  const auto remote = [](int f) { return Pad{static_cast<u8>((f / CHANGE_EVERY) % 4)}; };
  struct Lagging final : Transport
  {
    Lagging(std::function<Pad(int)> remote_pad, int lag_frames)
        : pad(std::move(remote_pad)), lag(lag_frames)
    {
    }
    void Send(const Packet&) override {}
    std::vector<Packet> Receive() override
    {
      Packet packet;
      packet.seat = 1;
      packet.first_frame = next;
      for (; next <= session->CurrentFrame() - lag; ++next)
        packet.pads.push_back(pad(next));
      packet.current_frame = std::max(0, session->CurrentFrame() - lag);
      return {packet};
    }
    bool Connected() const override { return true; }
    std::function<Pad(int)> pad;
    int lag;
    Session* session = nullptr;
    int next = 0;
  };
  struct Recording final : Game
  {
    bool Save(int frame) override { return game.Save(frame); }
    bool Load(int frame) override
    {
      std::set<int> held;
      for (const auto& [f, state] : game.m_snapshots)
        held.insert(f);
      held_at_load.push_back(std::move(held));
      return game.Load(frame);
    }
    void SetPads(int frame, const Pads& pads) override { game.SetPads(frame, pads); }
    void SetResimulating(bool resimulating) override { game.SetResimulating(resimulating); }
    std::optional<u64> SnapshotChecksum(int frame) override { return game.SnapshotChecksum(frame); }
    void ResetPacing() override { game.ResetPacing(); }
    FakeGame game;
    std::vector<std::set<int>> held_at_load;
  };

  for (const int LAG : {4, Config{}.max_rollback})
  for (int k = 2; k <= MAX_SNAPSHOT_EVERY; ++k)
  {
    SCOPED_TRACE(::testing::Message() << "lag " << LAG << ", snapshot every " << k);
    Recording game;
    Lagging net(remote, LAG);
    Config config;
    config.time_sync = false;
    config.max_delay = config.input_delay;
    config.snapshot_every = k;
    game.game.ring_slots = static_cast<size_t>(config.max_rollback) + 2;
    Session session(config, game, net);
    net.session = &session;
    int completed = -1;
    for (int i = 0; i < 1200; ++i)
    {
      const Step step = session.OnBoundary(completed, Pad{});
      ASSERT_NE(step.kind, StepKind::Ended) << session.Error();
      if (step.kind == StepKind::Run || step.kind == StepKind::Rollback)
        completed = game.game.Run();
    }
    const Stats& stats = session.GetStats();
    const std::vector<std::pair<int, u64>>& loads = game.game.m_loads;
    ASSERT_EQ(loads.size(), static_cast<size_t>(stats.rollbacks));
    ASSERT_GT(loads.size(), 40u);

    FakeGame straight;
    for (int f = 0; f <= stats.confirmed_frame; ++f)
    {
      straight.SetPads(f, Pads{Pad{}, remote(f)});
      straight.Run();
    }
    const auto state_at = [](const FakeGame& g, int f) {
      const auto it = g.m_history.find(f);
      return it == g.m_history.end() ? std::optional<u64>() : it->second;
    };
    int before_wrong = 0;
    for (size_t i = 0; i < loads.size(); ++i)
    {
      const int wrong = static_cast<int>(i + 1) * CHANGE_EVERY;
      const int loaded = loads[i].first;
      const std::set<int>& held = game.held_at_load[i];
      EXPECT_LE(loaded, wrong);
      EXPECT_LE(wrong - loaded, k - 1) << "wrong frame " << wrong;
      const auto newer = held.upper_bound(wrong);
      EXPECT_EQ(newer == held.begin() ? -1 : *std::prev(newer), loaded) << "wrong frame " << wrong;
      EXPECT_EQ(loads[i].second, state_at(straight, loaded)) << "load of frame " << loaded;
      before_wrong += loaded < wrong;
    }
    EXPECT_GT(before_wrong, static_cast<int>(loads.size()) / 3);
    EXPECT_EQ(stats.deepest_rollback, LAG + k - 1);
    EXPECT_EQ(stats.stalls, 0);
    std::printf("[ between lag %d k=%d ] %zu rollbacks, %d loaded a snapshot before the wrong "
                "frame\n",
                LAG, k, loads.size(), before_wrong);
    EXPECT_EQ(stats.resimulated_frames - stats.rollbacks * LAG, [&] {
      int extra = 0;
      for (size_t i = 0; i < loads.size(); ++i)
        extra += static_cast<int>(i + 1) * CHANGE_EVERY - loads[i].first;
      return extra;
    }());
    int compared = 0;
    for (int f = 0; f <= stats.confirmed_frame; ++f, ++compared)
      EXPECT_EQ(state_at(game.game, f), state_at(straight, f)) << "frame " << f;
    EXPECT_GT(compared, 700);
  }
}

TEST(OrcaSessionSparse, SpacingFollowsTheMeasuredSaveTime)
{
  for (const auto& [save_ms, k] : std::vector<std::pair<double, int>>{
           {0.0, 1}, {1.7, 1}, {2.9, 1}, {3.5, 2}, {6.0, 2}, {6.4, 3}, {9.4, 4}, {40.0, 4}})
  {
    SCOPED_TRACE(::testing::Message() << save_ms << " ms");
    Config config;
    Match match(config, 5, 0);
    match.a->game.save_ms = save_ms;
    match.Play(130);
    ExpectClean(match);
    EXPECT_EQ(match.a->session.GetStats().snapshot_every, k);
    EXPECT_EQ(match.b->session.GetStats().snapshot_every, 1);
    config.snapshot_every = 2;
    Match fixed(config, 5, 0);
    fixed.a->game.save_ms = save_ms;
    fixed.Play(130);
    EXPECT_EQ(fixed.a->session.GetStats().snapshot_every, 2);
  }
  Match match(Config{}, 5, 0);
  for (const auto& [save_ms, k] : std::vector<std::pair<double, int>>{
           {6.4, 3}, {6.1, 3}, {5.9, 3}, {5.7, 2}, {6.2, 2}, {6.3, 3}, {0.5, 1}})
  {
    SCOPED_TRACE(::testing::Message() << save_ms << " ms");
    match.a->game.save_ms = save_ms;
    match.Play(60);
    EXPECT_EQ(match.a->session.GetStats().snapshot_every, k);
  }
  ExpectClean(match);
}

namespace
{
template <typename T>
T At(const std::map<int, T>& map, int frame)
{
  const auto it = map.find(frame);
  if (it == map.end())
  {
    ADD_FAILURE() << "no frame " << frame;
    return T{};
  }
  return it->second;
}

struct DropInPlayer
{
  DropInPlayer(const Config& config, FakeNet& net, int index, int start)
      : transport(net, index), session(config, game, transport), completed(start - 1)
  {
    game.ring_slots = static_cast<size_t>(config.max_rollback) + 2;
  }
  Pad LocalPad(int tick) const
  {
    Pad pad{};
    pad[0] = static_cast<u8>((tick / 5 + seat * 3) % 4);
    pad[2] = static_cast<u8>(tick & 0xff);
    pad[4] = static_cast<u8>(1 + seat);
    return pad;
  }
  int Tick(int tick, int frames)
  {
    int ran = 0;
    for (int guard = 0; guard < 4000 && ran < frames; ++guard)
    {
      const Step step = session.OnBoundary(completed, LocalPad(tick));
      if (step.kind == StepKind::Ended || step.kind == StepKind::Stall ||
          step.kind == StepKind::Wait)
      {
        return ran;
      }
      const bool new_frame = step.kind == StepKind::Run && !session.Resimulating();
      completed = game.Run();
      if (new_frame)
        ++ran;
    }
    return ran;
  }
  FakeGame game;
  FakeNet::End transport;
  Session session;
  int completed;
  int seat = 0;
};

Config DropInConfig(int local_seat, int start_frame)
{
  Config config;
  config.seats = MAX_SEATS;
  config.local_seat = local_seat;
  config.authority_seat = 0;
  config.start_frame = start_frame;
  std::array<SeatPlan, MAX_SEATS> plan{};
  plan[0] = {0, NEVER, 0};
  config.plan = plan;
  return config;
}

std::vector<Pads> PlaySolo(FakeGame* game, int from, int frames, std::vector<Pads> log = {})
{
  for (int f = from; f < from + frames; ++f)
  {
    Pads pads;
    pads.fill(UNPLUGGED_PAD);
    pads[0] = Pad{static_cast<u8>((f / 9) % 4), 0, static_cast<u8>(f & 0xff), 0, 9, 0, 0, 0};
    game->SetPads(f, pads);
    game->Run();
    log.push_back(pads);
  }
  return log;
}
}

TEST(OrcaPadCodec, UnpluggedIsNotAButton)
{
  EXPECT_TRUE(IsUnplugged(UNPLUGGED_PAD));
  EXPECT_FALSE(IsUnplugged(Pad{}));
  GCPadStatus held;
  held.button = 0xFFFF;
  EXPECT_FALSE(IsUnplugged(EncodePad(held)));
  EXPECT_FALSE(DecodePad(UNPLUGGED_PAD).isConnected);
  EXPECT_TRUE(DecodePad(Pad{}).isConnected);
}

TEST(OrcaDropIn, PacketFieldsRoundTrip)
{
  Packet sent;
  sent.seat = 0;
  sent.current_frame = 900;
  std::array<SeatPlan, MAX_SEATS> roster{};
  roster[0] = {0, NEVER, 0};
  roster[1] = {950, 1200, 900};
  sent.roster = roster;
  sent.history_ack = 899;
  sent.history_seat = 1;
  sent.history_first = 600;
  Pads a;
  a.fill(UNPLUGGED_PAD);
  a[0] = Pad{1, 2, 3, 4, 5, 6, 7, 8};
  Pads b = a;
  b[0][0] = 9;
  sent.history = {a, a, a, b, a};
  sent.leaving = true;
  Packet got;
  ASSERT_TRUE(DecodePacket(EncodePacket(sent), &got));
  ASSERT_TRUE(got.roster);
  EXPECT_EQ(*got.roster, roster);
  EXPECT_EQ(got.history_ack, 899);
  EXPECT_EQ(got.history_seat, 1);
  EXPECT_EQ(got.history_first, 600);
  EXPECT_EQ(got.history, sent.history);
  EXPECT_TRUE(got.leaving);
  EXPECT_NE(EncodePacket(sent).find("[3,"), std::string::npos);

  std::optional<Packet> pending;
  Packet first = sent;
  first.history = {a, a};
  Packet second = sent;
  second.history_first = 602;
  second.history = {b};
  CoalescePacket(&pending, first);
  CoalescePacket(&pending, second);
  EXPECT_EQ(pending->history_first, 600);
  EXPECT_EQ(pending->history.size(), 3u);
}

TEST(OrcaDropIn, FriendJoinsPlaysLeavesAndAnotherJoins)
{
  for (const auto& [latency, jitter] : std::vector<std::pair<int, int>>{{1, 0}, {4, 2}, {8, 4}})
  {
    SCOPED_TRACE(::testing::Message() << "latency " << latency << " jitter " << jitter);
    FakeNet net;
    net.latency = latency;
    net.jitter = jitter;
    FakeGame solo;
    std::vector<Pads> log = PlaySolo(&solo, 0, 300);
    const int keyframe = 240;

    auto host = std::make_unique<DropInPlayer>(DropInConfig(0, 300), net, 0, 300);
    host->game.m_state = solo.m_state;
    host->game.m_history = solo.m_history;
    host->game.m_ran_pads = solo.m_ran_pads;
    host->session.SetLog(0, log);
    host->session.AddPeer(1, 300, keyframe);
    EXPECT_EQ(host->session.Plan()[0], (SeatPlan{0, NEVER, 0})) << host->session.Describe();
    auto joiner = std::make_unique<DropInPlayer>(DropInConfig(1, keyframe), net, 1, keyframe);
    joiner->seat = 1;
    joiner->game.m_state = At(solo.m_history, keyframe);

    int t = 0;
    int plugged_at = -1;
    for (; t < 1200; ++t)
    {
      net.now = t;
      if (t % 30 == 0)
      {
        for (DropInPlayer* p : {host.get(), joiner.get()})
          p->session.OnRoundTrip((latency + jitter / 2) * 1000 / 60);
      }
      host->Tick(t, 1);
      const bool behind = joiner->session.CatchingUp() ||
                          joiner->session.AuthorityFrame() - joiner->session.CurrentFrame() > 12;
      joiner->Tick(t, behind ? 40 : 1);
      ASSERT_TRUE(host->session.Error().empty()) << host->session.Error();
      ASSERT_TRUE(joiner->session.Error().empty()) << joiner->session.Error();
      if (plugged_at < 0 && !joiner->session.CatchingUp())
        plugged_at = host->session.Plan()[1].plug_from;
    }
    ASSERT_GE(plugged_at, 300);
    for (int f = 300; f < host->session.CurrentFrame(); ++f)
      ASSERT_FALSE(IsUnplugged(At(host->game.m_ran_pads, f)[0])) << "frame " << f;
    for (const FakeGame* game : {&host->game, &joiner->game})
    {
      EXPECT_TRUE(IsUnplugged(At(game->m_ran_pads, plugged_at - 1)[1]));
      EXPECT_FALSE(IsUnplugged(At(game->m_ran_pads, plugged_at)[1]));
      EXPECT_TRUE(IsUnplugged(At(game->m_ran_pads, plugged_at)[2]));
    }
    const int confirmed =
        std::min({host->session.GetStats().confirmed_frame, host->session.CurrentFrame() - 1,
                  joiner->session.GetStats().confirmed_frame, joiner->session.CurrentFrame() - 1});
    ASSERT_GT(confirmed, plugged_at + 300);
    for (int f = keyframe; f <= confirmed; ++f)
    {
      ASSERT_EQ(At(host->game.m_history, f), At(joiner->game.m_history, f)) << "frame " << f;
      ASSERT_EQ(At(host->game.m_ran_pads, f), At(joiner->game.m_ran_pads, f)) << "frame " << f;
    }
    EXPECT_GT(host->session.GetStats().checksums_matched, 10);
    EXPECT_GT(joiner->session.GetStats().checksums_matched, 10);
    if (latency == 1)
    {
      EXPECT_GT(joiner->session.GetStats().stalls, 0);
      EXPECT_EQ(joiner->session.GetStats().counted_stalls, 0);
      for (const DropInPlayer* p : {host.get(), joiner.get()})
        EXPECT_EQ(p->session.GetStats().delay_raises, 0);
    }

    joiner->session.RequestLeave();
    for (int end = t + 200; t < end && !(joiner->session.LeftDone() && host->session.Idle()); ++t)
    {
      net.now = t;
      host->Tick(t, 1);
      if (!joiner->session.LeftDone())
        joiner->Tick(t, 1);
    }
    ASSERT_TRUE(joiner->session.LeftDone());
    ASSERT_TRUE(host->session.Idle());
    const int unplug = host->session.Plan()[1].unplug_from;
    ASSERT_NE(unplug, NEVER);
    EXPECT_EQ(joiner->session.Plan()[1].unplug_from, unplug);
    EXPECT_FALSE(IsUnplugged(At(host->game.m_ran_pads, unplug - 1)[1]));
    EXPECT_TRUE(IsUnplugged(At(host->game.m_ran_pads, unplug)[1]));
    for (int f = keyframe; f < unplug; ++f)
      ASSERT_EQ(At(host->game.m_history, f), At(joiner->game.m_history, f)) << "frame " << f;

    int base = -1;
    std::vector<Pads> host_log = host->session.TakeLog(&base);
    const int next = host->session.CurrentFrame();
    ASSERT_EQ(base, 0);
    ASSERT_EQ(static_cast<int>(host_log.size()), next);
    for (int f = 0; f < next; ++f)
      ASSERT_EQ(host_log[f], At(host->game.m_ran_pads, f)) << "frame " << f;
    const std::vector<Pad> pending = host->session.PendingLocal();
    host_log = PlaySolo(&host->game, next, 400, host_log);
    (void)pending;

    const int keyframe2 = next + 300;
    const int start2 = next + 400;
    FakeNet net2;
    net2.latency = latency;
    net2.jitter = jitter;
    auto host2 = std::make_unique<DropInPlayer>(DropInConfig(0, start2), net2, 0, start2);
    host2->game.m_state = host->game.m_state;
    host2->game.m_history = host->game.m_history;
    host2->game.m_ran_pads = host->game.m_ran_pads;
    host2->session.SetLog(0, host_log);
    host2->session.AddPeer(1, start2, keyframe2);
    auto joiner2 = std::make_unique<DropInPlayer>(DropInConfig(1, keyframe2), net2, 1, keyframe2);
    joiner2->seat = 1;
    joiner2->game.m_state = At(host->game.m_history, keyframe2);
    for (int u = 0; u < 900; ++u)
    {
      net2.now = u;
      if (u % 30 == 0)
      {
        for (DropInPlayer* p : {host2.get(), joiner2.get()})
          p->session.OnRoundTrip((latency + jitter / 2) * 1000 / 60);
      }
      host2->Tick(u, 1);
      const bool behind = joiner2->session.CatchingUp() ||
                          joiner2->session.AuthorityFrame() - joiner2->session.CurrentFrame() > 12;
      joiner2->Tick(u, behind ? 40 : 1);
      ASSERT_TRUE(host2->session.Error().empty()) << host2->session.Error();
      ASSERT_TRUE(joiner2->session.Error().empty()) << joiner2->session.Error();
    }
    EXPECT_FALSE(joiner2->session.CatchingUp());
    EXPECT_GT(host2->session.GetStats().checksums_matched, 5);
    const int confirmed2 =
        std::min({host2->session.GetStats().confirmed_frame, host2->session.CurrentFrame() - 1,
                  joiner2->session.GetStats().confirmed_frame, joiner2->session.CurrentFrame() - 1});
    EXPECT_GT(confirmed2, start2 + 300);
    for (int f = keyframe2; f <= confirmed2; ++f)
      ASSERT_EQ(At(host2->game.m_history, f), At(joiner2->game.m_history, f)) << "frame " << f;
  }
}

TEST(OrcaDropIn, DroppedFriendUnplugsAfterItsLastInput)
{
  FakeNet net;
  net.latency = 4;
  FakeGame solo;
  std::vector<Pads> log = PlaySolo(&solo, 0, 120);
  auto host = std::make_unique<DropInPlayer>(DropInConfig(0, 120), net, 0, 120);
  host->game.m_state = solo.m_state;
  host->session.SetLog(0, log);
  host->session.AddPeer(1, 120, 60);
  auto joiner = std::make_unique<DropInPlayer>(DropInConfig(1, 60), net, 1, 60);
  joiner->seat = 1;
  joiner->game.m_state = At(solo.m_history, 60);
  int t = 0;
  for (; t < 400; ++t)
  {
    net.now = t;
    host->Tick(t, 1);
    joiner->Tick(t, joiner->session.CatchingUp() ? 40 : 1);
  }
  ASSERT_FALSE(joiner->session.CatchingUp());
  host->session.DropSeat(1);
  for (int end = t + 60; t < end; ++t)
  {
    net.now = t;
    host->Tick(t, 1);
  }
  ASSERT_TRUE(host->session.Error().empty()) << host->session.Error();
  EXPECT_TRUE(host->session.Idle());
  const int unplug = host->session.Plan()[1].unplug_from;
  EXPECT_TRUE(IsUnplugged(At(host->game.m_ran_pads, unplug)[1]));
  EXPECT_TRUE(IsUnplugged(At(host->game.m_ran_pads, host->session.CurrentFrame() - 1)[1]));
}

TEST(OrcaDropIn, FriendBackInTheSameSeatAfterADrop)
{
  for (const bool heard_late : {false, true})
  {
    SCOPED_TRACE(heard_late ? "the drop heard as the friend comes back" : "the drop heard at once");
    FakeNet net;
    net.latency = 4;
    FakeGame solo;
    std::vector<Pads> log = PlaySolo(&solo, 0, 120);
    auto host = std::make_unique<DropInPlayer>(DropInConfig(0, 120), net, 0, 120);
    host->game.m_state = solo.m_state;
    host->game.m_history = solo.m_history;
    host->game.m_ran_pads = solo.m_ran_pads;
    host->session.SetLog(0, log);
    host->session.AddPeer(1, 120, 60);
    auto joiner = std::make_unique<DropInPlayer>(DropInConfig(1, 60), net, 1, 60);
    joiner->seat = 1;
    joiner->game.m_state = At(solo.m_history, 60);
    int t = 0;
    for (; t < 400; ++t)
    {
      net.now = t;
      host->Tick(t, 1);
      joiner->Tick(t, joiner->session.CatchingUp() ? 40 : 1);
    }
    ASSERT_FALSE(joiner->session.CatchingUp());
    const int first_plug = host->session.Plan()[1].plug_from;
    ASSERT_NE(first_plug, NEVER);

    joiner.reset();
    const auto quiet = [&](int ticks) {
      for (int end = t + ticks; t < end; ++t)
      {
        net.now = t;
        host->Tick(t, 1);
        net.queues[1].clear();
      }
    };
    const auto heard = [&](int ticks) {
      for (int end = t + ticks; t < end; ++t)
      {
        net.now = t;
        host->Tick(t, 1);
      }
    };
    if (heard_late)
    {
      quiet(60);
      heard(6);
      ASSERT_NE(host->session.Plan()[1].live_from, NEVER);
      host->session.DropSeat(1);
      heard(30);
    }
    else
    {
      host->session.DropSeat(1);
      quiet(60);
      heard(30);
    }
    ASSERT_TRUE(host->session.Error().empty()) << host->session.Error();
    ASSERT_TRUE(host->session.Idle());
    ASSERT_EQ(host->session.Plan()[1].plug_from, first_plug);
    ASSERT_FALSE(net.queues[1].empty());
    const int keyframe = host->session.CurrentFrame() - 1;
    const int next = host->session.CurrentFrame();
    host->session.AddPeer(1, next, keyframe);
    heard(20);
    auto back = std::make_unique<DropInPlayer>(DropInConfig(1, keyframe), net, 1, keyframe);
    back->seat = 1;
    back->game.m_state = At(host->game.m_history, keyframe);
    for (int end = t + 600; t < end; ++t)
    {
      net.now = t;
      host->Tick(t, 1);
      back->Tick(t, back->session.CatchingUp() ? 40 : 1);
      ASSERT_TRUE(host->session.Error().empty()) << host->session.Error();
      ASSERT_TRUE(back->session.Error().empty()) << back->session.Error();
    }
    ASSERT_FALSE(back->session.CatchingUp()) << back->session.Describe();
    const int plugged_again = host->session.Plan()[1].plug_from;
    EXPECT_GE(plugged_again, next);
    EXPECT_EQ(back->session.Plan()[1].plug_from, plugged_again);
    const int confirmed =
        std::min({host->session.GetStats().confirmed_frame, host->session.CurrentFrame() - 1,
                  back->session.GetStats().confirmed_frame, back->session.CurrentFrame() - 1});
    ASSERT_GT(confirmed, plugged_again + 120);
    for (int f = keyframe; f <= confirmed; ++f)
    {
      ASSERT_EQ(At(host->game.m_history, f), At(back->game.m_history, f)) << "frame " << f;
      ASSERT_EQ(At(host->game.m_ran_pads, f), At(back->game.m_ran_pads, f)) << "frame " << f;
    }
    EXPECT_FALSE(IsUnplugged(At(host->game.m_ran_pads, plugged_again)[1]));
    EXPECT_TRUE(IsUnplugged(At(host->game.m_ran_pads, plugged_again - 1)[1]));
    EXPECT_GT(back->session.GetStats().checksums_matched, 2);
  }
}

TEST(OrcaDropIn, PlugInWaitsForEveryPlayerToHoldThePortValues)
{
  struct Inject final : Transport
  {
    void Send(const Packet& packet) override { sent.push_back(packet); }
    std::vector<Packet> Receive() override { return std::exchange(queue, {}); }
    bool Connected() const override { return true; }
    std::vector<Packet> queue;
    std::vector<Packet> sent;
  };
  FakeGame solo;
  std::vector<Pads> log = PlaySolo(&solo, 0, 300);
  Inject net;
  FakeGame game;
  game.m_state = solo.m_state;
  game.m_history = solo.m_history;
  game.m_ran_pads = solo.m_ran_pads;
  Session host(DropInConfig(0, 300), game, net);
  host.SetLog(0, log);
  host.AddPeer(1, 300, 240);
  host.AddPeer(2, 300, 240);
  host.RequireValues(1, 4);
  host.RequireValues(2, 5);
  int completed = 299;
  std::array<int, MAX_SEATS> sequence{};
  const auto boundary = [&](int ack1, int ack2) {
    for (const auto& [seat, ack] : {std::pair(1, ack1), std::pair(2, ack2)})
    {
      Packet p;
      p.seat = seat;
      p.sequence = ++sequence[seat];
      p.current_frame = host.CurrentFrame();
      p.history_ack = host.CurrentFrame() - 1;
      p.values_ack = ack;
      net.queue.push_back(p);
    }
    const Step step = host.OnBoundary(completed, Pad{});
    EXPECT_EQ(step.kind, StepKind::Run) << host.Error();
    completed = game.Run();
  };
  for (int i = 0; i < 3; ++i)
    boundary(4, 4);
  EXPECT_NE(host.Plan()[1].plug_from, NEVER) << host.Describe();
  EXPECT_EQ(host.Plan()[2].plug_from, NEVER) << host.Describe();
  for (int i = 0; i < 3; ++i)
    boundary(4, 5);
  EXPECT_EQ(host.Plan()[2].plug_from, NEVER) << host.Describe();
  boundary(5, 4);
  const int plugged = host.Plan()[2].plug_from;
  EXPECT_NE(plugged, NEVER) << host.Describe();
  EXPECT_GT(plugged, host.CurrentFrame());
  ASSERT_FALSE(net.sent.empty());
  ASSERT_TRUE(net.sent.back().roster);
  EXPECT_EQ((*net.sent.back().roster)[2].plug_from, plugged);
  EXPECT_TRUE(host.Error().empty()) << host.Error();
}

TEST(OrcaDropIn, PlayersAcknowledgeTheValuesTheyHold)
{
  FakeNet net;
  DropInPlayer joiner(DropInConfig(1, 40), net, 1, 40);
  joiner.seat = 1;
  FakeNet::End host_end(net, 0);
  joiner.session.SetValuesHeld(3);
  joiner.session.SetValuesHeld(2);
  joiner.Tick(0, 1);
  net.now = 1000;
  const std::vector<Packet> got = host_end.Receive();
  ASSERT_FALSE(got.empty());
  EXPECT_EQ(got.back().values_ack, 3);
  Packet decoded;
  ASSERT_TRUE(DecodePacket(EncodePacket(got.back()), &decoded));
  EXPECT_EQ(decoded.values_ack, 3);
  Packet none;
  ASSERT_TRUE(DecodePacket(EncodePacket(Packet{}), &none));
  EXPECT_EQ(none.values_ack, -1);
}

TEST(OrcaDropIn, PlanSurvivesTheGuestFloatMode)
{
  Common::FPU::SetSIMDMode(Common::FPU::ROUND_NEAR, true);
  FakeNet net;
  DropInPlayer host(DropInConfig(0, 100), net, 0, 100);
  host.session.AddPeer(1, 100, 40);
  const std::array<SeatPlan, MAX_SEATS> plan = host.session.Plan();
  Common::FPU::LoadDefaultSIMDState();
  EXPECT_EQ(plan[0], (SeatPlan{0, NEVER, 0}));
  EXPECT_EQ(plan[1], (SeatPlan{NEVER, NEVER, 100}));
  EXPECT_EQ(plan[2], SeatPlan{});
}

namespace
{
constexpr Pad SCRIPTED_PAD{0x03, 0x00, 0xEE, 0x00, 0x7F, 0x00, 0x00, 0x00};

struct ScriptedHost
{
  ScriptedHost()
  {
    net.latency = 4;
    FakeGame solo;
    std::vector<Pads> log = PlaySolo(&solo, 0, 300);
    host = std::make_unique<DropInPlayer>(DropInConfig(0, 300), net, 0, 300);
    host->game.m_state = solo.m_state;
    host->game.m_history = solo.m_history;
    host->game.m_ran_pads = solo.m_ran_pads;
    host->session.SetLog(0, log);
    host->session.AddPeer(1, 300, KEYFRAME);
    joiner = std::make_unique<DropInPlayer>(DropInConfig(1, KEYFRAME), net, 1, KEYFRAME);
    joiner->seat = 1;
    joiner->game.m_state = At(solo.m_history, KEYFRAME);
  }
  void PlayUntilLive(int live)
  {
    for (; t < 3000; ++t)
    {
      net.now = t;
      host->Tick(t, 1);
      const bool behind = joiner->session.CatchingUp() ||
                          joiner->session.AuthorityFrame() - joiner->session.CurrentFrame() > 12;
      joiner->Tick(t, behind ? 40 : 1);
      ASSERT_TRUE(host->session.Error().empty()) << host->session.Error();
      ASSERT_TRUE(joiner->session.Error().empty()) << joiner->session.Error();
      if (!joiner->session.CatchingUp() &&
          joiner->session.CurrentFrame() > joiner->session.Plan()[1].plug_from + live)
      {
        break;
      }
    }
    ASSERT_FALSE(joiner->session.CatchingUp());
    roster = host->session.Plan();
    from_frame = joiner->session.CurrentFrame();
    net.queues[1].clear();
  }
  Packet Scripted(std::optional<std::array<SeatPlan, MAX_SEATS>> with_roster) const
  {
    Packet p;
    p.seat = 0;
    p.first_frame = 0;
    p.ack = {-1, joiner->session.CurrentFrame() - 1, -1, -1};
    p.current_frame = joiner->session.CurrentFrame();
    p.roster = with_roster;
    return p;
  }
  Packet ScriptedHistory(std::optional<std::array<SeatPlan, MAX_SEATS>> with_roster,
                       int first = -1) const
  {
    Packet p = Scripted(with_roster);
    p.history_seat = 1;
    p.history_first = first >= 0 ? first : joiner->session.LogEnd();
    for (int i = 0; i < MAX_HISTORY_IN_PACKET; ++i)
    {
      Pads pads;
      pads.fill(UNPLUGGED_PAD);
      pads[0] = Pad{0, 0, 0, 0, 9, 0, 0, 0};
      pads[1] = SCRIPTED_PAD;
      p.history.push_back(pads);
    }
    return p;
  }
  void Send(Packet packet)
  {
    packet.sequence = ++sequence;
    net.now = t;
    net.queues[1].push_back({t, std::move(packet)});
    joiner->Tick(t, 1);
    net.queues[0].clear();
    ++t;
  }
  int ScriptedFrames() const
  {
    int n = 0;
    for (int f = from_frame; f < joiner->session.CurrentFrame(); ++f)
    {
      const auto it = joiner->game.m_ran_pads.find(f);
      if (it != joiner->game.m_ran_pads.end() && it->second[1] == SCRIPTED_PAD)
        ++n;
    }
    return n;
  }

  static constexpr int KEYFRAME = 240;
  FakeNet net;
  std::unique_ptr<DropInPlayer> host;
  std::unique_ptr<DropInPlayer> joiner;
  std::array<SeatPlan, MAX_SEATS> roster{};
  int t = 0;
  int sequence = 1 << 20;
  int from_frame = 0;
};
}

TEST(OrcaDropIn, HistoryPastTheJoinersLiveFrameEndsTheSession)
{
  ScriptedHost h;
  h.PlayUntilLive(60);
  if (::testing::Test::HasFatalFailure())
    return;
  for (int i = 0; i < 3 && h.joiner->session.Error().empty(); ++i)
    h.Send(h.ScriptedHistory(h.roster));
  EXPECT_NE(h.joiner->session.Error().find("past this player's live frame"), std::string::npos)
      << h.joiner->session.Error();
  EXPECT_EQ(h.ScriptedFrames(), 0);
}

TEST(OrcaDropIn, RosterThatTakesBackTheJoinersSeatEndsTheSession)
{
  {
    SCOPED_TRACE("plug and live frame reset");
    ScriptedHost h;
    h.PlayUntilLive(60);
    if (::testing::Test::HasFatalFailure())
      return;
    auto reset = h.roster;
    reset[1] = {NEVER, NEVER, NEVER};
    h.Send(h.Scripted(reset));
    EXPECT_NE(h.joiner->session.Error().find("plug-in"), std::string::npos)
        << h.joiner->session.Error();
    EXPECT_FALSE(h.joiner->session.CatchingUp());
  }
  {
    SCOPED_TRACE("plug moved later");
    ScriptedHost h;
    h.PlayUntilLive(60);
    if (::testing::Test::HasFatalFailure())
      return;
    auto moved = h.roster;
    moved[1].plug_from = h.joiner->session.CurrentFrame() + 4096;
    h.Send(h.Scripted(moved));
    EXPECT_NE(h.joiner->session.Error().find("plug-in"), std::string::npos)
        << h.joiner->session.Error();
  }
  {
    SCOPED_TRACE("live frame moved");
    ScriptedHost h;
    h.PlayUntilLive(60);
    if (::testing::Test::HasFatalFailure())
      return;
    auto moved = h.roster;
    moved[1].live_from = h.joiner->session.CurrentFrame() + 4096;
    h.Send(h.Scripted(moved));
    EXPECT_NE(h.joiner->session.Error().find("live frame"), std::string::npos)
        << h.joiner->session.Error();
  }
  {
    SCOPED_TRACE("reset, history, plug again");
    ScriptedHost h;
    h.PlayUntilLive(60);
    if (::testing::Test::HasFatalFailure())
      return;
    auto reset = h.roster;
    reset[1] = {NEVER, NEVER, NEVER};
    h.Send(h.Scripted(reset));
    for (int i = 0; i < 10; ++i)
      h.Send(h.ScriptedHistory(reset));
    auto again = h.roster;
    again[1].plug_from = h.joiner->session.CurrentFrame() + 1;
    for (int i = 0; i < 300; ++i)
      h.Send(h.Scripted(again));
    EXPECT_FALSE(h.joiner->session.Error().empty());
    EXPECT_EQ(h.ScriptedFrames(), 0);
  }
  {
    SCOPED_TRACE("dropped");
    ScriptedHost h;
    h.PlayUntilLive(60);
    if (::testing::Test::HasFatalFailure())
      return;
    auto dropped = h.roster;
    dropped[1].live_from = NEVER;
    dropped[1].unplug_from = h.joiner->session.CurrentFrame() + 40;
    h.Send(h.Scripted(dropped));
    EXPECT_TRUE(h.joiner->session.Error().empty()) << h.joiner->session.Error();
  }
}

TEST(OrcaDropIn, HostRosterThatEndsTheSessionIsThePeersFault)
{
  const auto ended_by = [](const char* what, const std::function<void(ScriptedHost&)>& send) {
    SCOPED_TRACE(what);
    ScriptedHost h;
    h.PlayUntilLive(60);
    if (::testing::Test::HasFatalFailure())
      return;
    send(h);
    const std::string error = h.joiner->session.Error();
    ASSERT_FALSE(error.empty());
    EXPECT_TRUE(h.joiner->session.ErrorFromPeer()) << error;
    EXPECT_FALSE(Orca::Online::IsDesync(error)) << error;
    EXPECT_TRUE(Orca::Online::ReportSessionEnd(error, h.joiner->session.ErrorFromPeer())) << error;
  };
  ended_by("plug-in reset", [](ScriptedHost& h) {
    auto reset = h.roster;
    reset[1] = {NEVER, NEVER, NEVER};
    h.Send(h.Scripted(reset));
  });
  ended_by("live frame moved", [](ScriptedHost& h) {
    auto moved = h.roster;
    moved[1].live_from = h.joiner->session.CurrentFrame() + 4096;
    h.Send(h.Scripted(moved));
  });
  ended_by("unplug moved later", [](ScriptedHost& h) {
    auto later = h.roster;
    later[1].unplug_from = NEVER;
    auto earlier = h.roster;
    earlier[1].unplug_from = h.joiner->session.CurrentFrame() + 600;
    h.Send(h.Scripted(earlier));
    h.Send(h.Scripted(later));
  });
  ended_by("history past the live frame", [](ScriptedHost& h) {
    for (int i = 0; i < 3 && h.joiner->session.Error().empty(); ++i)
      h.Send(h.ScriptedHistory(h.roster));
  });
  ended_by("an invalid packet", [](ScriptedHost& h) {
    Packet p = h.Scripted(h.roster);
    p.current_frame = -5;
    h.Send(std::move(p));
  });
}

TEST(OrcaDropIn, PlugInBeforeTheJoinerHoldsItsHistoryEndsTheSession)
{
  ScriptedHost h;
  h.from_frame = ScriptedHost::KEYFRAME;
  std::array<SeatPlan, MAX_SEATS> roster{};
  roster[0] = {0, NEVER, 0};
  roster[1] = {NEVER, NEVER, 1 << 29};
  Packet first = h.ScriptedHistory(roster);
  first.history.resize(20);
  for (Pads& pads : first.history)
    pads[1] = UNPLUGGED_PAD;
  h.Send(std::move(first));
  ASSERT_TRUE(h.joiner->session.Error().empty()) << h.joiner->session.Error();
  ASSERT_EQ(h.joiner->session.LogEnd(), ScriptedHost::KEYFRAME + 20);

  auto plugged = roster;
  plugged[1].plug_from =
      std::max(h.joiner->session.CurrentFrame(), h.joiner->session.LogEnd()) + 10;
  h.Send(h.Scripted(plugged));
  EXPECT_NE(h.joiner->session.Error().find("plug-in"), std::string::npos)
      << h.joiner->session.Error();
  auto unplugged = plugged;
  unplugged[1].unplug_from = h.joiner->session.LogEnd();
  for (int i = 0; i < 4; ++i)
    h.Send(h.ScriptedHistory(unplugged));
  for (int i = 0; i < 300; ++i)
    h.Send(h.Scripted(plugged));
  EXPECT_FALSE(h.joiner->session.Error().empty());
  EXPECT_EQ(h.joiner->session.LogEnd(), ScriptedHost::KEYFRAME + 20);
  EXPECT_EQ(h.ScriptedFrames(), 0);
}

TEST(OrcaDropIn, JoinersUnplugOnlyComesEarlier)
{
  for (const bool back_to_never : {true, false})
  {
    SCOPED_TRACE(back_to_never ? "back to never" : "later");
    ScriptedHost h;
    h.PlayUntilLive(60);
    if (::testing::Test::HasFatalFailure())
      return;
    auto leaving = h.roster;
    leaving[1].unplug_from = h.joiner->session.CurrentFrame() + 40;
    h.Send(h.Scripted(leaving));
    ASSERT_TRUE(h.joiner->session.Error().empty()) << h.joiner->session.Error();
    auto earlier = leaving;
    earlier[1].unplug_from -= 10;
    h.Send(h.Scripted(earlier));
    ASSERT_TRUE(h.joiner->session.Error().empty()) << h.joiner->session.Error();
    auto moved = earlier;
    moved[1].unplug_from = back_to_never ? NEVER : earlier[1].unplug_from + 400;
    h.Send(h.Scripted(moved));
    EXPECT_NE(h.joiner->session.Error().find("unplug"), std::string::npos)
        << h.joiner->session.Error();
  }
  {
    SCOPED_TRACE("plugged and unplugged at once");
    ScriptedHost h;
    h.from_frame = ScriptedHost::KEYFRAME;
    std::array<SeatPlan, MAX_SEATS> roster{};
    roster[0] = {0, NEVER, 0};
    roster[1] = {NEVER, NEVER, ScriptedHost::KEYFRAME};
    h.Send(h.Scripted(roster));
    const int plug = h.joiner->session.CurrentFrame() + 10;
    roster[1].plug_from = plug;
    roster[1].unplug_from = plug;
    h.Send(h.Scripted(roster));
    ASSERT_TRUE(h.joiner->session.Error().empty()) << h.joiner->session.Error();
    h.Send(h.ScriptedHistory(roster));
    EXPECT_NE(h.joiner->session.Error().find("past this player's live frame"), std::string::npos)
        << h.joiner->session.Error();
    roster[1].unplug_from = NEVER;
    for (int i = 0; i < 60; ++i)
      h.Send(h.Scripted(roster));
    EXPECT_EQ(h.ScriptedFrames(), 0);
  }
}

TEST(OrcaDropIn, PlugInAsTheFirstRosterTheJoinerHears)
{
  FakeNet net;
  net.latency = 4;
  FakeGame solo;
  std::vector<Pads> log = PlaySolo(&solo, 0, 300);
  DropInPlayer host(DropInConfig(0, 300), net, 0, 300);
  host.game.m_state = solo.m_state;
  host.game.m_history = solo.m_history;
  host.game.m_ran_pads = solo.m_ran_pads;
  host.session.SetLog(0, log);
  host.session.AddPeer(1, 300, 300);
  DropInPlayer joiner(DropInConfig(1, 300), net, 1, 300);
  joiner.seat = 1;
  joiner.game.m_state = solo.m_state;
  bool heard_plug = false;
  int t = 0;
  for (; t < 2000; ++t)
  {
    net.now = t;
    host.Tick(t, 1);
    if (host.session.Plan()[1].plug_from == NEVER)
      net.queues[1].clear();
    const bool behind = joiner.session.CatchingUp() ||
                        joiner.session.AuthorityFrame() - joiner.session.CurrentFrame() > 12;
    joiner.Tick(t, behind ? 40 : 1);
    ASSERT_TRUE(host.session.Error().empty()) << host.session.Error();
    ASSERT_TRUE(joiner.session.Error().empty()) << joiner.session.Error();
    if (!heard_plug && joiner.session.Plan()[1].plug_from != NEVER)
    {
      heard_plug = true;
      EXPECT_EQ(joiner.session.Plan()[1].plug_from, host.session.Plan()[1].plug_from);
    }
    if (heard_plug && joiner.session.CurrentFrame() > joiner.session.Plan()[1].plug_from + 120)
      break;
  }
  EXPECT_TRUE(heard_plug);
  EXPECT_FALSE(joiner.session.CatchingUp());
  EXPECT_GT(joiner.session.CurrentFrame(), joiner.session.Plan()[1].plug_from + 120);
}

TEST(OrcaDropIn, HistoryAheadOfTheJoinerIsBounded)
{
  FakeNet net;
  net.latency = 4;
  constexpr int KEYFRAME = 240;
  DropInPlayer joiner(DropInConfig(1, KEYFRAME), net, 1, KEYFRAME);
  joiner.seat = 1;
  std::array<SeatPlan, MAX_SEATS> roster{};
  roster[0] = {0, NEVER, 0};
  roster[1] = {NEVER, NEVER, 1 << 29};
  int sequence = 0;
  int t = 0;
  for (int i = 0; i < 2000 && joiner.session.Error().empty(); ++i, ++t)
  {
    Packet p;
    p.seat = 0;
    p.sequence = ++sequence;
    p.ack = {-1, -1, -1, -1};
    p.current_frame = joiner.session.CurrentFrame();
    p.roster = roster;
    p.history_seat = 1;
    p.history_first = joiner.session.LogEnd();
    for (int k = 0; k < MAX_HISTORY_IN_PACKET; ++k)
    {
      Pads pads;
      pads.fill(UNPLUGGED_PAD);
      pads[0] = Pad{static_cast<u8>(k % 4), 0, 0, 0, 9, 0, 0, 0};
      p.history.push_back(pads);
    }
    net.now = t;
    net.queues[1].push_back({t, std::move(p)});
    joiner.Tick(t, 1);
    net.queues[0].clear();
  }
  EXPECT_TRUE(joiner.session.Error().empty()) << joiner.session.Error();
  EXPECT_TRUE(joiner.session.CatchingUp());
  EXPECT_LE(joiner.session.LogEnd(), joiner.session.CurrentFrame() + 4096 + 1);
  EXPECT_GT(joiner.session.LogEnd(), KEYFRAME + 4096 - MAX_HISTORY_IN_PACKET);
}

TEST(OrcaSessionDirect, StaleCopiesNeverPutTheRosterOrTheClockBack)
{
  struct Scripted final : Transport
  {
    void Send(const Packet&) override {}
    std::vector<Packet> Receive() override { return std::exchange(queue, {}); }
    bool Connected() const override { return true; }
    std::vector<Packet> queue;
  };
  Config config;
  config.seats = 2;
  config.local_seat = 1;
  config.authority_seat = 0;
  config.time_sync = false;
  config.max_delay = config.input_delay;
  std::array<SeatPlan, MAX_SEATS> plan{};
  plan[0] = {0, NEVER, 0};
  plan[1] = {0, NEVER, 0};
  config.plan = plan;
  FakeGame game;
  Scripted net;
  Session session(config, game, net);
  int completed = -1;
  const auto run_to = [&](int frame) {
    for (int guard = 0; guard < 4000 && session.CurrentFrame() < frame; ++guard)
    {
      const Step step = session.OnBoundary(completed, Pad{});
      if (step.kind == StepKind::Ended || step.kind == StepKind::Stall)
        return;
      if (step.kind != StepKind::Wait)
        completed = game.Run();
    }
  };
  const auto host_pad = [](int f) { return Pad{static_cast<u8>(f % 4), 0, static_cast<u8>(f), 0, 7, 0, 0, 0}; };
  std::array<SeatPlan, MAX_SEATS> roster = plan;
  const auto host_packet = [&](int sequence, int first, int count, int current) {
    Packet p;
    p.seat = 0;
    p.sequence = sequence;
    p.first_frame = first;
    for (int f = first; f < first + count; ++f)
      p.pads.push_back(host_pad(f));
    p.ack = {-1, -1, -1, -1};
    p.current_frame = current;
    p.roster = roster;
    return p;
  };

  Packet older = host_packet(1, 0, 6, 4);
  roster[1].unplug_from = 400;
  Packet newer = host_packet(2, 0, 12, 10);
  net.queue = {newer, older};
  run_to(1);
  ASSERT_TRUE(session.Error().empty()) << session.Error();
  EXPECT_EQ(session.Plan()[1].unplug_from, 400);
  EXPECT_EQ(session.AuthorityFrame(), 10);
  net.queue = {newer, newer};
  run_to(2);
  ASSERT_TRUE(session.Error().empty()) << session.Error();
  EXPECT_EQ(session.Plan()[1].unplug_from, 400);

  net.queue = {host_packet(4, 8, 16, 14)};
  run_to(10);
  ASSERT_TRUE(session.Error().empty()) << session.Error();
  EXPECT_EQ(session.GetStats().confirmed_frame, 23);
  net.queue = {host_packet(10, 16, 40, 30)};
  run_to(40);
  ASSERT_TRUE(session.Error().empty()) << session.Error();
  EXPECT_EQ(session.GetStats().confirmed_frame, 55);
  EXPECT_EQ(session.AuthorityFrame(), 30);
  Packet stale = host_packet(5, 0, 30, 12);
  stale.roster = plan;
  net.queue = {stale};
  run_to(41);
  EXPECT_EQ(session.AuthorityFrame(), 30);
  EXPECT_EQ(session.Plan()[1].unplug_from, 400);

  net.queue = {host_packet(11, 56, 64, 100), host_packet(12, 100, 60, 150)};
  run_to(140);
  ASSERT_TRUE(session.Error().empty()) << session.Error();
  ASSERT_TRUE(game.SnapshotChecksum(60).has_value());
  ASSERT_TRUE(game.SnapshotChecksum(120).has_value());
  const int matched = session.GetStats().checksums_matched;
  Packet with_checksum = host_packet(13, 150, 4, 152);
  with_checksum.checksum_frame = 60;
  with_checksum.checksum = *game.SnapshotChecksum(60);
  net.queue = {with_checksum, with_checksum};
  run_to(142);
  ASSERT_TRUE(session.Error().empty()) << session.Error();
  EXPECT_EQ(session.GetStats().checksums_matched, matched + 1);
  Packet direct_copy = host_packet(15, 150, 8, 156);
  Packet relay_copy = direct_copy;
  relay_copy.checksum_frame = 120;
  relay_copy.checksum = *game.SnapshotChecksum(120) ^ 1;
  net.queue = {direct_copy, relay_copy};
  run_to(144);
  EXPECT_NE(session.Error().find("Desync at frame 120"), std::string::npos) << session.Error();
}

TEST(OrcaSessionDirect, FriendJoinsOverADirectLink)
{
  FakeNet net;
  net.latency = 6;
  net.jitter = 2;
  net.direct_latency = 0;
  net.direct_jitter = 2;
  net.direct_loss_percent = 20;
  FakeGame solo;
  std::vector<Pads> log = PlaySolo(&solo, 0, 300);
  const int keyframe = 240;
  auto host = std::make_unique<DropInPlayer>(DropInConfig(0, 300), net, 0, 300);
  host->game.m_state = solo.m_state;
  host->game.m_history = solo.m_history;
  host->game.m_ran_pads = solo.m_ran_pads;
  host->session.SetLog(0, log);
  host->session.AddPeer(1, 300, keyframe);
  auto joiner = std::make_unique<DropInPlayer>(DropInConfig(1, keyframe), net, 1, keyframe);
  joiner->seat = 1;
  joiner->game.m_state = At(solo.m_history, keyframe);
  int t = 0;
  for (; t < 1500; ++t)
  {
    net.now = t;
    if (t % 30 == 0)
    {
      for (DropInPlayer* p : {host.get(), joiner.get()})
        p->session.OnRoundTrip(7 * 1000 / 60);
    }
    host->Tick(t, 1);
    const bool behind = joiner->session.CatchingUp() ||
                        joiner->session.AuthorityFrame() - joiner->session.CurrentFrame() > 12;
    joiner->Tick(t, behind ? 40 : 1);
    ASSERT_TRUE(host->session.Error().empty()) << host->session.Error();
    ASSERT_TRUE(joiner->session.Error().empty()) << joiner->session.Error();
  }
  ASSERT_FALSE(joiner->session.CatchingUp());
  EXPECT_GT(net.direct_delivered, 1000);
  const int plugged_at = host->session.Plan()[1].plug_from;
  ASSERT_NE(plugged_at, NEVER);
  EXPECT_EQ(joiner->session.Plan()[1].plug_from, plugged_at);
  const int confirmed =
      std::min({host->session.GetStats().confirmed_frame, host->session.CurrentFrame() - 1,
                joiner->session.GetStats().confirmed_frame, joiner->session.CurrentFrame() - 1});
  ASSERT_GT(confirmed, plugged_at + 600);
  for (int f = keyframe; f <= confirmed; ++f)
  {
    ASSERT_EQ(At(host->game.m_history, f), At(joiner->game.m_history, f)) << "frame " << f;
    ASSERT_EQ(At(host->game.m_ran_pads, f), At(joiner->game.m_ran_pads, f)) << "frame " << f;
  }
  for (const DropInPlayer* p : {host.get(), joiner.get()})
  {
    EXPECT_GT(p->session.GetStats().checksums_matched, 10);
    EXPECT_LE(p->session.GetStats().checksums_matched, (confirmed - keyframe) / 60 + 1);
  }

  joiner->session.RequestLeave();
  for (int end = t + 200; t < end && !(joiner->session.LeftDone() && host->session.Idle()); ++t)
  {
    net.now = t;
    host->Tick(t, 1);
    if (!joiner->session.LeftDone())
      joiner->Tick(t, 1);
  }
  ASSERT_TRUE(joiner->session.LeftDone());
  ASSERT_TRUE(host->session.Idle());
  EXPECT_EQ(joiner->session.Plan()[1].unplug_from, host->session.Plan()[1].unplug_from);
}

TEST(OrcaSessionDirect, DirectLinkBesideTheRelay)
{
  const auto side = [](int relay_ms, bool direct) {
    RelayMatch::Side s;
    s.relay_ms = relay_ms;
    s.input_every = 5;
    if (direct)
    {
      s.direct_ms = 1;
      s.direct_jitter_ms = 3;
      s.direct_loss_percent = 10;
    }
    return s;
  };
  RelayMatch relay(Config{}, side(8, false), side(40, false));
  relay.Play(3600);
  relay.Print("relay only");
  relay.ExpectClean();
  RelayMatch both(Config{}, side(8, true), side(40, true));
  both.Play(3600);
  both.Print("direct + relay");
  both.ExpectClean();
  EXPECT_GT(both.CompareConfirmed(), 3400);
  for (int i = 0; i < 2; ++i)
  {
    const Stats& s = both.SessionOf(i).GetStats();
    EXPECT_GT(both.DirectReceivedBy(i), 3000);
    EXPECT_GE(s.checksums_matched, 55);
    EXPECT_LE(s.checksums_matched, 60);
    EXPECT_LT(std::abs(s.frame_advantage), 1.0);
    EXPECT_LE(s.waits, 3);
    EXPECT_EQ(s.delay, 2);
    EXPECT_LT(s.rollbacks * 4, relay.SessionOf(i).GetStats().rollbacks) << "side " << i;
  }
}

TEST(OrcaSessionDirect, RoomBlipWithLiveInputsKeepsEveryFrameHash)
{
  using namespace std::chrono_literals;
  Config config;
  Match control(config, 0, 0);
  Match recovery(config, 0, 0);
  for (Match* match : {&control, &recovery})
  {
    match->net.direct_latency = 0;
    match->rtt_samples = false;
  }
  PeerRoomPresence peer;
  RoomReconnect room;
  for (int tick = 0; tick < 1200; ++tick)
  {
    const auto now = RoomClock::time_point{} + std::chrono::milliseconds(tick * 1000 / 60);
    if (tick == 200)
    {
      peer.Away(now);
      ASSERT_TRUE(room.Start(now, 0));
      recovery.net.relay_open = false;
    }
    if (tick >= 200 && tick < 1000)
    {
      EXPECT_EQ(peer.Check(now, now), RoomLoss::None);
      EXPECT_EQ(room.Check(now), RoomLoss::None);
    }
    if (tick == 1000)
    {
      ASSERT_TRUE(peer.Back(now, true));
      room.since.reset();
      recovery.net.relay_open = true;
    }
    control.Play(1);
    recovery.Play(1);
  }
  EXPECT_GT(recovery.CompareConfirmed(), 1100);
  for (const auto& pair : {std::pair{control.a.get(), recovery.a.get()},
                          std::pair{control.b.get(), recovery.b.get()}})
  {
    EXPECT_TRUE(pair.second->session.Error().empty());
    EXPECT_EQ(pair.first->game.m_history, pair.second->game.m_history);
    EXPECT_EQ(pair.first->game.m_ran_pads, pair.second->game.m_ran_pads);
    EXPECT_EQ(pair.first->session.CurrentFrame(), pair.second->session.CurrentFrame());
    EXPECT_EQ(pair.first->session.GetStats().checksums_matched,
              pair.second->session.GetStats().checksums_matched);
  }
}

namespace
{
struct FriendsMesh
{
  struct Pending
  {
    int at;
    Packet packet;
  };
  std::array<std::vector<Pending>, MAX_SEATS> pending;
  std::array<bool, MAX_SEATS> blackout{};
  std::array<bool, MAX_SEATS> hold_departed{};
  std::array<int, MAX_SEATS> latency{};
  std::array<std::array<int, MAX_SEATS>, MAX_SEATS> pair_latency{};
  std::array<std::array<int, MAX_SEATS>, MAX_SEATS> drop_packets{};
  std::array<int, MAX_SEATS> history_sends{};
  std::array<Packet, MAX_SEATS> last_sent;
  std::array<int, MAX_SEATS> reported_rtt{-1, -1, -1, -1};
  std::vector<std::string> wire;
  int tick = 0;

  struct End final : Transport
  {
    End(FriendsMesh& network, int local) : mesh(network), seat(local) {}
    void Deliver(int to, const Packet& original)
    {
      Packet packet = original;
      if (mesh.reported_rtt[seat] >= 0)
        packet.rtt = mesh.reported_rtt[seat];
      mesh.last_sent[seat] = packet;
      if (mesh.drop_packets[seat][to] > 0)
      {
        --mesh.drop_packets[seat][to];
        return;
      }
      if (mesh.hold_departed[to])
        for (auto& [departed_seat, stay] : packet.departed)
          stay.pads.clear();
      if (!mesh.blackout[seat] && !mesh.blackout[to])
      {
        Packet decoded;
        EXPECT_TRUE(DecodePacket(EncodePacket(packet), &decoded));
        mesh.pending[to].push_back({mesh.tick + mesh.latency[to] + mesh.latency[seat] + mesh.pair_latency[seat][to], std::move(decoded)});
        mesh.wire.push_back(EncodePacket(packet));
        if (packet.history_seat == to && !packet.history.empty())
          ++mesh.history_sends[to];
      }
    }
    void Send(const Packet& packet) override
    {
      for (int to = 0; to < MAX_SEATS; ++to)
        if (to != seat)
          Deliver(to, packet);
    }
    void SendToSeats(const std::map<int, Packet>& packets) override
    {
      for (const auto& [to, packet] : packets)
        Deliver(to, packet);
    }
    std::vector<Packet> Receive() override
    {
      std::vector<Packet> packets;
      auto& queue = mesh.pending[seat];
      for (auto it = queue.begin(); it != queue.end();)
      {
        if (it->at <= mesh.tick)
        {
          if (!mesh.blackout[seat] && !mesh.blackout[it->packet.seat])
            packets.push_back(std::move(it->packet));
          it = queue.erase(it);
        }
        else
          ++it;
      }
      return packets;
    }
    bool Connected() const override { return true; }
    FriendsMesh& mesh;
    int seat;
  };
};

struct MeshPlayer
{
  MeshPlayer(FriendsMesh& network, int local, int start, bool enabled)
      : transport(network, local), completed(start - 1)
  {
    Config config = DropInConfig(local, start);
    config.per_peer_inputs = enabled;
    config.time_sync = false;
    session = std::make_unique<Session>(config, game, transport);
    session->SetValuesHeld(4);
  }
  int Tick(int wall, int frames = 1)
  {
    int ran = 0;
    for (int guard = 0; guard < 4000 && ran < frames; ++guard)
    {
      Pad pad{static_cast<u8>((wall / 7 + transport.seat * 3) % 4),
              static_cast<u8>(wall % 3), 0, 0, static_cast<u8>(transport.seat + 1), 0, 0, 0};
      if (held_start)
        pad[0] |= PAD_WIRE_START_HIGH;
      if (unplugged_pad)
        pad = UNPLUGGED_PAD;
      const Step step = session->OnBoundary(completed, pad);
      if (step.kind == StepKind::Ended || step.kind == StepKind::Wait || step.kind == StepKind::Stall)
        return ran;
      const bool fresh = step.kind == StepKind::Run && !session->Resimulating();
      for (int seat = 0; seat < MAX_SEATS; ++seat)
        ran_plugged[step.frame][seat] = session->Plugged(seat, step.frame);
      completed = game.Run();
      if (fresh)
        ++ran;
    }
    return ran;
  }
  FriendsMesh::End transport;
  std::map<int, std::array<bool, MAX_SEATS>> ran_plugged;
  FakeGame game;
  std::unique_ptr<Session> session;
  int completed;
  bool held_start = false;
  bool unplugged_pad = false;
};

struct FourFriends
{
  explicit FourFriends(bool enabled = true, int friends = 3)
  {
    players[0] = std::make_unique<MeshPlayer>(mesh, 0, 300, enabled);
    auto log = PlaySolo(&players[0]->game, 0, 300);
    players[0]->session->SetLog(240, {log.begin() + 240, log.end()});
    std::vector<int> seats;
    for (int seat = 1; seat <= friends; ++seat)
    {
      seats.push_back(seat);
      players[seat] = std::make_unique<MeshPlayer>(mesh, seat, 240, enabled);
      players[seat]->game.m_state = At(players[0]->game.m_history, 240);
      players[0]->session->RequireValues(seat, 4);
    }
    if (friends > 1)
      players[0]->session->AddPeerBatch(seats, 300, 240);
    else
      players[0]->session->AddPeer(1, 300, 240);
  }
  void Play(int ticks)
  {
    for (int count = 0; count < ticks; ++count, ++mesh.tick)
    {
      for (int seat = 0; seat < MAX_SEATS; ++seat)
      {
        if (!players[seat] || disabled[seat])
          continue;
        const bool catching = seat > 0 && players[seat]->session->CatchingUp();
        players[seat]->Tick(mesh.tick, catching ? 40 : 1);
      }
    }
  }
  void Compare(int first = 300)
  {
    int through = NEVER;
    for (int seat = 0; seat < MAX_SEATS; ++seat)
    {
      if (players[seat] && !disabled[seat])
      {
        EXPECT_TRUE(players[seat]->session->Error().empty()) << seat << ": " << players[seat]->session->Error();
        through = std::min(through, players[seat]->session->FinalFrame() - 1);
        EXPECT_LE(players[seat]->session->GetStats().deepest_rollback, 10);
      }
    }
    std::string state;
    if (through <= first + 50)
      for (int seat = 0; seat < MAX_SEATS; ++seat)
        if (players[seat] && !disabled[seat])
          state += std::to_string(seat) + ": " + players[seat]->session->Describe() + "\n";
    ASSERT_GT(through, first + 50) << state;
    for (int frame = first; frame <= through; ++frame)
    {
      for (int seat = 1; seat < MAX_SEATS; ++seat)
      {
        if (players[seat] && !disabled[seat])
        {
          ASSERT_EQ(At(players[0]->game.m_history, frame), At(players[seat]->game.m_history, frame))
              << "seat " << seat << " frame " << frame;
          ASSERT_EQ(At(players[0]->ran_plugged, frame), At(players[seat]->ran_plugged, frame))
              << "controller ports at seat " << seat << " frame " << frame;
        }
      }
    }
  }
  FriendsMesh mesh;
  std::array<std::unique_ptr<MeshPlayer>, MAX_SEATS> players;
  std::array<bool, MAX_SEATS> disabled{};
};
}

TEST(OrcaDoubles, ThreeRebuildsPlugTogetherAfterTheSlowest)
{
  FourFriends friends;
  friends.disabled[3] = true;
  friends.Play(400);
  for (int seat = 1; seat < MAX_SEATS; ++seat)
    EXPECT_EQ(friends.players[0]->session->Plan()[seat].plug_from, NEVER);
  for (int seat = 1; seat < MAX_SEATS; ++seat)
    EXPECT_GT(friends.mesh.history_sends[seat], 0);
  friends.disabled[3] = false;
  friends.Play(800);
  const int plug = friends.players[0]->session->Plan()[1].plug_from;
  ASSERT_NE(plug, NEVER);
  for (int seat = 1; seat < MAX_SEATS; ++seat)
    EXPECT_EQ(friends.players[0]->session->Plan()[seat].plug_from, plug);
  friends.Compare();
}

TEST(OrcaDoubles, APeerLeavesAndTheOtherThreeContinue)
{
  FourFriends friends;
  friends.Play(300);
  const int before = friends.players[0]->session->CurrentFrame();
  friends.players[0]->session->DropSeat(3);
  friends.disabled[3] = true;
  friends.Play(300);
  EXPECT_GT(friends.players[0]->session->CurrentFrame(), before + 250);
  friends.Compare();
}

TEST(OrcaDoubles, OneBlackoutStallsAllAndRecoversTheSameSessions)
{
  FourFriends friends;
  friends.Play(300);
  std::array<Session*, MAX_SEATS> sessions;
  for (int seat = 0; seat < MAX_SEATS; ++seat)
    sessions[seat] = friends.players[seat]->session.get();
  friends.mesh.blackout[3] = true;
  friends.Play(40);
  std::array<int, MAX_SEATS> stopped;
  for (int seat = 0; seat < MAX_SEATS; ++seat)
    stopped[seat] = sessions[seat]->CurrentFrame();
  friends.Play(40);
  for (int seat = 0; seat < MAX_SEATS; ++seat)
    EXPECT_EQ(sessions[seat]->CurrentFrame(), stopped[seat]);
  for (int seat = 0; seat < 3; ++seat)
    EXPECT_EQ(sessions[seat]->MissingInputSeats(), (std::vector<int>{3}));
  friends.mesh.blackout[3] = false;
  friends.Play(400);
  for (int seat = 0; seat < MAX_SEATS; ++seat)
  {
    EXPECT_EQ(friends.players[seat]->session.get(), sessions[seat]);
    EXPECT_GT(sessions[seat]->CurrentFrame(), stopped[seat] + 300);
  }
  friends.Compare();
}

TEST(OrcaDoubles, RebuildingRetainedPeerDoesNotThrottleSeatedPeers)
{
  FourFriends friends;
  friends.Play(300);
  friends.players[0]->session->DropSeat(3);
  friends.disabled[3] = true;
  friends.Play(30);
  const int live = friends.players[0]->session->CurrentFrame();
  const int keyframe = live - 20;
  friends.players[3] = std::make_unique<MeshPlayer>(friends.mesh, 3, keyframe, true);
  friends.players[3]->game.m_state = At(friends.players[0]->game.m_history, keyframe);
  friends.mesh.pending[3].clear();
  friends.players[0]->session->AddPeer(3, live, keyframe);
  friends.players[0]->session->RequireValues(3, 4);
  friends.Play(400);
  for (int seat = 0; seat < 3; ++seat)
    EXPECT_GT(friends.players[seat]->session->CurrentFrame(), live + 350);
  friends.disabled[3] = false;
  friends.Play(800);
  friends.Compare(live);
}

TEST(OrcaDoubles, TwoPlayerPacketsAndEveryFrameRemainIdentical)
{
  FourFriends ordinary(false, 1);
  FourFriends enabled(true, 1);
  ordinary.Play(400);
  enabled.Play(400);
  EXPECT_EQ(ordinary.mesh.wire, enabled.mesh.wire);
  for (int seat = 0; seat < 2; ++seat)
  {
    EXPECT_EQ(ordinary.players[seat]->game.m_history, enabled.players[seat]->game.m_history);
    EXPECT_EQ(ordinary.players[seat]->session->Plan(), enabled.players[seat]->session->Plan());
  }
  ordinary.Compare();
  enabled.Compare();
}

TEST(OrcaDoubles, WorstOfSixPairsStaysWithinRollbackLimit)
{
  FourFriends friends;
  friends.mesh.latency = {0, 1, 2, 3};
  friends.Play(1800);
  friends.Compare();
}

TEST(OrcaDoubles, StartWaitsForTheWholeBatchAndTheGateTravelsWithTheRoster)
{
  static_assert(PAD_WIRE_START_HIGH == (PAD_BUTTON_START >> 8));
  FourFriends friends;
  friends.players[0]->held_start = true;
  friends.disabled[3] = true;
  friends.Play(200);
  for (int frame = 303; frame < friends.players[0]->completed; ++frame)
    EXPECT_EQ(At(friends.players[0]->game.m_ran_pads, frame)[0][0] & PAD_WIRE_START_HIGH, 0);
  friends.disabled[3] = false;
  friends.Play(400);
  const int plug = friends.players[0]->session->Plan()[3].plug_from;
  ASSERT_NE(plug, NEVER);
  EXPECT_NE(At(friends.players[0]->game.m_ran_pads, plug + 20)[0][0] & PAD_WIRE_START_HIGH, 0);
  friends.Compare();
  Packet gate;
  gate.roster = friends.players[0]->session->Plan();
  gate.start_gate = {300, plug};
  Packet decoded;
  ASSERT_TRUE(DecodePacket(EncodePacket(gate), &decoded));
  EXPECT_EQ(decoded.start_gate, gate.start_gate);
}

TEST(OrcaDoubles, AThirdPeersChecksumIsComparedEvenWhenOthersReportTheSameFrame)
{
  FourFriends friends;
  friends.Play(300);
  const auto& checksums = friends.players[0]->session->LocalChecksums();
  ASSERT_FALSE(checksums.empty());
  const auto [frame, hash] = *checksums.rbegin();
  Packet wrong;
  wrong.seat = 3;
  wrong.sequence = 100000;
  wrong.current_frame = friends.players[0]->session->CurrentFrame();
  wrong.checksum_frame = frame;
  wrong.checksum = hash ^ 1;
  Packet correct = wrong;
  correct.seat = 1;
  correct.checksum = hash;
  friends.players[0]->session->AddPeer(3, wrong.current_frame, wrong.current_frame);
  friends.players[0]->session->AddPeer(1, wrong.current_frame, wrong.current_frame);
  friends.mesh.pending[0].clear();
  friends.mesh.pending[0].push_back({friends.mesh.tick, wrong});
  friends.mesh.pending[0].push_back({friends.mesh.tick, correct});
  friends.players[0]->Tick(friends.mesh.tick);
  EXPECT_NE(friends.players[0]->session->Error().find("Desync"), std::string::npos);
}

TEST(OrcaDoubles, ActivePredictionsUseAFinalCheckpointForResyncCapture)
{
  FourFriends friends;
  for (auto& player : friends.players)
    player->session->SetFixedDelay(2);
  friends.Play(200);
  ASSERT_NE(friends.players[0]->session->Plan()[3].plug_from, NEVER);
  friends.mesh.latency = {0, 1, 2, 12};
  auto& host = *friends.players[0];
  for (int tick = 0; tick < 60 && host.session->Settled(); ++tick)
    friends.Play(1);
  ASSERT_FALSE(host.session->Settled()) << host.session->Describe();
  const int next = host.session->CurrentFrame();
  const auto capture = Rollback::OnlineMatch::ChooseJoinCapture(host.session.get(), true, next, 300);
  ASSERT_TRUE(capture);
  ASSERT_TRUE(capture->hash);
  EXPECT_LT(capture->frame, next);
  EXPECT_EQ(*capture->hash, At(host.game.m_history, capture->frame));
  EXPECT_FALSE(Rollback::OnlineMatch::ChooseJoinCapture(host.session.get(), false, next, 300));
  EXPECT_FALSE(Rollback::OnlineMatch::ChooseJoinCapture(host.session.get(), true, next,
                                                       capture->frame + 1));
  friends.mesh.latency.fill(0);
  friends.Play(300);
  friends.Compare();
}

TEST(OrcaDoubles, UnevenBlackoutDropKeepsEverySurvivorHash)
{
  for (const bool host_hears_first : {false, true})
  {
    SCOPED_TRACE(host_hears_first);
    FourFriends friends;
    friends.mesh.latency = {0, 1, 2, 1};
    friends.mesh.pair_latency[3][host_hears_first ? 1 : 0] = 8;
    friends.Play(400);
    friends.mesh.blackout[3] = true;
    friends.Play(50);
    const int before = friends.players[0]->session->CurrentFrame();
    EXPECT_NE(friends.mesh.last_sent[0].ack[3], friends.mesh.last_sent[1].ack[3]);
    friends.players[0]->session->DropSeat(3);
    friends.disabled[3] = true;
    friends.Play(400);
    EXPECT_GT(friends.players[0]->session->CurrentFrame(), before + 300);
    EXPECT_GT(friends.players[1]->session->CurrentFrame(), before + 300);
    friends.Compare();
  }
}

TEST(OrcaDoubles, UnevenRetainedRecoveryKeepsEverySurvivorHash)
{
  FourFriends friends;
  friends.mesh.latency = {0, 1, 2, 1};
  friends.mesh.pair_latency[3][0] = 8;
  friends.Play(400);
  friends.mesh.blackout[3] = true;
  friends.Play(50);
  auto& host = *friends.players[0];
  host.session->DropSeat(3);
  const int live = host.session->CurrentFrame();
  const int keyframe = live - 30;
  friends.disabled[3] = true;
  host.session->AddPeer(3, live, keyframe);
  friends.Play(200);
  friends.players[3] = std::make_unique<MeshPlayer>(friends.mesh, 3, keyframe, true);
  friends.players[3]->game.m_state = At(host.game.m_history, keyframe);
  friends.mesh.pending[3].clear();
  friends.mesh.blackout[3] = false;
  friends.disabled[3] = false;
  friends.Play(1000);
  friends.Compare(keyframe);
}

TEST(OrcaDoubles, SeatedStartAcrossUnevenBatchArrivalAndDrop)
{
  FourFriends friends(true, 1);
  friends.mesh.latency = {0, 2, 1, 3};
  friends.Play(400);
  friends.players[1]->held_start = true;
  friends.Play(40);
  auto& host = *friends.players[0];
  const int live = host.session->CurrentFrame();
  host.session->AddPeerBatch({2, 3}, live, live - 20);
  friends.Play(40);
  host.session->DropSeat(2);
  host.session->DropSeat(3);
  friends.Play(300);
  friends.Compare();
}

TEST(OrcaDoubles, CatchingUpStallNamesItsMissingSeatWithUnevenLatency)
{
  FourFriends friends;
  friends.mesh.latency = {0, 1, 3, 2};
  friends.mesh.blackout[0] = true;
  friends.Play(50);
  for (int seat = 1; seat < MAX_SEATS; ++seat)
  {
    ASSERT_TRUE(friends.players[seat]->session->CatchingUp());
    EXPECT_EQ(friends.players[seat]->session->MissingInputSeats(), (std::vector<int>{0}));
  }
}

TEST(OrcaDoubles, BoundaryWaitsForHostRosterAfterAnotherFriendsHoldExpires)
{
  FourFriends friends;
  friends.mesh.latency = {0, 1, 3, 2};
  friends.Play(400);
  friends.mesh.blackout[3] = true;
  friends.Play(60);
  auto& joiner = *friends.players[1];
  ASSERT_EQ(joiner.session->MissingInputSeats(), (std::vector<int>{3}));
  EXPECT_TRUE(Rollback::OnlineMatch::WaitForFriendsRoster(*joiner.session, true, true));
  friends.disabled[1] = true;
  bool silent = false;
  int polls = 0;
  const auto step = Rollback::StepSession(*joiner.session, joiner.game, joiner.completed,
      [] { return Pad{}; }, [&] {
        if (++polls == 20)
        {
          friends.players[0]->session->DropSeat(3);
          friends.disabled[3] = true;
        }
        if (polls >= 20)
          friends.Play(1);
        return polls > 200;
      }, std::chrono::milliseconds(1), std::chrono::milliseconds(2), &silent, nullptr,
      [&](auto) { return Rollback::OnlineMatch::WaitForFriendsRoster(*joiner.session, true, true); });
  EXPECT_FALSE(silent);
  EXPECT_NE(step.kind, StepKind::Ended);
  if (step.kind == StepKind::Run || step.kind == StepKind::Rollback)
    joiner.completed = joiner.game.Run();
  friends.disabled[1] = false;
  friends.Play(300);
  friends.Compare();
}

TEST(OrcaDoubles, DelayedBatchRosterRetainsSeatedLocalInputs)
{
  FourFriends friends(true, 1);
  friends.mesh.latency = {0, 4, 1, 2};
  friends.players[0]->session->SetFixedDelay(6);
  friends.players[1]->session->SetFixedDelay(6);
  friends.Play(400);
  auto& host = *friends.players[0];
  const int live = host.session->CurrentFrame();
  const int keyframe = live - 20;
  friends.mesh.pair_latency[0][1] = 30;
  for (int seat : {2, 3})
  {
    friends.players[seat] = std::make_unique<MeshPlayer>(friends.mesh, seat, keyframe, true);
    friends.players[seat]->game.m_state = At(host.game.m_history, keyframe);
  }
  host.session->AddPeerBatch({2, 3}, live, keyframe);
  friends.Play(1200);
  EXPECT_NE(host.session->Plan()[2].plug_from, NEVER);
  EXPECT_NE(host.session->Plan()[3].plug_from, NEVER);
  friends.Compare(keyframe);
}

TEST(OrcaDoubles, BatchHistorySenderCountsOnlyTheFramesItEmits)
{
  FriendsMesh mesh;
  mesh.latency = {0, 2, 1, 3};
  MeshPlayer host(mesh, 0, 600, true);
  host.session->SetLog(0, std::vector<Pads>(600));
  host.session->AddPeerBatch({1, 2, 3}, 600, 0);
  host.Tick(0);
  ASSERT_FALSE(mesh.wire.empty());
  for (const auto& wire : mesh.wire)
  {
    Packet packet;
    ASSERT_TRUE(DecodePacket(wire, &packet));
    EXPECT_LE(packet.history.size(), 128u);
  }
}

TEST(OrcaDoubles, BoundaryEmptyMissingSelectionUsesOrdinaryRecovery)
{
  FourFriends friends;
  friends.mesh.latency = {0, 2, 1, 3};
  friends.Play(400);
  EXPECT_FALSE(Rollback::OnlineMatch::FriendsMissingInputs(*friends.players[0]->session, true));
  EXPECT_FALSE(Rollback::OnlineMatch::WaitForFriendsRoster(*friends.players[1]->session, true, true));
}

TEST(OrcaLegacy044, InitialSessionPacketGolden)
{
  FakeNet net;
  FakeNet::End transport(net, 0);
  FakeGame game;
  Config config;
  config.time_sync = false;
  Session session(config, game, transport);
  ASSERT_EQ(session.OnBoundary(-1, Pad{1}).kind, StepKind::Run);
  ASSERT_FALSE(net.queues[1].empty());
  EXPECT_EQ(EncodePacket(net.queues[1].back().packet),
      R"({"a":[-1,-1,-1,-1],"c":0,"f":0,"j":0,"k":"p","n":0,"p":"000000000000000000000000000000000100000000000000","q":1,"s":0,"sp":[0,0,0,0]})");
}

TEST(OrcaDoubles, BoundaryDropsOnlyMissingFriendsAfterUnevenBlackout)
{
  FourFriends friends;
  friends.mesh.latency = {0, 2, 1, 3};
  friends.mesh.pair_latency[3][0] = 7;
  friends.Play(400);
  friends.mesh.blackout[3] = true;
  friends.Play(60);
  EXPECT_EQ(Rollback::OnlineMatch::DropMissingFriends(*friends.players[0]->session, true),
            std::optional(std::vector<int>{3}));
  friends.disabled[3] = true;
  friends.Play(400);
  friends.Compare();
  EXPECT_FALSE(Rollback::OnlineMatch::DropMissingFriends(*friends.players[0]->session, true));
}

TEST(OrcaDoubles, LateSeatStallNamesMissingPeerWithUnevenLatency)
{
  Config config;
  config.per_peer_inputs = true;
  Match match(config, 1, 0);
  match.net.extra = {2, 0};
  match.net.tamper = [](int from, Packet* packet) {
    if (from != 1)
      return;
    const int keep = packet->current_frame - 5 - packet->first_frame + 1;
    if (keep < static_cast<int>(packet->pads.size()))
      packet->pads.resize(std::max(keep, 0));
  };
  match.Play(900);
  ASSERT_TRUE(match.a->session.LateSeat(1));
  match.net.tamper = [](int from, Packet* packet) { if (from == 1) packet->pads.clear(); };
  match.Play(30);
  EXPECT_EQ(match.a->session.MissingInputSeats(), (std::vector<int>{1}));
}

TEST(OrcaDoubles, BatchPlugMarginIncludesSeatedPeers)
{
  FourFriends friends(true, 1);
  friends.mesh.latency = {0, 2, 1, 3};
  friends.mesh.reported_rtt = {0, 400, 0, 0};
  friends.players[0]->session->OnRoundTrip(0);
  friends.players[0]->session->OnRoundTrip(0);
  friends.players[0]->session->OnRoundTrip(0);
  friends.Play(400);
  auto& host = *friends.players[0];
  const int live = host.session->CurrentFrame();
  const int keyframe = live - 20;
  for (int seat : {2, 3})
  {
    friends.players[seat] = std::make_unique<MeshPlayer>(friends.mesh, seat, keyframe, true);
    friends.players[seat]->game.m_state = At(host.game.m_history, keyframe);
  }
  host.session->AddPeerBatch({2, 3}, live, keyframe);
  for (int tick = 0; tick < 400 && host.session->Plan()[2].plug_from == NEVER; ++tick)
    friends.Play(1);
  ASSERT_NE(host.session->Plan()[2].plug_from, NEVER);
  EXPECT_GE(host.session->Plan()[2].plug_from - friends.mesh.last_sent[0].current_frame,
            7 + 12 + 24 + 4);
  friends.Play(400);
  friends.Compare(keyframe);
}

TEST(OrcaLegacy044, SessionPacketStreamGolden)
{
  FourFriends ordinary(false, 1);
  ordinary.mesh.latency = {0, 3, 1, 2};
  ordinary.Play(400);
  ordinary.Compare();
  u64 hash = 1469598103934665603ULL;
  for (const auto& packet : ordinary.mesh.wire)
  {
    for (const unsigned char byte : packet)
      hash = (hash ^ byte) * 1099511628211ULL;
    hash = (hash ^ '\n') * 1099511628211ULL;
  }
  RecordProperty("wire_hash", std::to_string(hash));
  RecordProperty("wire_count", std::to_string(ordinary.mesh.wire.size()));
  EXPECT_EQ(hash, 9745056752490245998ULL);
}

TEST(OrcaDoubles, RetainedRebuildFinishesItsBufferedInputsBeforeTheNewStay)
{
  FourFriends friends;
  friends.mesh.latency = {0, 1, 2, 0};
  for (auto& player : friends.players)
    player->session->SetFixedDelay(6);
  friends.Play(400);
  auto& host = *friends.players[0];
  const int live = host.session->CurrentFrame();
  const int keyframe = live - 20;
  host.session->DropSeat(3);
  const int unplug = host.session->Plan()[3].unplug_from;
  EXPECT_GT(unplug, live);
  host.session->AddPeer(3, live, keyframe);
  EXPECT_TRUE(host.session->Plugged(3, live));
  EXPECT_TRUE(host.session->Plugged(3, unplug - 1));
  EXPECT_FALSE(host.session->Plugged(3, unplug));
  for (auto& queue : friends.mesh.pending)
    std::erase_if(queue, [](const auto& pending) { return pending.packet.seat == 3; });
  friends.mesh.pending[3].clear();
  friends.players[3] = std::make_unique<MeshPlayer>(friends.mesh, 3, keyframe, true);
  friends.players[3]->game.m_state = At(host.game.m_history, keyframe);
  friends.Play(1000);
  friends.Compare(keyframe);
}

TEST(OrcaDoubles, UnplugPrefixStillNamesItsMissingSeat)
{
  FourFriends friends;
  friends.mesh.latency = {0, 1, 2, 1};
  friends.mesh.pair_latency[3][1] = 8;
  friends.Play(400);
  friends.mesh.blackout[3] = true;
  friends.Play(50);
  friends.mesh.hold_departed[1] = true;
  friends.players[0]->session->DropSeat(3);
  friends.disabled[3] = true;
  friends.Play(50);
  EXPECT_EQ(friends.players[1]->session->MissingInputSeats(), (std::vector<int>{3}));
  EXPECT_TRUE(Rollback::OnlineMatch::WaitForFriendsRoster(*friends.players[1]->session, true, true));
  friends.mesh.hold_departed[1] = false;
  friends.Play(400);
  friends.Compare();
}

TEST(OrcaDoubles, StartGateComparesLatePadsAndRetiredCopiesAfterMasking)
{
  FakeNet net;
  net.latency = 1;
  net.extra = {0, 2};
  FakeNet::End transport(net, 1);
  FakeGame game;
  auto config = DropInConfig(1, 0);
  config.per_peer_inputs = true;
  config.time_sync = false;
  Session session(config, game, transport);
  Packet host;
  host.seat = 0;
  host.sequence = 1;
  host.roster = config.plan;
  (*host.roster)[1] = {20, NEVER, 0};
  host.start_gate = {0, NEVER};
  host.pads = {Pad{PAD_WIRE_START_HIGH}};
  host.departed[1] = {0, 12, 0, std::vector<Pad>(12, Pad{PAD_WIRE_START_HIGH})};
  net.queues[1].push_back({0, host});
  int completed = -1;
  for (int tick = 0; tick < 3; ++tick)
  {
    net.now = tick;
    ASSERT_EQ(session.OnBoundary(completed, Pad{PAD_WIRE_START_HIGH}).kind, StepKind::Run);
    completed = game.Run();
  }
  EXPECT_EQ(At(game.m_ran_pads, 1)[0][0] & PAD_WIRE_START_HIGH, 0);
  EXPECT_EQ(At(game.m_ran_pads, 1)[1][0] & PAD_WIRE_START_HIGH, 0);
  host.sequence = 2;
  host.current_frame = 3;
  host.first_frame = 1;
  net.queues[1].push_back({net.latency + net.extra[1], host});
  net.now = 3;
  EXPECT_EQ(session.OnBoundary(completed, Pad{PAD_WIRE_START_HIGH}).kind, StepKind::Run);
  EXPECT_EQ(session.GetStats().rollbacks, 0);
  EXPECT_TRUE(session.Error().empty());
}

TEST(OrcaDoubles, TwoRetainedRebuildsPreserveBufferedPortHistory)
{
  FourFriends friends;
  friends.mesh.latency = {0, 1, 2, 0};
  for (auto& player : friends.players)
    player->session->SetFixedDelay(6);
  friends.Play(400);
  auto& host = *friends.players[0];
  int keyframe = 0;
  const auto rebuild = [&] {
    const int live = host.session->CurrentFrame();
    keyframe = live - 20;
    host.session->DropSeat(3);
    host.session->AddPeer(3, live, keyframe);
    for (auto& queue : friends.mesh.pending)
      std::erase_if(queue, [](const auto& pending) { return pending.packet.seat == 3; });
    friends.mesh.pending[3].clear();
    friends.players[3] = std::make_unique<MeshPlayer>(friends.mesh, 3, keyframe, true);
    friends.players[3]->game.m_state = At(host.game.m_history, keyframe);
  };
  const int before = host.session->CurrentFrame();
  rebuild();
  for (int tick = 0; tick < 100 && host.session->Plan()[3].plug_from == NEVER; ++tick)
    friends.Play(1);
  ASSERT_NE(host.session->Plan()[3].plug_from, NEVER);
  ASSERT_LT(host.session->CurrentFrame(), before + 6);
  rebuild();
  friends.Play(1000);
  friends.Compare(keyframe);
}

TEST(OrcaDoubles, UnevenRequestedLeaveForwardsTheCompletedPrefix)
{
  FourFriends friends;
  friends.mesh.latency = {0, 1, 3, 1};
  friends.mesh.pair_latency[3][1] = 8;
  friends.Play(400);
  friends.players[3]->session->RequestLeave();
  for (int tick = 0; tick < 200 && !friends.players[3]->session->LeftDone(); ++tick)
    friends.Play(1);
  ASSERT_TRUE(friends.players[3]->session->LeftDone());
  friends.disabled[3] = true;
  friends.Play(400);
  EXPECT_EQ(friends.players[0]->session->Plan()[3].live_from, NEVER);
  friends.Compare();
}

TEST(OrcaDoubles, RebuildingSeatAcknowledgesItsCompletedPrefixAcrossChunks)
{
  for (const int room_friends : {1, 3})
  {
    SCOPED_TRACE(room_friends);
    FourFriends friends(true, room_friends);
    friends.mesh.latency = {0, 1, 2, 0};
    const int recovering = room_friends;
    friends.mesh.pair_latency[recovering][0] = 2;
    for (auto& player : friends.players)
      if (player)
        player->session->SetFixedDelay(6);
    friends.Play(400);
    friends.mesh.reported_rtt[recovering] = 2000;
    auto& host = *friends.players[0];
    int keyframe = 0;
    const auto rebuild = [&] {
      const int live = host.session->CurrentFrame();
      keyframe = live - 20;
      host.session->DropSeat(recovering);
      host.session->AddPeer(recovering, live, keyframe);
      for (auto& queue : friends.mesh.pending)
        std::erase_if(queue, [&](const auto& pending) { return pending.packet.seat == recovering; });
      friends.mesh.pending[recovering].clear();
      friends.players[recovering] = std::make_unique<MeshPlayer>(friends.mesh, recovering, keyframe, true);
      friends.players[recovering]->game.m_state = At(host.game.m_history, keyframe);
    };
    rebuild();
    for (int tick = 0; tick < 100 && host.session->Plan()[recovering].plug_from == NEVER; ++tick)
      friends.Play(1);
    ASSERT_NE(host.session->Plan()[recovering].plug_from, NEVER);
    ASSERT_GT(host.session->Plan()[recovering].plug_from - host.session->CurrentFrame(),
              MAX_PADS_IN_PACKET * 2);
    rebuild();
    friends.Play(1500);
    EXPECT_GT(friends.players[recovering]->session->CurrentFrame(), keyframe + 1000)
        << friends.players[recovering]->session->Describe();
    friends.Compare(keyframe);
  }
}

TEST(OrcaDoubles, HostBoundaryKeepsPlayingAfterStaggeredFriendBlackouts)
{
  FourFriends friends;
  friends.mesh.latency = {0, 1, 1, 1};
  friends.Play(400);
  friends.mesh.blackout[2] = true;
  friends.Play(6);
  friends.mesh.blackout[3] = true;
  friends.Play(50);
  auto& host = *friends.players[0];
  ASSERT_EQ(host.session->MissingInputSeats(), (std::vector<int>{2}));
  friends.disabled[0] = friends.disabled[2] = friends.disabled[3] = true;
  bool dropped_both = false;
  for (int boundary = 0; boundary < 40; ++boundary)
  {
    const int running = Rollback::OnlineMatch::PumpFriendsHostBoundaryForTest(
        host.session, host.completed, {1, 2, 3}, [&] { friends.Play(1); },
        [] { return std::vector<int>{}; });
    ASSERT_TRUE(host.session);
    ASSERT_TRUE(host.session->Error().empty()) << host.session->Error();
    ASSERT_TRUE(running != host.completed || host.session->Resimulating());
    for (int seat = 0; seat < MAX_SEATS; ++seat)
      host.ran_plugged[running][seat] = host.session->Plugged(seat, running);
    host.completed = host.game.Run();
    EXPECT_EQ(host.completed, running);
    dropped_both = host.session->Plan()[2].live_from == NEVER &&
                   host.session->Plan()[3].live_from == NEVER;
    if (dropped_both && !host.session->Resimulating())
      break;
  }
  EXPECT_TRUE(dropped_both);
  const int after_drop = host.completed;
  friends.disabled[0] = false;
  friends.Play(100);
  EXPECT_GT(host.completed, after_drop + 30);
  friends.Compare();
}

TEST(OrcaDoubles, HostBoundaryRoomDepartureDoesNotSpendTheNextFriendsRetry)
{
  FourFriends friends;
  friends.players[2]->unplugged_pad = true;
  friends.Play(400);
  friends.mesh.blackout[3] = true;
  friends.Play(6);
  friends.mesh.blackout[2] = true;
  friends.Play(50);
  auto& host = *friends.players[0];
  ASSERT_EQ(host.session->MissingInputSeats(), (std::vector<int>{3}));
  friends.disabled[0] = friends.disabled[2] = friends.disabled[3] = true;
  bool told_left = false;
  for (int boundary = 0; boundary < 40; ++boundary)
  {
    const int running = Rollback::OnlineMatch::PumpFriendsHostBoundaryForTest(
        host.session, host.completed, {1, 2, 3}, [&] { friends.Play(1); }, [&] {
          return std::exchange(told_left, true) ? std::vector<int>{} : std::vector<int>{2};
        });
    ASSERT_TRUE(host.session);
    ASSERT_TRUE(host.session->Error().empty()) << host.session->Error();
    ASSERT_TRUE(running != host.completed || host.session->Resimulating());
    for (int seat = 0; seat < MAX_SEATS; ++seat)
      host.ran_plugged[running][seat] = host.session->Plugged(seat, running);
    host.completed = host.game.Run();
    EXPECT_EQ(host.completed, running);
    if (host.session->Plan()[3].live_from == NEVER && !host.session->Resimulating())
      break;
  }
  EXPECT_TRUE(told_left);
  EXPECT_EQ(host.session->Plan()[2].live_from, NEVER);
  EXPECT_EQ(host.session->Plan()[3].live_from, NEVER);
  const int compare_from = host.completed + 10;
  friends.disabled[0] = false;
  friends.Play(100);
  friends.Compare(compare_from);
}

TEST(OrcaDoubles, RosterWaitUsesTheSessionsAuthoritySeat)
{
  FakeNet net;
  FakeNet::End transport(net, 1);
  FakeGame game;
  auto config = DropInConfig(1, 0);
  config.per_peer_inputs = true;
  config.authority_seat = 2;
  std::swap((*config.plan)[0], (*config.plan)[2]);
  Session session(config, game, transport);
  ASSERT_EQ(session.MissingInputSeats(), (std::vector<int>{2}));
  EXPECT_FALSE(Rollback::OnlineMatch::WaitForFriendsRoster(session, true, true));
}

TEST(OrcaDoubles, WaitingOverlayAlwaysSelectsAnotherSeat)
{
  using Rollback::OnlineMatch::WaitingFriendSeat;
  EXPECT_EQ(WaitingFriendSeat(1, {1, 3}, true, 0, {}), 3);
  EXPECT_EQ(WaitingFriendSeat(1, {1}, true, 0, {}), 0);
  EXPECT_EQ(WaitingFriendSeat(0, {0}, false, 0, {0, 2}), 2);
  EXPECT_EQ(WaitingFriendSeat(0, {0}, true, 0, {0}), -1);
}

TEST(OrcaDoubles, SuccessiveBatchesKeepTheUnconfirmedStartGate)
{
  FourFriends friends(true, 1);
  friends.mesh.latency = {0, 2, 1, 3};
  friends.players[0]->held_start = friends.players[1]->held_start = true;
  friends.Play(400);
  auto& host = *friends.players[0];
  int compare_from = 300;
  const auto add = [&](int seat) {
    const int live = host.session->CurrentFrame();
    const int keyframe = live - 20;
    compare_from = keyframe;
    friends.players[seat] = std::make_unique<MeshPlayer>(friends.mesh, seat, keyframe, true);
    friends.players[seat]->held_start = true;
    friends.players[seat]->game.m_state = At(host.game.m_history, keyframe);
    host.session->AddPeerBatch({seat}, live, keyframe);
  };
  add(2);
  for (int tick = 0; tick < 100 && host.session->Plan()[2].plug_from == NEVER; ++tick)
    friends.Play(1);
  ASSERT_NE(host.session->Plan()[2].plug_from, NEVER);
  const auto previous = friends.mesh.last_sent[0].start_gate;
  ASSERT_TRUE(previous);
  ASSERT_GT(previous->second, host.session->LogEnd());
  add(3);
  friends.Play(1);
  ASSERT_TRUE(friends.mesh.last_sent[0].start_gate);
  EXPECT_EQ(friends.mesh.last_sent[0].start_gate->first, previous->first);
  friends.Play(1000);
  friends.Compare(compare_from);
}

TEST(OrcaDoubles, TwentyLostHostPacketsAcrossBatchesKeepCharacterSelectStartEqual)
{
  FourFriends friends(true, 1);
  friends.Play(400);
  auto& host = *friends.players[0];
  int first = host.session->CurrentFrame() - 20;
  const auto add = [&](int seat) {
    const int live = host.session->CurrentFrame();
    const int keyframe = live - 20;
    first = std::max(first, keyframe);
    friends.players[seat] = std::make_unique<MeshPlayer>(friends.mesh, seat, keyframe, true);
    friends.players[seat]->game.m_state = At(host.game.m_history, keyframe);
    host.session->AddPeerBatch({seat}, live, keyframe);
  };
  add(2);
  friends.Play(1);
  ASSERT_TRUE(friends.mesh.last_sent[0].start_gate);
  const auto open = *friends.mesh.last_sent[0].start_gate;
  ASSERT_EQ(open.second, NEVER);
  friends.mesh.drop_packets[0][1] = 20;
  host.session->DropSeat(2);
  friends.disabled[2] = true;
  friends.Play(4);
  const auto ended = *friends.mesh.last_sent[0].start_gate;
  ASSERT_NE(ended.second, NEVER);
  ASSERT_LE(ended.second, host.session->LogEnd());
  add(3);
  friends.Play(1);
  ASSERT_TRUE(friends.mesh.last_sent[0].start_gate);
  EXPECT_EQ(friends.mesh.last_sent[0].start_gate->first, open.first);
  friends.Play(400);
  EXPECT_EQ(friends.mesh.drop_packets[0][1], 0);
  const int start = host.session->CurrentFrame();
  for (const int seat : {0, 1, 3})
    friends.players[seat]->held_start = true;
  friends.Play(180);
  friends.Compare(first);
  for (const int seat : {0, 1, 3})
  {
    const auto& player = *friends.players[seat];
    EXPECT_GT(player.session->GetStats().checksums_matched, 0);
    EXPECT_NE(At(player.game.m_ran_pads, start + 60)[0][0] & PAD_WIRE_START_HIGH, 0);
  }
}

TEST(OrcaDoubles, ReceivingAnOpenGateThenAMovedGateDoesNotKeepItForever)
{
  FakeNet net;
  FakeNet::End transport(net, 1);
  FakeGame game;
  auto config = DropInConfig(1, 0);
  config.per_peer_inputs = true;
  config.time_sync = false;
  (*config.plan)[1] = {0, NEVER, 0};
  Session session(config, game, transport);
  int completed = -1;
  for (int frame = 0; frame < 12; ++frame)
  {
    Packet host;
    host.seat = 0;
    host.sequence = frame + 1;
    host.current_frame = frame;
    host.first_frame = frame;
    host.roster = config.plan;
    host.start_gate = frame == 0 ? std::pair{0, NEVER} : std::pair{1, 3};
    host.pads = {Pad{PAD_WIRE_START_HIGH}};
    host.ack[1] = frame;
    net.queues[1].push_back({0, host});
    ASSERT_EQ(session.OnBoundary(completed, Pad{PAD_WIRE_START_HIGH}).kind, StepKind::Run);
    completed = game.Run();
  }
  EXPECT_TRUE(session.Error().empty());
  EXPECT_NE(At(game.m_ran_pads, 8)[0][0] & PAD_WIRE_START_HIGH, 0);
}

TEST(OrcaDoubles, ConfirmedDeparturesStopRepeatingTheirHistory)
{
  FourFriends friends;
  friends.mesh.latency = {0, 1, 3, 2};
  friends.Play(400);
  friends.mesh.blackout[3] = true;
  friends.Play(60);
  friends.players[0]->session->DropSeat(3);
  friends.disabled[3] = true;
  friends.Play(400);
  EXPECT_TRUE(friends.mesh.last_sent[0].departed.empty());
  friends.players[0]->session->SetFixedDelay(4);
  friends.players[0]->session->SetFixedDelay(std::nullopt);
  friends.Play(100);
  EXPECT_TRUE(friends.mesh.last_sent[0].departed.empty());
  friends.Compare();
}

TEST(OrcaDoubles, NewAuthorityGateRetainsThePeersUnconfirmedPreviousGate)
{
  FakeNet net;
  net.latency = 1;
  net.extra = {0, 2};
  FakeNet::End transport(net, 1);
  FakeGame game;
  auto config = DropInConfig(1, 0);
  config.per_peer_inputs = true;
  config.time_sync = false;
  Session session(config, game, transport);
  Packet host;
  host.seat = 0;
  host.sequence = 1;
  host.roster = config.plan;
  (*host.roster)[1] = {20, NEVER, 0};
  host.start_gate = {0, 12};
  host.pads = {Pad{PAD_WIRE_START_HIGH}};
  host.departed[1] = {0, 12, 0, std::vector<Pad>(12, Pad{PAD_WIRE_START_HIGH})};
  net.queues[1].push_back({0, host});
  int completed = -1;
  for (int tick = 0; tick < 3; ++tick)
  {
    net.now = tick;
    ASSERT_EQ(session.OnBoundary(completed, Pad{PAD_WIRE_START_HIGH}).kind, StepKind::Run);
    completed = game.Run();
  }
  EXPECT_EQ(At(game.m_ran_pads, 1)[0][0] & PAD_WIRE_START_HIGH, 0);
  EXPECT_EQ(At(game.m_ran_pads, 1)[1][0] & PAD_WIRE_START_HIGH, 0);
  host.start_gate = {20, NEVER};
  host.sequence = 2;
  host.current_frame = 3;
  host.first_frame = 1;
  net.queues[1].push_back({net.latency + net.extra[1], host});
  net.now = 3;
  EXPECT_EQ(session.OnBoundary(completed, Pad{PAD_WIRE_START_HIGH}).kind, StepKind::Run);
  EXPECT_EQ(session.GetStats().rollbacks, 0);
  EXPECT_TRUE(session.Error().empty());
}

TEST(OrcaDoubles, FasterBatchKeepsThePreviousUnconfirmedGateEnd)
{
  FourFriends friends(true, 1);
  friends.players[0]->held_start = friends.players[1]->held_start = true;
  friends.Play(400);
  auto& host = *friends.players[0];
  int compare_from = 300;
  const auto add = [&](int seat) {
    const int live = host.session->CurrentFrame();
    compare_from = live - 20;
    friends.players[seat] = std::make_unique<MeshPlayer>(friends.mesh, seat, compare_from, true);
    friends.players[seat]->game.m_state = At(host.game.m_history, compare_from);
    friends.players[seat]->held_start = true;
    host.session->AddPeerBatch({seat}, live, compare_from);
  };
  friends.mesh.reported_rtt[2] = 400;
  add(2);
  for (int tick = 0; tick < 100 && host.session->Plan()[2].plug_from == NEVER; ++tick)
    friends.Play(1);
  ASSERT_TRUE(friends.mesh.last_sent[0].start_gate);
  const auto previous = *friends.mesh.last_sent[0].start_gate;
  ASSERT_NE(previous.second, NEVER);
  ASSERT_GT(previous.second, host.session->CurrentFrame() + 23);
  host.session->DropSeat(2);
  friends.disabled[2] = true;
  add(3);
  for (int tick = 0; tick < 100 && host.session->Plan()[3].plug_from == NEVER; ++tick)
    friends.Play(1);
  ASSERT_NE(host.session->Plan()[3].plug_from, NEVER);
  ASSERT_TRUE(friends.mesh.last_sent[0].start_gate);
  EXPECT_EQ(friends.mesh.last_sent[0].start_gate->first, previous.first);
  EXPECT_GE(friends.mesh.last_sent[0].start_gate->second, previous.second);
  friends.Play(800);
  friends.Compare(compare_from);
}

TEST(OrcaCoSeats, CoSeatsOffIsIdentical)
{
  FourFriends ordinary(false, 1);
  FourFriends enabled(true, 1);
  ordinary.Play(400);
  enabled.Play(400);
  EXPECT_EQ(ordinary.mesh.wire, enabled.mesh.wire);
  u64 wire_hash = 0;
  for (const auto& packet : enabled.mesh.wire)
    for (const unsigned char byte : packet)
      wire_hash = Mix(wire_hash, byte);
  u64 frame_hash = 0;
  for (int seat = 0; seat < 2; ++seat)
  {
    EXPECT_EQ(ordinary.players[seat]->game.m_history, enabled.players[seat]->game.m_history);
    EXPECT_EQ(ordinary.players[seat]->game.m_ran_pads, enabled.players[seat]->game.m_ran_pads);
    EXPECT_EQ(ordinary.players[seat]->session->Plan(), enabled.players[seat]->session->Plan());
    for (const auto& [frame, pads] : enabled.players[seat]->game.m_ran_pads)
    {
      frame_hash = Mix(frame_hash, static_cast<u64>(frame));
      for (const Pad& pad : pads)
        for (const u8 byte : pad)
          frame_hash = Mix(frame_hash, byte);
    }
  }
  EXPECT_EQ(wire_hash, 0x4fbbf8356c8b5cdeull);
  EXPECT_EQ(frame_hash, 0x493a6b3462a47136ull);
  ordinary.Compare();
  enabled.Compare();
}

namespace
{
struct CoMeshPlayer : MeshPlayer
{
  CoMeshPlayer(FriendsMesh& network, int local, const std::vector<int>& siblings,
               const std::vector<int>& host_group, int start = -1)
      : MeshPlayer(network, local, start >= 0 ? start : (local == 0 ? 300 : 240), true),
        co_seats(siblings)
  {
    const int first = start >= 0 ? start : (local == 0 ? 300 : 240);
    Config config = DropInConfig(local, first);
    config.per_peer_inputs = true;
    config.co_seats = siblings;
    config.snapshot_every = 1;
    for (const int seat : host_group)
      (*config.plan)[seat] = {0, NEVER, 0};
    config.start_pad = Input(local, first - 1);
    for (const int seat : siblings)
      config.start_pads[seat] = Input(seat, first - 1);
    session = std::make_unique<Session>(config, game, transport);
    session->SetValuesHeld(4);
    for (int seat = 0; seat < MAX_SEATS; ++seat)
      if (session->IsLocal(seat) && session->Plugged(seat, first))
        for (int frame = first; frame < first + session->Delay(); ++frame)
          sample_wall[seat][frame] = first - 1;
  }
  static Pad Input(int seat, int wall)
  {
    return Pad{static_cast<u8>((wall / 7 + seat * 3) % 4),
               static_cast<u8>(wall % 3), 0, 0, static_cast<u8>(seat + 1), 0, 0, 0};
  }
  int TickCo(int wall, int frames = 1)
  {
    int ran = 0;
    for (int guard = 0; guard < 4000 && ran < frames; ++guard)
    {
      Pads pads{};
      for (int seat = 0; seat < MAX_SEATS; ++seat)
      {
        pads[seat] = Input(seat, wall);
        if (held_start)
          pads[seat][0] |= PAD_WIRE_START_HIGH;
        if (neutral_seats[seat])
          pads[seat] = Pad{};
      }
      const bool sampling = !session->Resimulating();
      const int target = session->CurrentFrame() + session->Delay();
      const auto plan = session->Plan();
      const Step step = session->OnBoundary(completed, pads[transport.seat], &pads);
      if (sampling && step.kind != StepKind::Wait && step.kind != StepKind::Ended)
        for (int seat = 0; seat < MAX_SEATS; ++seat)
          if (session->IsLocal(seat) && target >= plan[seat].plug_from &&
              target < plan[seat].unplug_from &&
              (sample_wall[seat].empty() || target > sample_wall[seat].rbegin()->first))
            sample_wall[seat].emplace(target, wall);
      if (step.kind == StepKind::Ended || step.kind == StepKind::Wait || step.kind == StepKind::Stall)
        return ran;
      const bool fresh = step.kind == StepKind::Run && !session->Resimulating();
      for (int seat = 0; seat < MAX_SEATS; ++seat)
        ran_plugged[step.frame][seat] = session->Plugged(seat, step.frame);
      completed = game.Run();
      if (fresh)
        ++ran;
    }
    return ran;
  }
  std::vector<int> co_seats;
  std::array<bool, MAX_SEATS> neutral_seats{};
  std::array<std::map<int, int>, MAX_SEATS> sample_wall{};
};

struct CoSeatMesh
{
  explicit CoSeatMesh(const std::vector<std::vector<int>>& machine_groups) : groups(machine_groups)
  {
    for (const auto& group : groups)
    {
      const int lead = group.front();
      players[lead] = std::make_unique<CoMeshPlayer>(mesh, lead,
          std::vector<int>(group.begin() + 1, group.end()), groups.front());
    }
    std::vector<Pads> log;
    for (int frame = 0; frame < 300; ++frame)
    {
      Pads pads;
      pads.fill(UNPLUGGED_PAD);
      for (const int seat : groups.front())
        pads[seat] = CoMeshPlayer::Input(seat, frame);
      for (int seat = 0; seat < MAX_SEATS; ++seat)
        players[0]->ran_plugged[frame][seat] = !IsUnplugged(pads[seat]);
      players[0]->game.SetPads(frame, pads);
      players[0]->game.Run();
      log.push_back(pads);
    }
    players[0]->session->SetLog(240, {log.begin() + 240, log.end()});
    std::vector<int> arrivals;
    for (size_t machine = 1; machine < groups.size(); ++machine)
    {
      const auto& group = groups[machine];
      players[group.front()]->game.m_state = At(players[0]->game.m_history, 240);
      players[0]->session->SetGroup(group.front(), {group.begin() + 1, group.end()});
      for (const int seat : group)
      {
        arrivals.push_back(seat);
        players[0]->session->RequireValues(seat, 4);
      }
    }
    players[0]->session->AddPeerBatch(arrivals, 300, 240);
  }
  void Play(int ticks)
  {
    for (int count = 0; count < ticks; ++count, ++mesh.tick)
      for (auto& player : players)
        if (player && !disabled[player->transport.seat])
          player->TickCo(mesh.tick, player->session->CatchingUp() ? 40 : 1);
  }
  void Compare(int first = 240)
  {
    int through = NEVER;
    for (const auto& player : players)
    {
      if (!player)
        continue;
      ASSERT_TRUE(player->session->Error().empty()) << player->session->Error() << " "
                                                   << player->session->Describe();
      through = std::min(through, player->session->FinalFrame() - 1);
      EXPECT_LE(player->session->GetStats().deepest_rollback, 7);
      EXPECT_EQ(player->session->PeersInPlay(), static_cast<int>(groups.size()));
      int seats = 0;
      for (const auto& group : groups)
        seats += static_cast<int>(group.size());
      EXPECT_EQ(player->session->PlayersInPlay(), seats);
    }
    ASSERT_GT(through, first + 100) << players[0]->session->Describe();
    for (int frame = first; frame <= through; ++frame)
      for (const auto& player : players)
      {
        if (!player)
          continue;
        ASSERT_EQ(At(players[0]->game.m_history, frame), At(player->game.m_history, frame))
            << "machine " << player->transport.seat << " frame " << frame;
        ASSERT_EQ(At(players[0]->game.m_ran_pads, frame), At(player->game.m_ran_pads, frame))
            << "machine " << player->transport.seat << " frame " << frame;
        ASSERT_EQ(At(players[0]->ran_plugged, frame), At(player->ran_plugged, frame));
      }
    for (const auto& group : groups)
      for (const auto& player : players)
        if (player)
          for (const int seat : group)
          {
            EXPECT_EQ(player->session->LeadOf(seat), group.front());
            EXPECT_EQ(player->session->Plan()[seat].live_from,
                      player->session->Plan()[group.front()].live_from);
            EXPECT_EQ(player->session->Plan()[seat].plug_from,
                      player->session->Plan()[group.front()].plug_from);
          }
    for (const std::string& wire : mesh.wire)
    {
      Packet packet;
      ASSERT_TRUE(DecodePacket(wire, &packet));
      ASSERT_TRUE(players[packet.seat]);
      EXPECT_EQ(packet.co.size(), players[packet.seat]->co_seats.size());
      for (const auto& [seat, pads] : packet.co)
      {
        EXPECT_EQ(pads.size(), packet.pads.size());
        EXPECT_EQ(players[0]->session->LeadOf(seat), packet.seat);
      }
      for (const int seat : groups[0])
        if (packet.seat == 0)
          EXPECT_EQ(packet.ack[seat], -1);
      if (packet.groups)
        EXPECT_EQ(packet.seat, 0);
      if (packet.history_seat >= 0)
        ASSERT_TRUE(players[packet.history_seat]);
    }
  }
  FriendsMesh mesh;
  std::array<std::unique_ptr<CoMeshPlayer>, MAX_SEATS> players{};
  std::array<bool, MAX_SEATS> disabled{};
  std::vector<std::vector<int>> groups;
};

Pad ExpectedCoSample(const CoMeshPlayer& owner, int seat, int frame)
{
  const auto& samples = owner.sample_wall[seat];
  const auto after = samples.upper_bound(frame);
  return after == samples.begin() ? Pad{} : CoMeshPlayer::Input(seat, std::prev(after)->second);
}

const std::vector<std::vector<std::vector<int>>> CO_TOPOLOGIES{
    {{0, 1}, {2}}, {{0, 1}, {2, 3}}, {{0, 1}, {2}, {3}}, {{0}, {1}, {2, 3}},
    {{0, 3}, {1, 2}}};
}

TEST(OrcaCoSeats, FixedGroupsAtEveryLatencyAndWorstPairs)
{
  for (const auto& groups : CO_TOPOLOGIES)
    for (int latency = 0; latency <= 4; ++latency)
    {
      SCOPED_TRACE(fmt::format("host seats {} machines {} latency {}", groups[0].size(),
                              groups.size(), latency));
      CoSeatMesh friends(groups);
      if (latency == 4)
        friends.mesh.latency = {0, 1, 2, 3};
      else
        friends.mesh.latency.fill(latency);
      friends.Play(1800);
      friends.Compare();
    }
}

TEST(OrcaCoSeats, SixPairWorstCaseStillAgrees)
{
  CoSeatMesh friends({{0}, {1}, {2}, {3}});
  friends.mesh.latency = {0, 1, 2, 3};
  friends.Play(1800);
  friends.Compare();
}

TEST(OrcaCoSeats, EveryMachineCanBlackOutAndReturn)
{
  for (const auto& groups : CO_TOPOLOGIES)
    for (const auto& dark : groups)
    {
      SCOPED_TRACE(fmt::format("host seats {} machines {} dark {}", groups[0].size(),
                              groups.size(), dark.front()));
      CoSeatMesh friends(groups);
      friends.mesh.latency = {0, 1, 2, 3};
      friends.Play(400);
      friends.mesh.blackout[dark.front()] = true;
      friends.Play(60);
      std::array<int, MAX_SEATS> stopped{};
      for (const auto& player : friends.players)
        if (player)
          stopped[player->transport.seat] = player->session->CurrentFrame();
      friends.Play(30);
      for (const auto& player : friends.players)
        if (player)
          EXPECT_EQ(player->session->CurrentFrame(), stopped[player->transport.seat]);
      friends.mesh.blackout[dark.front()] = false;
      friends.Play(600);
      for (const auto& player : friends.players)
        if (player)
          EXPECT_GT(player->session->CurrentFrame(), stopped[player->transport.seat] + 400);
      friends.Compare();
    }
}

TEST(OrcaCoSeats, StartGateMasksEveryLocalPadUntilTheAtomicPlug)
{
  CoSeatMesh friends({{0, 1}, {2, 3}});
  for (auto& player : friends.players)
    if (player)
      player->held_start = true;
  friends.mesh.blackout[2] = true;
  friends.Play(80);
  EXPECT_EQ(friends.players[0]->session->Plan()[2].plug_from, NEVER);
  for (int frame = 302; frame < friends.players[0]->completed; ++frame)
    for (const int seat : {0, 1})
      EXPECT_EQ(At(friends.players[0]->game.m_ran_pads, frame)[seat][0] & PAD_WIRE_START_HIGH, 0);
  friends.mesh.blackout[2] = false;
  friends.Play(600);
  const int plug = friends.players[0]->session->Plan()[2].plug_from;
  ASSERT_NE(plug, NEVER);
  EXPECT_EQ(plug, friends.players[0]->session->Plan()[3].plug_from);
  for (const int seat : {0, 1, 2, 3})
    EXPECT_NE(At(friends.players[0]->game.m_ran_pads, plug + 30)[seat][0] & PAD_WIRE_START_HIGH, 0);
  friends.Compare();
}

TEST(OrcaCoSeats, AnotherGroupsInputsEndForPeer)
{
  CoSeatMesh friends({{0, 1}, {2}, {3}});
  friends.Play(400);
  friends.Compare();
  Packet packet = friends.mesh.last_sent[2];
  packet.co = {{3, packet.pads}};
  friends.mesh.pending[0].clear();
  friends.mesh.pending[0].push_back({friends.mesh.tick, packet});
  friends.players[0]->TickCo(friends.mesh.tick);
  EXPECT_TRUE(friends.players[0]->session->ErrorFromPeer());
}

TEST(OrcaCoSeats, RosterGroupsMustComeFromTheHostAndMatchItsPlans)
{
  for (int invalid = 0; invalid < 5; ++invalid)
  {
    CoSeatMesh friends({{0, 1}, {2, 3}});
    friends.Play(400);
    friends.Compare();
    const int sender = invalid == 0 ? 2 : 0;
    const int recipient = sender == 0 ? 2 : 0;
    Packet packet = friends.mesh.last_sent[sender];
    packet.sequence += 100;
    packet.roster = friends.players[0]->session->Plan();
    packet.groups = std::array<int, MAX_SEATS>{0, 0, 2, 2};
    if (invalid == 1)
      ++(*packet.roster)[3].live_from;
    if (invalid == 2)
      ++(*packet.roster)[3].plug_from;
    if (invalid == 3)
      (*packet.groups)[1] = 2;
    if (invalid == 4)
      (*packet.groups)[3] = -1;
    friends.mesh.pending[recipient].clear();
    friends.mesh.pending[recipient].push_back({friends.mesh.tick, packet});
    friends.players[recipient]->TickCo(friends.mesh.tick);
    EXPECT_TRUE(friends.players[recipient]->session->ErrorFromPeer()) << invalid;
  }
}

TEST(OrcaCoSeats, DelayChangesKeepEachSeatsHeldInputAndRange)
{
  CoSeatMesh friends({{0, 1}, {2, 3}});
  friends.Play(400);
  std::array<int, MAX_SEATS> before_raise{-1, -1, -1, -1};
  for (const auto& player : friends.players)
    if (player)
      for (const int seat : player->co_seats)
      {
        ASSERT_FALSE(player->sample_wall[seat].empty());
        before_raise[seat] = player->sample_wall[seat].rbegin()->first;
      }
  for (auto& player : friends.players)
    if (player)
      player->session->SetFixedDelay(6);
  friends.Play(80);
  for (auto& player : friends.players)
    if (player)
      player->session->SetFixedDelay(1);
  friends.Play(400);
  friends.Compare();
  for (const auto& player : friends.players)
    if (player)
    {
      const auto& pads = At(player->game.m_ran_pads, 300);
      if (player->transport.seat == 0)
      {
        EXPECT_EQ(pads[0], CoMeshPlayer::Input(0, 299));
        EXPECT_EQ(pads[1], CoMeshPlayer::Input(1, 299));
      }
    }
  for (const auto& player : friends.players)
    if (player)
      for (const int seat : player->co_seats)
      {
        const auto& samples = player->sample_wall[seat];
        const auto raised = samples.upper_bound(before_raise[seat]);
        ASSERT_NE(raised, samples.end());
        EXPECT_GT(raised->first, before_raise[seat] + 1);
        for (int frame = before_raise[seat] + 1; frame < raised->first; ++frame)
          EXPECT_EQ(At(player->game.m_ran_pads, frame)[seat],
                    CoMeshPlayer::Input(seat, samples.at(before_raise[seat]))) << frame;
        for (const auto& [frame, pads] : player->game.m_ran_pads)
          if (frame >= samples.begin()->first && !IsUnplugged(pads[seat]))
            EXPECT_EQ(pads[seat], ExpectedCoSample(*player, seat, frame)) << seat << " " << frame;
      }
  for (const auto& wire : friends.mesh.wire)
  {
    Packet packet;
    ASSERT_TRUE(DecodePacket(wire, &packet));
    const auto& owner = *friends.players[packet.seat];
    for (const auto& [seat, inputs] : packet.co)
      for (size_t index = 0; index < inputs.size(); ++index)
      {
        const int frame = packet.first_frame + static_cast<int>(index);
        if (owner.session->Plugged(seat, frame))
          EXPECT_EQ(inputs[index], ExpectedCoSample(owner, seat, frame)) << seat << " " << frame;
      }
  }
}

TEST(OrcaCoSeats, InputAcknowledgementUsesTheSlowestLocalSeat)
{
  CoSeatMesh friends({{0, 1}, {2}});
  for (int tick = 0; tick < 400; ++tick)
  {
    const int through = std::max(299, friends.players[0]->session->CurrentFrame() - 30);
    for (auto& pending : friends.mesh.pending[0])
      pending.packet.ack[1] = std::min(pending.packet.ack[1], through);
    friends.Play(1);
  }
  const int frame = friends.players[0]->session->CurrentFrame();
  Packet packet = friends.mesh.last_sent[2];
  packet.sequence = 0;
  packet.ack[0] = frame + 1;
  packet.ack[1] = frame - 30;
  friends.mesh.pending[0].clear();
  friends.mesh.pending[0].push_back({friends.mesh.tick, packet});
  friends.players[0]->TickCo(friends.mesh.tick);
  EXPECT_TRUE(friends.players[0]->session->Error().empty());
  EXPECT_EQ(friends.mesh.last_sent[0].first_frame, frame - 29);
  friends.Play(400);
  friends.Compare();
}

TEST(OrcaCoSeats, DuplicateAndStaleCopiesKeepPadsAndMachineCounts)
{
  CoSeatMesh control({{0, 1}, {2, 3}});
  CoSeatMesh repeated({{0, 1}, {2, 3}});
  control.Play(1000);
  repeated.Play(1000);
  std::array<std::set<int>, MAX_SEATS> copied{};
  for (int tick = 0; tick < 900; ++tick)
  {
    for (auto* simulation : {&control, &repeated})
    {
      if (tick == 100)
      {
        simulation->players[0]->session->OnLocalHitch();
        simulation->players[2]->session->OnLocalHitch();
        simulation->players[2]->session->OnLocalHitch();
      }
      if (tick == 400 || tick == 418)
        simulation->disabled[2] = tick == 400;
      for (auto& player : simulation->players)
        if (player)
          player->game.now_ms = simulation->mesh.tick * (1000.0 / 60);
    }
    for (int seat : {0, 2})
    {
      auto& pending = repeated.mesh.pending[seat];
      const auto copies = pending;
      for (auto it = copies.rbegin(); it != copies.rend(); ++it)
        if (copied[seat].insert(it->packet.sequence).second)
          pending.push_back({it->at + 2, it->packet});
    }
    control.Play(1);
    repeated.Play(1);
  }
  control.Compare();
  repeated.Compare();
  for (int seat : {0, 2})
  {
    EXPECT_EQ(control.players[seat]->game.m_ran_pads, repeated.players[seat]->game.m_ran_pads);
    EXPECT_EQ(control.players[seat]->session->GetStats().checksums_matched,
              repeated.players[seat]->session->GetStats().checksums_matched);
    EXPECT_EQ(control.players[seat]->session->GetStats().peer_hitches,
              repeated.players[seat]->session->GetStats().peer_hitches);
    EXPECT_GT(control.players[seat]->session->GetStats().peer_hitches, 0);
    EXPECT_GT(control.players[seat]->session->GetStats().hitches, 0);
    EXPECT_GT(control.players[seat]->session->GetStats().checksums_matched, 5);
    EXPECT_EQ(control.players[seat]->session->GetStats().counted_stalls,
              repeated.players[seat]->session->GetStats().counted_stalls);
    EXPECT_EQ(control.players[seat]->session->PeersInPlay(), 2);
    EXPECT_EQ(control.players[seat]->session->PlayersInPlay(), 4);
  }
  EXPECT_GT(control.players[0]->session->GetStats().counted_stalls, 0);
  EXPECT_GT(control.mesh.last_sent[0].stalls, 0);
  EXPECT_EQ(control.mesh.last_sent[0].stalls, repeated.mesh.last_sent[0].stalls);
}

TEST(OrcaCoSeats, EstablishedGroupsStayFixedEvenWhenPlansMatch)
{
  CoSeatMesh friends({{0}, {1}, {2}, {3}});
  friends.Play(400);
  friends.Compare();
  Packet packet = friends.mesh.last_sent[0];
  packet.sequence += 100;
  packet.groups = std::array<int, MAX_SEATS>{0, 1, 2, 2};
  friends.mesh.pending[1].clear();
  friends.mesh.pending[1].push_back({friends.mesh.tick, packet});
  friends.players[1]->TickCo(friends.mesh.tick);
  EXPECT_TRUE(friends.players[1]->session->ErrorFromPeer());
}

namespace
{
std::array<u64, 2> SingleSeatGolden(int scenario)
{
  FourFriends friends;
  friends.mesh.latency = {0, 1, 2, 3};
  if (scenario == 2)
  {
    for (int seat = 0; seat < MAX_SEATS; ++seat)
    {
      auto& player = *friends.players[seat];
      Config config = DropInConfig(seat, seat == 0 ? 300 : 240);
      config.per_peer_inputs = true;
      config.time_sync = true;
      player.session = std::make_unique<Session>(config, player.game, player.transport);
      player.session->SetValuesHeld(4);
      for (int sample = 0; sample < 3; ++sample)
        player.session->OnRoundTrip(40 + 60 * seat);
    }
    std::vector<Pads> log;
    for (int frame = 240; frame < 300; ++frame)
      log.push_back(At(friends.players[0]->game.m_ran_pads, frame));
    friends.players[0]->session->SetLog(240, std::move(log));
    for (int seat = 1; seat < MAX_SEATS; ++seat)
      friends.players[0]->session->RequireValues(seat, 4);
    friends.players[0]->session->AddPeerBatch({1, 2, 3}, 300, 240);
  }
  int compare_from = 300;
  for (int tick = 0; tick < 1800; ++tick)
  {
    if (scenario == 1 && tick == 400)
    {
      auto& host = *friends.players[0];
      const int live = host.session->CurrentFrame();
      compare_from = live - 30;
      host.session->DropSeat(3);
      host.session->AddPeer(3, live, compare_from);
      for (auto& queue : friends.mesh.pending)
        std::erase_if(queue, [](const auto& pending) { return pending.packet.seat == 3; });
      friends.players[3] = std::make_unique<MeshPlayer>(friends.mesh, 3, compare_from, true);
      friends.players[3]->game.m_state = At(host.game.m_history, compare_from);
      friends.mesh.pending[3].clear();
    }
    for (auto& player : friends.players)
      player->game.now_ms = tick * (1000.0 / 60);
    friends.Play(1);
    if (scenario == 2 && tick > 200 && tick % 3 == 0)
      friends.players[3]->Tick(friends.mesh.tick);
  }
  friends.Compare(compare_from);
  if (scenario == 2)
  {
    int waits = 0;
    for (const auto& player : friends.players)
    {
      waits += player->session->GetStats().waits;
      EXPECT_GE(player->session->GetStats().peer_rtt_median, 160);
    }
    EXPECT_GT(waits, 0);
  }
  std::array<u64, 2> hashes{};
  for (const auto& wire : friends.mesh.wire)
    for (const unsigned char byte : wire)
      hashes[0] = Mix(hashes[0], byte);
  for (const auto& player : friends.players)
    for (const auto& [frame, pads] : player->game.m_ran_pads)
    {
      hashes[1] = Mix(hashes[1], static_cast<u64>(frame));
      for (const Pad& pad : pads)
        for (const u8 byte : pad)
          hashes[1] = Mix(hashes[1], byte);
    }
  return hashes;
}
}

TEST(OrcaCoSeats, WiderSingleSeatGoldens)
{
  const std::array<std::array<u64, 2>, 3> expected{{
      {0xb1ce5b70c91e2f59ull, 0x4fbd61ac9c2f09a5ull},
      {0xad6cfe7cd352759aull, 0x7bbf7c03ae2261b0ull},
      {0xcdaf4149adc7bebdull, 0x8020f66db1955da3ull}}};
  for (int scenario = 0; scenario < 3; ++scenario)
  {
    SCOPED_TRACE(scenario);
    EXPECT_EQ(SingleSeatGolden(scenario), expected[scenario]);
  }
}

TEST(OrcaCoSeats, DeparturesReleaseGroupOwnershipAndPresence)
{
  for (const bool whole_group : {false, true})
  {
    SCOPED_TRACE(whole_group);
    CoSeatMesh friends({{0}, {1}, {2, 3}});
    friends.Play(400);
    auto& host = *friends.players[0];
    host.session->DropSeat(whole_group ? 2 : 3);
    EXPECT_EQ(host.session->LeadOf(3), 3);
    if (whole_group)
      friends.players[2].reset();
    friends.Play(40);
    EXPECT_TRUE(host.session->Error().empty()) << host.session->Error();
    ASSERT_TRUE(friends.players[1]->session->Error().empty())
        << friends.players[1]->session->Error();
    EXPECT_EQ(friends.players[1]->session->LeadOf(3), 3);
    if (friends.mesh.last_sent[0].groups)
      EXPECT_EQ((*friends.mesh.last_sent[0].groups)[3], -1);
    if (whole_group)
    {
      const int live = host.session->CurrentFrame();
      host.session->AddPeerBatch({2}, live, live - 20);
      EXPECT_EQ(host.session->Plan()[3].live_from, NEVER);
      host.session->DropSeat(2);
      host.session->SetGroup(2, {3});
      EXPECT_TRUE(host.session->Error().empty()) << host.session->Error();
    }
  }
  CoSeatMesh singles({{0}, {1}, {2}, {3}});
  singles.Play(400);
  singles.players[0]->session->DropSeat(3);
  singles.players[0]->session->SetGroup(2, {3});
  EXPECT_TRUE(singles.players[0]->session->Error().empty());
}

TEST(OrcaCoSeats, GroupedLeadRequiresBatchAdmission)
{
  FriendsMesh mesh;
  CoMeshPlayer host(mesh, 0, {}, {0});
  host.session->SetGroup(2, {3});
  host.session->AddPeer(2, 300, 240);
  EXPECT_FALSE(host.session->Error().empty());
  EXPECT_EQ(host.session->Plan()[2].live_from, NEVER);
  EXPECT_EQ(host.session->Plan()[3].live_from, NEVER);
}

TEST(OrcaCoSeats, BatchReplacementKeepsTheDeclaredGroup)
{
  CoSeatMesh friends({{0, 1}, {2, 3}});
  friends.Play(400);
  auto& host = *friends.players[0];
  const int live = host.session->CurrentFrame();
  host.session->AddPeerBatch({2}, live, live - 20);
  EXPECT_TRUE(host.session->Error().empty());
  EXPECT_EQ(host.session->LeadOf(3), 2);
  EXPECT_EQ(host.session->Plan()[2].live_from, live);
  EXPECT_EQ(host.session->Plan()[3].live_from, live);
  EXPECT_EQ(host.session->Plan()[2].plug_from, NEVER);
  EXPECT_EQ(host.session->Plan()[3].plug_from, NEVER);
}

TEST(OrcaCoSeats, JoinerRejectsAdmissionWithoutItsCoSeat)
{
  FriendsMesh mesh;
  CoMeshPlayer joiner(mesh, 2, {3}, {0});
  Packet host;
  host.seat = 0;
  host.sequence = 1;
  host.current_frame = 300;
  std::array<SeatPlan, MAX_SEATS> roster{};
  roster[0] = {0, NEVER, 0};
  roster[2] = {NEVER, NEVER, 300};
  host.roster = roster;
  mesh.pending[2].push_back({0, host});
  joiner.TickCo(0);
  EXPECT_TRUE(joiner.session->ErrorFromPeer());
  EXPECT_NE(joiner.session->Error().find("without its co-seat"), std::string::npos);
}

namespace
{
void CompareCoFrames(const CoSeatMesh& friends, int first = 240)
{
  int through = NEVER;
  for (int lead = 0; lead < MAX_SEATS; ++lead)
    if (friends.players[lead] && !friends.disabled[lead])
    {
      const auto& player = *friends.players[lead];
      ASSERT_TRUE(player.session->Error().empty()) << player.session->Error() << " "
                                                  << player.session->Describe();
      EXPECT_LE(player.session->GetStats().deepest_rollback, 7);
      through = std::min(through, player.session->FinalFrame() - 1);
    }
  ASSERT_GT(through, first + 100);
  for (int frame = first; frame <= through; ++frame)
    for (int lead = 1; lead < MAX_SEATS; ++lead)
      if (friends.players[lead] && !friends.disabled[lead])
      {
        ASSERT_EQ(At(friends.players[0]->game.m_history, frame),
                  At(friends.players[lead]->game.m_history, frame)) << lead << " " << frame;
        ASSERT_EQ(At(friends.players[0]->game.m_ran_pads, frame),
                  At(friends.players[lead]->game.m_ran_pads, frame)) << lead << " " << frame;
        ASSERT_EQ(At(friends.players[0]->ran_plugged, frame),
                  At(friends.players[lead]->ran_plugged, frame)) << lead << " " << frame;
      }
}

void CheckCoCounts(const CoSeatMesh& friends, int seats, int machines)
{
  for (int lead = 0; lead < MAX_SEATS; ++lead)
    if (friends.players[lead] && !friends.disabled[lead])
    {
      EXPECT_EQ(friends.players[lead]->session->PlayersInPlay(), seats) << lead;
      EXPECT_EQ(friends.players[lead]->session->PeersInPlay(), machines) << lead;
    }
}

int RebuildCoMachine(CoSeatMesh& friends, int lead, const std::vector<int>& siblings)
{
  auto& host = *friends.players[0];
  const int live = host.session->CurrentFrame();
  const int keyframe = live - 30;
  host.session->DropSeat(lead);
  host.session->SetGroup(lead, siblings);
  std::vector<int> batch{lead};
  batch.insert(batch.end(), siblings.begin(), siblings.end());
  for (const int seat : batch)
    host.session->RequireValues(seat, 4);
  host.session->AddPeerBatch(batch, live, keyframe);
  for (auto& queue : friends.mesh.pending)
    std::erase_if(queue, [lead](const auto& pending) { return pending.packet.seat == lead; });
  friends.mesh.pending[lead].clear();
  std::vector<int> host_group;
  for (int seat = 0; seat < MAX_SEATS; ++seat)
    if (host.session->IsLocal(seat) && host.session->Plan()[seat].live_from != NEVER)
      host_group.push_back(seat);
  friends.players[lead] = std::make_unique<CoMeshPlayer>(friends.mesh, lead, siblings,
                                                       host_group, keyframe);
  friends.players[lead]->game.m_state = At(host.game.m_history, keyframe);
  friends.disabled[lead] = false;
  return keyframe;
}
}

TEST(OrcaCoSeats, PartialLeaveIsNeutralThenHostScheduledForEveryMachine)
{
  for (const bool host_co : {false, true})
  {
    SCOPED_TRACE(host_co);
    CoSeatMesh friends(host_co ? std::vector<std::vector<int>>{{0, 1}, {2}, {3}} :
                                std::vector<std::vector<int>>{{0}, {1, 2}, {3}});
    friends.mesh.latency = {0, 1, 2, 3};
    friends.Play(500);
    const int removed = host_co ? 1 : 2;
    auto& owner = *friends.players[host_co ? 0 : 1];
    auto& host = *friends.players[0];
    const int neutral_from = owner.session->CurrentFrame() + owner.session->Delay();
    owner.neutral_seats[removed] = true;
    friends.Play(2);
    const int scheduled_at = host.session->CurrentFrame();
    host.session->UnplugSeat(removed);
    const int unplug = host.session->Plan()[removed].unplug_from;
    ASSERT_NE(unplug, NEVER);
    EXPECT_EQ(unplug, scheduled_at + 7 + 2 * 6 + 4);
    friends.Play(2);
    host.session->UnplugSeat(removed);
    EXPECT_EQ(host.session->Plan()[removed].unplug_from, unplug);
    friends.Play(900);
    CompareCoFrames(friends);
    CheckCoCounts(friends, 3, 3);
    EXPECT_EQ(host.session->Plan()[removed].live_from, NEVER);
    for (const auto& player : friends.players)
      if (player)
      {
        for (int frame = neutral_from; frame < unplug; ++frame)
          EXPECT_EQ(At(player->game.m_ran_pads, frame)[removed], Pad{}) << frame;
        EXPECT_TRUE(IsUnplugged(At(player->game.m_ran_pads, unplug)[removed]));
        EXPECT_EQ(player->session->Plan()[removed].unplug_from, unplug);
      }
  }
}

TEST(OrcaCoSeats, GroupLeaveRequestSchedulesBothSeatsAndCompletes)
{
  CoSeatMesh friends({{0, 1}, {2, 3}});
  friends.mesh.latency = {0, 0, 2, 0};
  friends.Play(500);
  friends.players[2]->session->RequestLeave();
  for (int tick = 0; tick < 40 && friends.players[0]->session->Plan()[2].unplug_from == NEVER; ++tick)
    friends.Play(1);
  const int unplug = friends.players[0]->session->Plan()[2].unplug_from;
  ASSERT_NE(unplug, NEVER);
  EXPECT_EQ(unplug, friends.players[0]->session->Plan()[3].unplug_from);
  for (int tick = 0; tick < 400 && !friends.players[2]->session->LeftDone(); ++tick)
    friends.Play(1);
  EXPECT_TRUE(friends.players[2]->session->LeftDone());
  CompareCoFrames(friends);
  friends.disabled[2] = true;
  const int before = friends.players[0]->session->CurrentFrame();
  friends.Play(400);
  EXPECT_GT(friends.players[0]->session->CurrentFrame(), before + 350);
  CheckCoCounts(friends, 2, 1);
  EXPECT_EQ(friends.players[0]->session->Plan()[2].live_from, NEVER);
  EXPECT_EQ(friends.players[0]->session->Plan()[3].live_from, NEVER);
}

TEST(OrcaCoSeats, GroupRoomDropRetiresEverySeatAndSurvivorsAgree)
{
  CoSeatMesh friends({{0}, {1}, {2, 3}});
  friends.mesh.latency = {0, 1, 2, 0};
  friends.Play(500);
  friends.mesh.blackout[2] = true;
  friends.Play(50);
  const int before = friends.players[0]->session->CurrentFrame();
  friends.players[0]->session->DropSeat(2);
  friends.disabled[2] = true;
  friends.Play(600);
  CompareCoFrames(friends);
  CheckCoCounts(friends, 2, 2);
  EXPECT_GT(friends.players[0]->session->CurrentFrame(), before + 500);
  for (const int seat : {2, 3})
    EXPECT_EQ(friends.players[0]->session->Plan()[seat].live_from, NEVER);
}

TEST(OrcaCoSeats, UnevenGroupBlackoutKeepsEverySurvivorHash)
{
  for (const bool host_hears_first : {false, true})
  {
    SCOPED_TRACE(host_hears_first);
    CoSeatMesh friends({{0}, {1}, {2, 3}});
    friends.mesh.latency = {0, 1, 1, 0};
    friends.mesh.pair_latency[2][host_hears_first ? 1 : 0] = 8;
    friends.Play(500);
    friends.mesh.blackout[2] = true;
    friends.Play(50);
    EXPECT_NE(friends.mesh.last_sent[0].ack[2], friends.mesh.last_sent[1].ack[2]);
    EXPECT_NE(friends.mesh.last_sent[0].ack[3], friends.mesh.last_sent[1].ack[3]);
    const int before = friends.players[0]->session->CurrentFrame();
    friends.players[0]->session->DropSeat(2);
    friends.disabled[2] = true;
    friends.Play(600);
    EXPECT_GT(friends.players[1]->session->CurrentFrame(), before + 500);
    CompareCoFrames(friends);
    CheckCoCounts(friends, 2, 2);
  }
}

TEST(OrcaCoSeats, SlotReuseAcrossGroupShapesPreservesEveryFrame)
{
  CoSeatMesh friends({{0}, {1}, {2, 3}});
  friends.mesh.latency = {0, 1, 2, 3};
  friends.Play(500);
  int first = RebuildCoMachine(friends, 2, {});
  friends.Play(600);
  CompareCoFrames(friends, first);
  CheckCoCounts(friends, 3, 3);
  EXPECT_EQ(friends.players[0]->session->Plan()[3].live_from, NEVER);
  first = RebuildCoMachine(friends, 2, {3});
  friends.Play(900);
  CompareCoFrames(friends, first);
  CheckCoCounts(friends, 4, 3);
}

TEST(OrcaCoSeats, RemovingAnAdmittedCoSeatBeforePlugDoesNotKeepItsMachineCatchingUp)
{
  CoSeatMesh friends({{0}, {1, 2}, {3}});
  friends.disabled[3] = true;
  friends.Play(100);
  ASSERT_EQ(friends.players[0]->session->Plan()[1].plug_from, NEVER);
  friends.players[0]->session->UnplugSeat(2);
  friends.disabled[3] = false;
  friends.Play(700);
  EXPECT_NE(friends.players[0]->session->Plan()[1].plug_from, NEVER);
  EXPECT_EQ(friends.players[0]->session->Plan()[2].live_from, NEVER);
  EXPECT_FALSE(friends.players[1]->session->CatchingUp());
  CompareCoFrames(friends);
  CheckCoCounts(friends, 3, 3);
}

TEST(OrcaCoSeats, RebuildAfterPartialLeaveReleasesPrefixAfterGroupAcknowledges)
{
  CoSeatMesh friends({{0}, {1, 2}, {3}});
  friends.mesh.latency = {0, 1, 0, 2};
  friends.Play(600);
  friends.players[1]->neutral_seats[2] = true;
  friends.players[0]->session->UnplugSeat(2);
  const int unplug = friends.players[0]->session->Plan()[2].unplug_from;
  const int first = RebuildCoMachine(friends, 3, {});
  for (int tick = 0; tick < 100; ++tick)
  {
    for (auto& pending : friends.mesh.pending[0])
      if (pending.packet.seat == 1)
        pending.packet.ack[2] = -1;
    friends.Play(1);
  }
  EXPECT_TRUE(friends.mesh.last_sent[0].departed.contains(2));
  EXPECT_GE(friends.mesh.last_sent[1].ack[2], unplug - 1);
  const size_t wire_from = friends.mesh.wire.size();
  const int before = friends.players[0]->session->CurrentFrame();
  friends.Play(3600);
  CompareCoFrames(friends, first);
  CheckCoCounts(friends, 3, 3);
  EXPECT_GT(friends.players[0]->session->CurrentFrame(), before + 3400);
  EXPECT_TRUE(friends.mesh.last_sent[0].departed.empty());
  int empty_prefix_packets = 0;
  for (size_t index = wire_from; index < friends.mesh.wire.size(); ++index)
  {
    const auto& wire = friends.mesh.wire[index];
    EXPECT_LT(wire.size(), 16384u);
    Packet packet;
    ASSERT_TRUE(DecodePacket(wire, &packet));
    for (const auto& [seat, prefix] : packet.departed)
      EXPECT_LE(prefix.pads.size(), static_cast<size_t>(MAX_PADS_IN_PACKET));
    if (packet.seat == 0 && packet.departed.empty())
      ++empty_prefix_packets;
  }
  EXPECT_GT(empty_prefix_packets, 3000);
}

TEST(OrcaCoSeats, RepeatedGroupRebuildsPreserveCompletedLocalPrefixes)
{
  for (const bool three_machines : {false, true})
  {
    CoSeatMesh friends(three_machines ? std::vector<std::vector<int>>{{0}, {1}, {2, 3}} :
                                       std::vector<std::vector<int>>{{0, 1}, {2, 3}});
    friends.mesh.latency = {0, 1, 2, 0};
    friends.Play(500);
    friends.mesh.reported_rtt[2] = 2000;
    int first = RebuildCoMachine(friends, 2, {3});
    for (int tick = 0; tick < 200 && friends.players[0]->session->Plan()[2].plug_from == NEVER; ++tick)
      friends.Play(1);
    ASSERT_NE(friends.players[0]->session->Plan()[2].plug_from, NEVER);
    ASSERT_GT(friends.players[0]->session->Plan()[2].plug_from -
                  friends.players[0]->session->CurrentFrame(), MAX_PADS_IN_PACKET * 2);
    first = RebuildCoMachine(friends, 2, {3});
    friends.Play(1800);
    CompareCoFrames(friends, first);
    CheckCoCounts(friends, 4, three_machines ? 3 : 2);
    EXPECT_TRUE(friends.mesh.last_sent[0].departed.empty());
  }
}

namespace
{
void PlayCoTimed(CoSeatMesh& friends, int ticks)
{
  for (int tick = 0; tick < ticks; ++tick)
  {
    for (auto& player : friends.players)
      if (player)
        player->game.now_ms = friends.mesh.tick * (1000.0 / 60);
    friends.Play(1);
  }
}
}

TEST(OrcaCoSeats, GroupMachineIsSlowestInThePairLatencyMatrix)
{
  for (const auto& groups : CO_TOPOLOGIES)
  {
    CoSeatMesh friends(groups);
    int slow = 0;
    for (const auto& group : groups)
      if (group.size() > 1)
        slow = group.front();
    SCOPED_TRACE(slow);
    for (const auto& from : groups)
      for (const auto& to : groups)
        if (from.front() != to.front())
          friends.mesh.pair_latency[from.front()][to.front()] =
              from.front() == slow || to.front() == slow ? 8 + (from.front() == slow ? 1 : 0) : 1;
    PlayCoTimed(friends, 1800);
    friends.Compare();
    int predictions = 0;
    for (const auto& player : friends.players)
      if (player)
        predictions += player->session->GetStats().rollbacks + player->session->GetStats().stalls;
    EXPECT_GT(predictions, 0);
  }
}

TEST(OrcaCoSeats, RttAndHitchesComeFromEachMachineLead)
{
  for (const bool three_machines : {false, true})
  {
    CoSeatMesh friends(three_machines ? std::vector<std::vector<int>>{{0}, {1, 2}, {3}} :
                                       std::vector<std::vector<int>>{{0, 1}, {2, 3}});
    const int grouped = three_machines ? 1 : 2;
    std::array<int, MAX_SEATS> measured{};
    for (auto& player : friends.players)
      if (player)
      {
        const int lead = player->transport.seat;
        measured[lead] = lead == grouped ? 420 : 40 + 20 * lead;
        friends.mesh.reported_rtt[lead] = measured[lead];
        for (int sample = 0; sample < 3; ++sample)
          player->session->OnRoundTrip(measured[lead]);
      }
    PlayCoTimed(friends, 1200);
    for (auto& player : friends.players)
      if (player)
        for (int hitch = 0; hitch <= player->transport.seat; ++hitch)
          player->session->OnLocalHitch();
    PlayCoTimed(friends, 80);
    friends.Compare();
    CheckCoCounts(friends, 4, three_machines ? 3 : 2);
    for (const auto& player : friends.players)
      if (player)
      {
        int peer_rtt = 0;
        int peer_hitches = 0;
        for (const auto& other : friends.players)
          if (other && other != player)
          {
            peer_rtt = std::max(peer_rtt, measured[other->transport.seat]);
            peer_hitches = std::max(peer_hitches, other->transport.seat + 1);
          }
        EXPECT_EQ(player->session->GetStats().rtt_median, measured[player->transport.seat]);
        EXPECT_EQ(player->session->GetStats().peer_rtt_median, peer_rtt);
        EXPECT_EQ(player->session->GetStats().peer_hitches, peer_hitches);
        EXPECT_EQ(player->session->GetStats().hitches, player->transport.seat + 1);
        EXPECT_GT(player->session->GetStats().checksums_matched, 5);
      }
    EXPECT_EQ(friends.players[0]->session->Delay(), 6);
    Packet stale = friends.mesh.last_sent[grouped];
    --stale.sequence;
    stale.rtt = 9999;
    friends.mesh.pending[0].clear();
    friends.mesh.pending[0].push_back({friends.mesh.tick, stale});
    for (int attempt = 0; attempt < 4 && !friends.mesh.pending[0].empty(); ++attempt)
      friends.players[0]->TickCo(friends.mesh.tick);
    EXPECT_TRUE(friends.mesh.pending[0].empty());
    EXPECT_TRUE(friends.players[0]->session->Error().empty());
    EXPECT_EQ(friends.players[0]->session->GetStats().peer_rtt_median, 420);
  }
}

TEST(OrcaCoSeats, CoSeatOwnershipIsCheckedAtHostsAndJoiners)
{
  for (int invalid = 0; invalid < 6; ++invalid)
  {
    SCOPED_TRACE(invalid);
    CoSeatMesh friends(invalid == 5 ? std::vector<std::vector<int>>{{0, 1}, {2, 3}} :
                                     std::vector<std::vector<int>>{{0}, {1, 2}, {3}});
    friends.Play(400);
    const int sender = invalid <= 2 ? 3 : (invalid == 3 ? 1 : 0);
    const int receiver = invalid <= 1 ? 0 : (invalid == 2 ? 1 : (invalid == 5 ? 2 : 3));
    const int wrong = invalid == 1 ? 0 : (invalid == 3 || invalid == 5 ? 3 : 2);
    Packet packet = friends.mesh.last_sent[sender];
    packet.sequence += 100;
    packet.co = {{wrong, packet.pads}};
    friends.mesh.pending[receiver].clear();
    friends.mesh.pending[receiver].push_back({friends.mesh.tick, packet});
    friends.players[receiver]->TickCo(friends.mesh.tick);
    EXPECT_TRUE(friends.players[receiver]->session->ErrorFromPeer());
    EXPECT_EQ(friends.players[receiver]->session->Error(), "Invalid input group");
  }
}

TEST(OrcaCoSeats, EstablishedGroupMetadataCannotVanish)
{
  CoSeatMesh friends({{0, 1}, {2, 3}});
  friends.Play(400);
  Packet packet = friends.mesh.last_sent[0];
  packet.sequence += 100;
  packet.groups.reset();
  friends.mesh.pending[2].clear();
  friends.mesh.pending[2].push_back({friends.mesh.tick, packet});
  friends.players[2]->TickCo(friends.mesh.tick);
  EXPECT_TRUE(friends.players[2]->session->ErrorFromPeer());
  EXPECT_EQ(friends.players[2]->session->Error(), "Invalid roster groups");
}

TEST(OrcaCoSeats, StaleRosterGroupsDoNotRestoreDepartedMembership)
{
  CoSeatMesh friends({{0}, {1}, {2, 3}});
  friends.Play(400);
  Packet stale = friends.mesh.last_sent[0];
  friends.players[0]->session->DropSeat(2);
  friends.disabled[2] = true;
  friends.Play(60);
  ASSERT_EQ(friends.players[1]->session->LeadOf(3), 3);
  friends.mesh.pending[1].clear();
  friends.mesh.pending[1].push_back({friends.mesh.tick, stale});
  for (int attempt = 0; attempt < 4 && !friends.mesh.pending[1].empty(); ++attempt)
    friends.players[1]->TickCo(friends.mesh.tick);
  EXPECT_TRUE(friends.mesh.pending[1].empty());
  EXPECT_TRUE(friends.players[1]->session->Error().empty());
  EXPECT_EQ(friends.players[1]->session->LeadOf(3), 3);
  EXPECT_EQ(friends.players[1]->session->Plan()[3].live_from, NEVER);
}

TEST(OrcaCoSeats, UnusedCoPadFramesUseTheSamePaddingAsTheLead)
{
  CoSeatMesh friends({{0, 1}, {2}, {3}});
  friends.Play(400);
  friends.players[0]->neutral_seats[1] = true;
  friends.players[0]->session->UnplugSeat(1);
  const int unplug = friends.players[0]->session->Plan()[1].unplug_from;
  bool padding = false;
  for (int tick = 0; tick < 40; ++tick)
  {
    friends.Play(1);
    const Packet& packet = friends.mesh.last_sent[0];
    for (const auto& [seat, inputs] : packet.co)
      for (size_t index = 0; index < inputs.size(); ++index)
        if (packet.first_frame + static_cast<int>(index) >= unplug)
        {
          EXPECT_EQ(inputs[index], Pad{});
          padding = true;
        }
  }
  EXPECT_TRUE(padding);
  friends.Play(400);
  CompareCoFrames(friends);
}

TEST(OrcaCoSeats, VacatedLocalCoSeatCanBecomeARemoteMachine)
{
  for (const bool host_co : {false, true})
  {
    SCOPED_TRACE(host_co);
    CoSeatMesh friends(host_co ? std::vector<std::vector<int>>{{0, 1}, {2}, {3}} :
                                std::vector<std::vector<int>>{{0}, {1, 2}, {3}});
    friends.Play(600);
    const int seat = host_co ? 1 : 2;
    const int owner = host_co ? 0 : 1;
    friends.players[owner]->neutral_seats[seat] = true;
    friends.players[0]->session->UnplugSeat(seat);
    friends.Play(100);
    EXPECT_FALSE(friends.players[owner]->session->IsLocal(seat));
    const int first = RebuildCoMachine(friends, seat, {});
    friends.Play(900);
    CompareCoFrames(friends, first);
    CheckCoCounts(friends, 4, 4);
    EXPECT_FALSE(friends.players[owner]->session->IsLocal(seat));
    const auto& packet = friends.mesh.last_sent[owner];
    EXPECT_TRUE(std::ranges::none_of(packet.co,
        [seat](const auto& inputs) { return inputs.first == seat; }));
  }
}

namespace
{
std::array<u64, 2> SingleSeatLeaveGolden(bool before_plug)
{
  FourFriends friends;
  friends.mesh.latency = {0, 1, 2, 3};
  if (before_plug)
    friends.disabled[3] = true;
  for (int tick = 0; tick < 1800; ++tick)
  {
    if (before_plug && tick == 20)
    {
      EXPECT_EQ(friends.players[0]->session->Plan()[3].plug_from, NEVER);
      friends.players[0]->session->DropSeat(3);
    }
    if (!before_plug && tick == 400)
      friends.players[3]->session->RequestLeave();
    friends.Play(1);
    if (!before_plug && tick >= 400 && friends.players[3]->session->LeftDone())
      friends.disabled[3] = true;
  }
  EXPECT_TRUE(friends.disabled[3]);
  friends.Compare();
  std::array<u64, 2> hashes{};
  for (const auto& wire : friends.mesh.wire)
    for (const unsigned char byte : wire)
      hashes[0] = Mix(hashes[0], byte);
  for (const auto& player : friends.players)
    for (const auto& [frame, pads] : player->game.m_ran_pads)
    {
      hashes[1] = Mix(hashes[1], static_cast<u64>(frame));
      for (const Pad& pad : pads)
        for (const u8 byte : pad)
          hashes[1] = Mix(hashes[1], byte);
    }
  return hashes;
}
}

TEST(OrcaCoSeats, SingleSeatLeaveGoldens)
{
  const std::array<std::array<u64, 2>, 2> expected{{
      {0xea1b21ae0bbf358dull, 0x8234d50a77aa9ddeull},
      {0xdb737feccfdf981cull, 0x21b0105ae1dd8b8eull}}};
  EXPECT_EQ(SingleSeatLeaveGolden(false), expected[0]);
  EXPECT_EQ(SingleSeatLeaveGolden(true), expected[1]);
}

TEST(OrcaCoSeats, PendingCoSeatUnplugCanBeReplacedThroughEveryAdmissionPath)
{
  for (const bool lagged : {false, true})
  for (const bool local : {false, true})
  for (int method = 0; method < 3; ++method)
  {
    SCOPED_TRACE(lagged);
    SCOPED_TRACE(local);
    SCOPED_TRACE(method);
    CoSeatMesh friends(local ? std::vector<std::vector<int>>{{0, 2}, {1}} :
                               std::vector<std::vector<int>>{{0}, {1, 2}});
    if (lagged)
    {
      friends.mesh.latency = {0, 3, 1, 2};
      friends.mesh.pair_latency[0][1] = friends.mesh.pair_latency[1][0] = 8;
      for (auto& player : friends.players)
        if (player)
        {
          friends.mesh.reported_rtt[player->transport.seat] = 400;
          for (int sample = 0; sample < 3; ++sample)
            player->session->OnRoundTrip(400);
        }
    }
    friends.Play(600);
    auto& host = *friends.players[0];
    friends.players[local ? 0 : 1]->neutral_seats[2] = true;
    ASSERT_NE(host.session->Plan()[2].plug_from, NEVER);
    host.session->UnplugSeat(2);
    const int live = host.session->Plan()[2].unplug_from;
    const int first = host.session->CurrentFrame() - 30;
    const auto lead_plan = host.session->Plan()[local ? 0 : 1];
    ASSERT_GT(live, host.session->CurrentFrame());
    if (method == 0)
      host.session->AddPeer(2, live, first);
    else if (method == 1)
      host.session->AddPeerBatch({2}, live, first);
    else
    {
      host.session->SetGroup(2, {3});
      host.session->AddPeerBatch({2, 3}, live, first);
    }
    ASSERT_TRUE(host.session->Error().empty()) << host.session->Error();
    EXPECT_EQ(host.session->Plan()[local ? 0 : 1], lead_plan);
    EXPECT_EQ(host.session->LeadOf(2), 2);
    EXPECT_EQ(host.session->Plan()[2].live_from, live);
    if (method == 2)
      EXPECT_EQ(host.session->LeadOf(3), 2);
    friends.players[2] = std::make_unique<CoMeshPlayer>(friends.mesh, 2,
        method == 2 ? std::vector<int>{3} : std::vector<int>{}, std::vector<int>{0}, first);
    friends.players[2]->game.m_state = At(host.game.m_history, first);
    friends.mesh.pending[2].clear();
    for (auto& queue : friends.mesh.pending)
      std::erase_if(queue, [](const auto& pending) { return !pending.packet.co.empty(); });
    for (int tick = 0; tick < 900; ++tick)
    {
      for (auto& queue : friends.mesh.pending)
        std::erase_if(queue, [local](const auto& pending) {
          return pending.packet.seat == (local ? 0 : 1) && !pending.packet.co.empty();
        });
      friends.Play(1);
    }
    CompareCoFrames(friends, first);
    EXPECT_TRUE(host.session->Error().empty()) << host.session->Error();
    if (!local)
    {
      EXPECT_TRUE(friends.mesh.last_sent[1].departed.empty());
      bool replied = false;
      for (const auto& wire : friends.mesh.wire)
      {
        Packet packet;
        ASSERT_TRUE(DecodePacket(wire, &packet));
        if (packet.seat != 1 || packet.departed.empty())
          continue;
        replied = true;
        for (const auto& [departed_seat, prefix] : packet.departed)
        {
          EXPECT_EQ(departed_seat, 2);
          EXPECT_EQ(prefix.until, live);
          EXPECT_LE(prefix.pads.size(), static_cast<size_t>(MAX_PADS_IN_PACKET));
          EXPECT_LE(prefix.first_frame + static_cast<int>(prefix.pads.size()), live);
        }
      }
      if (lagged)
        EXPECT_TRUE(replied);
    }
  }
}

TEST(OrcaCoSeats, StepSessionReplaysThreeSeatRecordingThroughLoopback)
{
  FakeGame original;
  std::vector<Rollback::Loopback::RecordedFrame> recording;
  for (int frame = 0; frame < 600; ++frame)
  {
    Pads pads{};
    pads.fill(UNPLUGGED_PAD);
    for (int seat = 0; seat < 3; ++seat)
      pads[seat] = Pad{static_cast<u8>(frame / 11 + seat), 0, 0, 0,
                       static_cast<u8>(seat + 1), 0, 0, 0};
    recording.push_back({pads, original.m_state});
    original.SetPads(frame, pads);
    original.Run();
  }
  FakeGame replay;
  Rollback::Loopback::VirtualRemote remote(recording, {1}, 0, 9, 2, 2, 10, {2});
  Config config;
  config.seats = MAX_SEATS;
  config.local_seat = 0;
  config.co_seats = {2};
  config.time_sync = false;
  config.fixed_delay = 2;
  config.checksum_every = 10;
  config.snapshot_every = 1;
  config.start_pad = recording[0].pads[0];
  config.start_pads[2] = recording[0].pads[2];
  std::array<SeatPlan, MAX_SEATS> plan{};
  for (int seat = 0; seat < 3; ++seat)
    plan[seat] = {0, NEVER, 0};
  config.plan = plan;
  Session session(config, replay, remote);
  int completed = -1;
  int calls = 0;
  int polls = 0;
  for (int guard = 0; guard < 4000 && completed < 350; ++guard)
  {
    if (!session.Resimulating())
      remote.Tick();
    std::array<int, MAX_SEATS> sampled{};
    const Step step = Rollback::StepSession(session, replay, completed,
        std::function<Pad(int)>([&](int seat) {
          ++sampled[seat];
          return recording.at(session.CurrentFrame() + session.Delay()).pads[seat];
        }), [&] { ++polls; remote.TickWhileWaiting(); return polls > 4000; },
        std::chrono::microseconds(0));
    EXPECT_EQ(sampled, (std::array<int, MAX_SEATS>{1, 0, 1, 0}));
    ++calls;
    ASSERT_NE(step.kind, StepKind::Ended) << session.Error();
    completed = replay.Run();
  }
  ASSERT_GE(completed, 350);
  EXPECT_GT(calls, 350);
  EXPECT_GT(polls, 0);
  EXPECT_GT(session.GetStats().checksums_matched, 10);
  EXPECT_GT(session.GetStats().rollbacks, 0);
  EXPECT_LE(session.GetStats().deepest_rollback, config.max_rollback);
  for (int frame = 0; frame < session.FinalFrame(); ++frame)
  {
    EXPECT_EQ(At(replay.m_history, frame), recording[frame].checksum) << frame;
    EXPECT_EQ(At(replay.m_ran_pads, frame), recording[frame].pads) << frame;
  }
}

TEST(OrcaCoSeats, MissingCoSeatDropsItsWholeRemoteMachine)
{
  CoSeatMesh friends({{0}, {1, 2}, {3}});
  friends.Play(500);
  for (int tick = 0; tick < 40; ++tick)
  {
    for (auto& pending : friends.mesh.pending[0])
      if (pending.packet.seat == 1)
        pending.packet.co.clear();
    friends.Play(1);
  }
  auto& host = *friends.players[0]->session;
  ASSERT_EQ(host.MissingInputSeats(), (std::vector<int>{2}));
  const auto dropped = Rollback::OnlineMatch::DropMissingFriends(host, true);
  ASSERT_TRUE(dropped);
  EXPECT_EQ(*dropped, (std::vector<int>{1, 2}));
  EXPECT_EQ(host.Plan()[1].live_from, NEVER);
  EXPECT_EQ(host.Plan()[2].live_from, NEVER);
  EXPECT_NE(host.Plan()[3].live_from, NEVER);
  friends.disabled[1] = true;
  friends.Play(500);
  CompareCoFrames(friends);
}

TEST(OrcaCoSeats, CoSeatCancelledBeforePlugAcceptsTheFirstAdmissionRoster)
{
  for (const bool saw_roster : {false, true})
  {
    CoSeatMesh friends({{0}, {1, 2}});
    if (saw_roster)
      friends.Play(1);
    auto& host = *friends.players[0]->session;
    ASSERT_EQ(host.Plan()[2].plug_from, NEVER);
    host.UnplugSeat(2);
    EXPECT_EQ(host.Plan()[2].live_from, NEVER);
    EXPECT_EQ(host.Plan()[2].unplug_from, host.Plan()[1].live_from);
    friends.Play(900);
    CompareCoFrames(friends);
    CheckCoCounts(friends, 2, 2);
    EXPECT_FALSE(friends.players[1]->session->IsLocal(2));
    EXPECT_NE(host.Plan()[1].plug_from, NEVER);
    EXPECT_EQ(host.Plan()[1].unplug_from, NEVER);
  }
}

TEST(OrcaCoSeats, InvalidOrLocalDropStillClosesAnEmptyBatchGate)
{
  for (const int seat : {-1, 0, MAX_SEATS})
  {
    FriendsMesh mesh;
    CoMeshPlayer host(mesh, 0, {}, {0});
    host.session->AddPeerBatch({}, 300, 240);
    host.session->DropSeat(seat);
    host.TickCo(0);
    ASSERT_TRUE(mesh.last_sent[0].start_gate);
    EXPECT_NE(mesh.last_sent[0].start_gate->second, NEVER);
    EXPECT_TRUE(host.session->Error().empty()) << host.session->Error();
  }
}

TEST(OrcaCoSeats, ReplacementLeadCanArriveBeforeItsAuthorityRoster)
{
  CoSeatMesh friends({{0}, {1, 2}});
  friends.Play(500);
  Packet replacement;
  replacement.seat = 2;
  replacement.sequence = 1;
  replacement.current_frame = friends.players[0]->session->CurrentFrame();
  friends.mesh.pending[0].push_back({friends.mesh.tick, replacement});
  friends.players[0]->TickCo(friends.mesh.tick);
  EXPECT_TRUE(friends.players[0]->session->Error().empty());
  EXPECT_EQ(friends.players[0]->session->LeadOf(2), 1);
}

TEST(OrcaCoSeats, SingleSeatJoinerPlaysQueuedPadsAfterMatchOver)
{
  for (const int seat : {1, 2, 3})
  {
    for (int delay = 2; delay <= 6; ++delay)
    {
      FakeGame game;
      FakeNet net;
      FakeNet::End transport(net, 1);
      Config config;
      config.seats = 4;
      config.local_seat = seat;
      config.input_delay = delay;
      config.fixed_delay = delay;
      config.time_sync = false;
      auto session = std::make_unique<Session>(config, game, transport);
      for (int frame = 0; frame < 3; ++frame)
      {
        const Pad pad{0, static_cast<u8>(frame + 1), 0, 0, 0, 0, 0, 0};
        ASSERT_EQ(session->OnBoundary(frame - 1, pad).kind, StepKind::Run);
        game.Run();
      }
      const auto pending = session->PendingLocal();
      ASSERT_EQ(pending.size(), static_cast<size_t>(delay));
      const Pad fresh{0, 0x7f, 0, 0, 0, 0, 0, 0};
      int reads = 0;
      const auto log = Rollback::OnlineMatch::PumpJoinerSoloForTest(std::move(session),
          delay + 1, [&](int local) {
            EXPECT_EQ(local, 0);
            ++reads;
            return fresh;
          });
      ASSERT_GE(log.size(), pending.size() + 1);
      const size_t first = log.size() - pending.size() - 1;
      for (size_t i = 0; i < pending.size(); ++i)
      {
        EXPECT_EQ(log[first + i][0], pending[i]) << seat << ":" << delay << ":" << i;
        for (int port = 1; port < MAX_SEATS; ++port)
          EXPECT_EQ(log[first + i][port], UNPLUGGED_PAD);
      }
      EXPECT_EQ(log.back()[0], fresh);
      EXPECT_EQ(reads, 1) << seat << ":" << delay;
    }
  }
}

TEST(OrcaCoSeats, SessionLeadDeterminesLocalInputEvenBeforeMatchSeatAgrees)
{
  FakeGame game;
  FakeNet net;
  FakeNet::End transport(net, 1);
  for (const int seat : {1, 2, 3})
  {
    Config config;
    config.seats = 4;
    config.local_seat = seat;
    auto session = std::make_unique<Session>(config, game, transport);
    EXPECT_EQ(Rollback::OnlineMatch::PumpLeadSeatForTest(std::move(session), 0), seat);
  }
  EXPECT_EQ(Rollback::OnlineMatch::PumpLeadSeatForTest(nullptr, 2), 2);
}

TEST(OrcaCoSeats, LateCoInputAfterReadmissionKeepsTheLeadPlaying)
{
  CoSeatMesh friends({{0}, {1, 2}});
  friends.Play(500);
  auto& host = *friends.players[0];
  friends.players[1]->neutral_seats[2] = true;
  host.session->UnplugSeat(2);
  const int live = host.session->Plan()[2].unplug_from;
  Packet late = friends.mesh.last_sent[1];
  ASSERT_EQ(late.co.size(), 1u);
  host.session->AddPeer(2, live, host.session->CurrentFrame() - 30);
  ASSERT_TRUE(host.session->Error().empty());
  ASSERT_EQ(host.session->LeadOf(2), 2);
  late.sequence += 100;
  friends.mesh.pending[0].push_back({friends.mesh.tick, late});
  host.TickCo(friends.mesh.tick);
  EXPECT_TRUE(host.session->Error().empty()) << host.session->Error();
  EXPECT_FALSE(host.session->ErrorFromPeer());
  EXPECT_EQ(host.session->Plan()[1].unplug_from, NEVER);
}

TEST(OrcaCoSeats, RetakenSlotRunsPastUnplugWithoutDroppingItsFormerMachine)
{
  CoSeatMesh friends({{0}, {1, 2}, {3}});
  friends.Play(500);
  auto& host = *friends.players[0]->session;
  friends.players[1]->neutral_seats[2] = true;
  host.UnplugSeat(2);
  const int unplug = host.Plan()[2].unplug_from;
  const auto lead_plan = host.Plan()[1];
  host.PrepareAdmission(2);
  ASSERT_EQ(host.Plan()[2].live_from, NEVER);
  ASSERT_EQ(host.Plan()[2].unplug_from, unplug);
  friends.Play(900);
  for (const auto& player : friends.players)
    if (player)
    {
      ASSERT_TRUE(player->session->Error().empty()) << player->session->Describe();
      EXPECT_GT(player->session->FinalFrame(), unplug + 500);
      EXPECT_EQ(player->session->Plan()[1], lead_plan);
      EXPECT_NE(player->session->Plan()[3].live_from, NEVER);
      EXPECT_EQ(player->session->PeersInPlay(), 3);
    }
  CompareCoFrames(friends);
}

TEST(OrcaCoSeats, RetiredCopiesDoNotCarryIntoANewLeadStay)
{
  CoSeatMesh friends({{0}, {1, 2}});
  friends.Play(500);
  auto& host = *friends.players[0]->session;
  host.UnplugSeat(2);
  host.PrepareAdmission(2);
  host.DropSeat(1);
  host.AddPeer(1, host.CurrentFrame(), host.CurrentFrame());
  Packet invalid = friends.mesh.last_sent[1];
  invalid.sequence += 100;
  friends.mesh.pending[0].clear();
  friends.mesh.pending[0].push_back({friends.mesh.tick, invalid});
  friends.players[0]->TickCo(friends.mesh.tick);
  EXPECT_TRUE(host.ErrorFromPeer());
  EXPECT_EQ(host.Error(), "Invalid input group");
}
