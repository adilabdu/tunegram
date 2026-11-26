// Scenario tests: real lt_core clients over the simulated network and the
// reference sequencer.
#include <algorithm>
#include <cinttypes>
#include <climits>
#include <cstdio>
#include <string>
#include <vector>

#include "framework.hpp"
#include "internal.hpp"
#include "protocol.hpp"
#include "sim.hpp"

using sim::SimClient;
using sim::World;

namespace {

void setup(World& w, int n, sim::LinkParams link = {}) {
  w.server.create("u0", "-1001", "Test");
  for (int i = 0; i < n; ++i) {
    SimClient& c = w.add_client("u" + std::to_string(i), "N" + std::to_string(i));
    c.link = link;
  }
  for (auto& c : w.clients) c->start();
  w.sim.run_for(3000);
}

lt::Session& S(SimClient& c) { return *lt::session_of(c.core()); }

// Max - min of the audible positions of clients playing the reference item.
int64_t spread(World& w) {
  lt::Timeline ref = w.reference(w.sim.now);
  int64_t lo = INT64_MAX, hi = INT64_MIN;
  int n = 0;
  for (auto& c : w.clients) {
    if (!c->core() || !c->player.audible() || c->player.item != ref.item) continue;
    int64_t p = c->player.pos_at(w.sim.now);
    lo = std::min(lo, p);
    hi = std::max(hi, p);
    ++n;
  }
  return n ? hi - lo : 0;
}

// Max |position - reference| over clients that should be playing; a client
// that is not audible on the reference item counts as a huge error.
int64_t ref_error(World& w, int skip = -1) {
  lt::Timeline ref = w.reference(w.sim.now);
  if (!ref.playing) return 0;
  int64_t worst = 0;
  for (auto& c : w.clients) {
    if (!c->core() || c->idx == skip) continue;
    if (S(*c).conn_state() == lt::ConnState::Terminal) continue;
    if (!c->player.audible() || c->player.item != ref.item) return 1000000;
    worst = std::max(worst, std::llabs(c->player.pos_at(w.sim.now) - ref.pos));
  }
  return worst;
}

int audible_on(World& w, const std::string& item) {
  int n = 0;
  for (auto& c : w.clients) n += c->core() && c->player.audible() && c->player.item == item;
  return n;
}

std::string pct(std::vector<int64_t> v, double q) {
  if (v.empty()) return "-";
  std::sort(v.begin(), v.end());
  size_t i = static_cast<size_t>(q * static_cast<double>(v.size() - 1));
  return std::to_string(v[i]);
}

}  // namespace

// ---------------------------------------------------------------------------

TEST(clock_offset_estimation) {
  int seeds = tf::env_int("LT_SEEDS", 200);
  std::vector<int64_t> errs;
  for (int seed = 1; seed <= seeds; ++seed) {
    World w(static_cast<uint64_t>(seed));
    sim::Rng rng(static_cast<uint64_t>(seed) * 13);
    w.server.create("u0", "-1");
    SimClient& c = w.add_client("u0", "A");
    int64_t base = rng.range(10, 150);
    c.link.up_ms = c.link.down_ms = base;
    c.link.jitter_ms = rng.range(0, 40);
    c.start();
    w.sim.run_for(3000);
    REQUIRE(c.live());
    int64_t err = c.local_now() + S(c).clock().offset() - w.sim.now;  // estimated - true server time
    errs.push_back(std::llabs(err));
  }
  std::vector<int64_t> sorted = errs;
  std::sort(sorted.begin(), sorted.end());
  int64_t p95 = sorted[static_cast<size_t>(0.95 * static_cast<double>(sorted.size() - 1))];
  std::printf("     clock: %d seeds, symmetric latency 10-150 ms + jitter 0-40 ms: |error| p50=%s p95=%s max=%s ms\n",
              seeds, pct(errs, 0.5).c_str(), pct(errs, 0.95).c_str(), pct(errs, 1.0).c_str());
  CHECK(p95 <= 10);
  CHECK(sorted.back() <= 25);
}

TEST(clock_asymmetry_and_drift) {
  // Asymmetric path: the estimate is off by half the asymmetry (theoretical limit of NTP-style sync).
  {
    World w(5);
    w.server.create("u0", "-1");
    SimClient& c = w.add_client("u0", "A");
    c.link.up_ms = 20;
    c.link.down_ms = 120;
    c.link.jitter_ms = 4;
    c.start();
    w.sim.run_for(3000);
    int64_t err = c.local_now() + S(c).clock().offset() - w.sim.now;
    CHECK_MSG(err <= -40 && err >= -60, std::to_string(err));
  }
  // Drift of +-50 ppm over 10 minutes stays tracked by periodic pings.
  {
    World w(6);
    w.server.create("u0", "-1");
    SimClient& c = w.add_client("u0", "A");
    c.drift_ppm = 50;
    c.link.jitter_ms = 20;
    c.start();
    int64_t worst = 0;
    for (int i = 0; i < 60; ++i) {
      w.sim.run_for(10000);
      worst = std::max(worst, std::llabs(c.local_now() + S(c).clock().offset() - w.sim.now));
    }
    CHECK_MSG(worst <= 25, std::to_string(worst));
  }
}

TEST(local_first_commands) {
  sim::LinkParams link{150, 150, 20};
  World w(7);
  setup(w, 3, link);
  SimClient& a = *w.clients[0];
  SimClient& b = *w.clients[1];
  SimClient& c = *w.clients[2];
  a.insert(nullptr, 3, 300000);
  w.sim.run_for(2000);
  a.play();
  // Initiator: the player is commanded in the same instant (no round trip).
  CHECK(a.player.st == sim::FakePlayer::Starting || a.player.st == sim::FakePlayer::Loading ||
        a.player.st == sim::FakePlayer::Playing);
  w.sim.run_for(4000);
  CHECK_EQ(audible_on(w, a.player.item), 3);
  CHECK_MSG(spread(w) <= 50, std::to_string(spread(w)));

  b.pause();
  CHECK(b.player.st == sim::FakePlayer::Paused);  // immediate
  CHECK(a.player.st == sim::FakePlayer::Playing);  // remote: not yet
  CHECK(c.player.st == sim::FakePlayer::Playing);
  w.sim.run_for(1000);
  CHECK(a.player.st == sim::FakePlayer::Paused);
  CHECK(c.player.st == sim::FakePlayer::Paused);
  int64_t lo = std::min({a.player.pos_base, b.player.pos_base, c.player.pos_base});
  int64_t hi = std::max({a.player.pos_base, b.player.pos_base, c.player.pos_base});
  CHECK_MSG(hi - lo <= 30, std::to_string(hi - lo));  // everyone parks at the initiator's position

  c.seek(60000);
  CHECK_EQ(c.player.pos_base, static_cast<int64_t>(60000));
  w.sim.run_for(1000);
  uint64_t a_seeks_before = S(a).stats().reanchor_seeks;
  a.play();
  w.sim.run_for(3000);
  // Remote devices compensate for elapsed time: no one trails by the latency (150-340 ms here).
  for (int i = 0; i < 10; ++i) {
    w.sim.run_for(700);
    CHECK_MSG(ref_error(w) <= 50, std::to_string(ref_error(w)));
    CHECK_MSG(spread(w) <= 60, std::to_string(spread(w)));
  }
  CHECK_EQ(S(a).stats().reanchor_seeks, a_seeks_before);  // own confirmation: no re-seek
  CHECK(S(b).stats().reanchor_seeks >= 1);  // c's remote seek moved b's player
}

TEST(reanchor_start_seek_resume) {
  sim::LinkParams link{60, 60, 30};
  World w(8);
  setup(w, 4, link);
  SimClient& a = *w.clients[0];
  a.insert(nullptr, 2, 400000);
  w.sim.run_for(1500);
  a.play();  // start
  w.sim.run_for(2500);
  CHECK_MSG(ref_error(w) <= 50, std::to_string(ref_error(w)));
  w.clients[1]->seek(100000);  // seek
  w.sim.run_for(1500);
  lt::Timeline ref = w.reference(w.sim.now);
  CHECK(ref.pos >= 100000 && ref.pos < 102000);
  CHECK_MSG(ref_error(w) <= 50, std::to_string(ref_error(w)));
  w.clients[2]->pause();
  w.sim.run_for(3000);
  w.clients[3]->play();  // resume
  w.sim.run_for(1500);
  CHECK_MSG(ref_error(w) <= 50, std::to_string(ref_error(w)));
  // Every remote device re-anchored by seeking; the initiators did not need to.
  CHECK(S(*w.clients[0]).stats().reanchor_seeks >= 1);
  CHECK_EQ(S(*w.clients[1]).stats().corrections, static_cast<uint64_t>(0));
}

namespace {
struct CorrectionRig {
  World w;
  std::vector<int64_t> seek_times;
  explicit CorrectionRig(uint64_t seed) : w(seed) {
    setup(w, 2, sim::LinkParams{40, 40, 10});
    w.clients[0]->insert(nullptr, 1, 3600000);
    w.sim.run_for(1000);
    w.clients[0]->play();
    w.sim.run_for(5000);
    w.clients[1]->on_event = [this](const lt_event& ev) {
      if (ev.type == LT_EV_PLAYER_SEEK) seek_times.push_back(w.sim.now);
    };
  }
  SimClient& b() { return *w.clients[1]; }
};
}  // namespace

TEST(correction_two_consecutive_checks) {
  CorrectionRig r(9);
  SimClient& b = r.b();
  REQUIRE(ref_error(r.w) <= 50);
  int64_t t_glitch = r.w.sim.now;
  b.glitch(600);  // audio jumps ahead by 600 ms
  r.w.sim.run_for(5000);
  REQUIRE_MSG(r.seek_times.size() == 1, std::to_string(r.seek_times.size()));
  int64_t dt = r.seek_times[0] - t_glitch;
  // Seek only after the second consecutive check (checks are ~1 s apart).
  CHECK_MSG(dt > 1000 && dt <= 2100, std::to_string(dt));
  CHECK_EQ(S(b).stats().corrections, static_cast<uint64_t>(1));
  CHECK_MSG(ref_error(r.w) <= 100, std::to_string(ref_error(r.w)));
}

TEST(correction_single_spike_ignored) {
  CorrectionRig r(10);
  SimClient& b = r.b();
  // Align with the 1 s check grid: glitch right after a check, undo 1.5 s later,
  // so exactly one check sees the error.
  bool armed = true;
  b.on_event = [&](const lt_event& ev) {
    if (ev.type == LT_EV_PLAYER_SEEK) r.seek_times.push_back(r.w.sim.now);
    if (ev.type == LT_EV_PLAYER_QUERY && armed) {
      armed = false;
      r.w.sim.after(5, [&] { b.glitch(700); });
      r.w.sim.after(1505, [&] { b.glitch(-700); });
    }
  };
  r.w.sim.run_for(8000);
  CHECK_EQ(S(b).stats().corrections, static_cast<uint64_t>(0));
  CHECK(r.seek_times.empty());
}

TEST(correction_below_threshold_never_seeks) {
  CorrectionRig r(11);
  SimClient& b = r.b();
  b.glitch(350);
  r.w.sim.run_for(30000);
  CHECK_EQ(S(b).stats().corrections, static_cast<uint64_t>(0));
  CHECK(r.seek_times.empty());
}

TEST(correction_drift_is_fixed_by_seeks_not_rate) {
  CorrectionRig r(12);
  SimClient& b = r.b();
  b.player.rate = 1.004;  // runs 4 ms/s fast (far worse than real devices)
  b.glitch(0);            // re-base the fake player at the new rate
  int64_t worst = 0;
  for (int i = 0; i < 300; ++i) {
    r.w.sim.run_for(1000);
    worst = std::max(worst, ref_error(r.w));
  }
  uint64_t n = S(b).stats().corrections;
  CHECK_MSG(n >= 2 && n <= 4, std::to_string(n));
  CHECK_MSG(worst <= 420, std::to_string(worst));
  // There is no rate command in the API at all: corrections are seeks only.
  CHECK_EQ(static_cast<size_t>(n), r.seek_times.size());
}

TEST(join_at_live_position_when_audio_starts) {
  for (int variant = 0; variant < 2; ++variant) {
    World w(13 + static_cast<uint64_t>(variant));
    w.server.create("u0", "-1001");
    SimClient& a = w.add_client("u0", "A");
    a.start();
    w.sim.run_for(2000);
    a.insert(nullptr, 1, 600000);
    w.sim.run_for(500);
    a.play();
    w.sim.run_for(30000);
    SimClient& b = w.add_client("u1", "B");
    b.link = sim::LinkParams{80, 80, 20};
    b.player.load_ms = 1500;  // slow to prepare
    b.player.load_jitter_ms = 0;
    b.player.start_ms = 250;  // slow to actually start
    b.player.extrapolate = variant == 0;
    b.start();
    // Run until b's audio starts.
    for (int i = 0; i < 100 && !b.player.audible(); ++i) w.sim.run_for(50);
    REQUIRE(b.player.audible());
    REQUIRE(b.player.has_start_error);
    if (variant == 0) {
      // Adapter extrapolates `pos` from `ref` to the real start: on the live position at once.
      CHECK_MSG(b.player.last_start_error <= 30, std::to_string(b.player.last_start_error));
    } else {
      // Naive adapter: ~start latency behind, then the core seeks when audio started.
      CHECK(b.player.last_start_error >= 200);
    }
    w.sim.run_for(3000);
    CHECK_MSG(ref_error(w) <= 100, std::to_string(ref_error(w)));
    std::printf("     join (%s adapter): error at audio start %" PRId64 " ms, after 3 s %" PRId64 " ms\n",
                variant == 0 ? "extrapolating" : "naive", b.player.last_start_error, ref_error(w));
  }
}

TEST(reconnect_catch_up) {
  World w(14);
  setup(w, 3, sim::LinkParams{50, 50, 20});
  SimClient& a = *w.clients[0];
  SimClient& b = *w.clients[1];
  SimClient& c = *w.clients[2];
  std::vector<std::string> ids = a.insert(nullptr, 4, 300000);
  w.sim.run_for(1000);
  a.play();
  w.sim.run_for(3000);
  REQUIRE(ref_error(w) <= 50);

  w.set_net(2, false);
  w.sim.run_for(3000);
  CHECK_EQ(c.conn_state(), std::string("reconnecting"));
  CHECK(c.player.audible());  // keeps playing on the last known timeline
  std::vector<std::string> mine = c.insert(nullptr, 1, 100000);  // offline edit: survives
  c.pause();  // offline playback command: stale by the time it arrives
  a.insert(&ids[3], 2, 100000);
  b.remove(ids[1]);
  w.sim.run_for(5000);
  a.seek(50000);
  w.sim.run_for(4000);
  b.pause();
  w.sim.run_for(2000);
  b.play();
  w.sim.run_for(6000);
  w.set_net(2, true);
  w.sim.run_for(15000);
  std::string why;
  CHECK_MSG(w.converged(&why), why);
  std::vector<std::string> q = c.queue_ids();
  CHECK(std::find(q.begin(), q.end(), mine[0]) != q.end());
  CHECK(std::find(q.begin(), q.end(), ids[1]) == q.end());
  CHECK(w.reference(w.sim.now).playing);  // the stale offline pause had no effect
  CHECK_MSG(ref_error(w) <= 60, std::to_string(ref_error(w)));
  CHECK(S(c).stats().reconnects >= 1);
}

TEST(navigation_next_previous_in_session) {
  World w(15);
  setup(w, 2, sim::LinkParams{40, 40, 10});
  SimClient& a = *w.clients[0];
  SimClient& b = *w.clients[1];
  std::vector<std::string> ids = a.insert(nullptr, 3, 300000);
  w.sim.run_for(1000);
  a.play();
  w.sim.run_for(10000);
  // Both press next at the same moment: one skip.
  a.next();
  b.next();
  w.sim.run_for(2000);
  CHECK_EQ(w.reference(w.sim.now).item, ids[1]);
  CHECK_EQ(audible_on(w, ids[1]), 2);
  w.sim.run_for(1000);
  // Previous within 3 s of the start: back to the previous history entry.
  b.previous();
  CHECK_EQ(b.player.item, ids[0]);  // local-first: b loads it immediately
  w.sim.run_for(5000);
  CHECK_EQ(w.reference(w.sim.now).item, ids[0]);
  CHECK_EQ(audible_on(w, ids[0]), 2);
  // Previous past 3 s: restart the same track.
  a.previous();
  w.sim.run_for(1000);
  lt::Timeline ref = w.reference(w.sim.now);
  CHECK_EQ(ref.item, ids[0]);
  CHECK(ref.pos < 1500);
  CHECK_MSG(ref_error(w) <= 60, std::to_string(ref_error(w)));
}

TEST(shuffle_shared_order_and_auto_advance) {
  World w(16);
  setup(w, 3, sim::LinkParams{40, 40, 10});
  SimClient& a = *w.clients[0];
  std::vector<std::string> ids = a.insert(nullptr, 6, 5000);
  w.sim.run_for(1000);
  w.clients[1]->set_shuffle(true);
  w.sim.run_for(1000);
  std::vector<std::string> order = a.queue_ids();
  CHECK(order != ids);  // shuffled (seeded by the server)
  for (auto& c : w.clients) CHECK(c->queue_ids() == order);  // identical everywhere
  CHECK_EQ(order[0], ids[0]);  // current item first
  a.play();
  w.sim.run_for(45000);
  // Each client played every item exactly once, in the shared shuffled order.
  for (auto& c : w.clients) {
    if (tf::verbose()) {
      std::string e, cu, o;
      for (auto& x : c->player.ended_items) e += x + " ";
      for (auto& x : c->player.cut_items) cu += x + " ";
      for (auto& x : order) o += x + " ";
      std::printf("%s ended=[%s] cut=[%s] order=[%s]\n", c->name.c_str(), e.c_str(), cu.c_str(), o.c_str());
    }
    CHECK_MSG(c->player.ended_items == order, c->name);
    CHECK(c->player.cut_items.empty());
  }
  lt::Timeline ref = w.reference(w.sim.now);
  CHECK(!ref.playing);  // repeat off: end of queue, parked at the first item
  CHECK_EQ(ref.item, order[0]);
}

TEST(repeat_one_and_all) {
  World w(17);
  setup(w, 2, sim::LinkParams{40, 40, 10});
  SimClient& a = *w.clients[0];
  std::vector<std::string> ids = a.insert(nullptr, 2, 4000);
  w.sim.run_for(1000);
  a.set_repeat(LT_REPEAT_ONE);
  a.play();
  w.sim.run_for(13000);
  for (auto& c : w.clients) {
    REQUIRE(c->player.ended_items.size() >= 3);
    for (const auto& it : c->player.ended_items) CHECK_EQ(it, ids[0]);
  }
  w.clients[1]->set_repeat(LT_REPEAT_ALL);
  w.sim.run_for(20000);
  for (auto& c : w.clients) {
    const auto& e = c->player.ended_items;
    bool wrapped = false;
    for (size_t i = 1; i < e.size(); ++i) wrapped = wrapped || (e[i - 1] == ids[1] && e[i] == ids[0]);
    CHECK_MSG(wrapped, c->name);
  }
  CHECK(w.reference(w.sim.now).playing);
  CHECK_MSG(ref_error(w) <= 100, std::to_string(ref_error(w)));
}

TEST(cap_participants_10) {
  World w(18);
  w.server.create("u0", "-1001");
  for (int i = 0; i < 11; ++i) w.add_client("u" + std::to_string(i), "N" + std::to_string(i));
  for (int i = 0; i < 10; ++i) w.clients[static_cast<size_t>(i)]->start();
  w.sim.run_for(3000);
  SimClient& late = *w.clients[10];
  late.start();
  w.sim.run_for(60000);
  for (int i = 0; i < 10; ++i) CHECK(w.clients[static_cast<size_t>(i)]->live());
  CHECK_EQ(late.conn_state(), std::string("closed"));
  CHECK(std::find(late.fatal_errors.begin(), late.fatal_errors.end(), "session_full") != late.fatal_errors.end());
  CHECK_EQ(late.connects, 1);  // no reconnect loop
  CHECK_EQ(static_cast<int>(w.server.participants()), 10);
  CHECK_EQ(static_cast<int>(S(*w.clients[0]).view().participants.size()), 10);
  // Someone leaves; a fresh attempt by the 11th user now succeeds.
  w.clients[5]->leave();
  w.sim.run_for(2000);
  SimClient& again = w.add_client("u10", "N10");
  again.start();
  w.sim.run_for(3000);
  CHECK(again.live());
  CHECK_EQ(static_cast<int>(S(*w.clients[0]).view().participants.size()), 10);
}

TEST(version_mismatch_is_terminal) {
  {
    World w(19);
    w.server.create("u0", "-1001");
    SimClient& c = w.add_client("u0", "A", 2);  // newer client, server speaks 1
    c.start();
    w.sim.run_for(60000);
    CHECK_EQ(c.conn_state(), std::string("closed"));
    CHECK(std::find(c.fatal_errors.begin(), c.fatal_errors.end(), "version_unsupported") != c.fatal_errors.end());
    CHECK_EQ(c.connects, 1);
    CHECK(S(c).terminal_code() == "version_unsupported");
    std::string js = c.state_json();
    CHECK(js.find("\"error\":\"version_unsupported\"") != std::string::npos);
  }
  {
    World w(20);
    w.server.create("u0", "-1001");
    w.server.min_v = 2;  // server raised MIN_V; old app must update
    w.server.max_v = 3;
    SimClient& c = w.add_client("u0", "A");
    c.start();
    w.sim.run_for(30000);
    CHECK_EQ(c.conn_state(), std::string("closed"));
    CHECK_EQ(c.connects, 1);
  }
}

TEST(idempotent_duplicate_frames) {
  World w(21);
  sim::LinkParams link{40, 40, 30};
  link.dup = 1.0;  // every frame is delivered twice, both directions
  link.reorder = true;
  setup(w, 3, link);
  for (int i = 0; i < 3; ++i) w.clients[static_cast<size_t>(i)]->insert(nullptr, 2, 100000);
  w.sim.run_for(1000);
  w.clients[0]->play();
  w.sim.run_for(5000);
  std::string why;
  CHECK_MSG(w.converged(&why), why);
  CHECK(w.server.dup_acks > 0);
  CHECK_EQ(lt::alive_count(w.server.replay()), static_cast<size_t>(6));
  // Each op_id appears once in the log.
  std::vector<std::string> op_ids;
  for (const auto& op : w.server.parsed_log()) op_ids.push_back(op.op_id);
  std::sort(op_ids.begin(), op_ids.end());
  CHECK(std::adjacent_find(op_ids.begin(), op_ids.end()) == op_ids.end());
}

TEST(idempotent_resend_after_lost_event) {
  World w(22);
  setup(w, 2, sim::LinkParams{40, 40, 10});
  SimClient& a = *w.clients[0];
  int64_t head0 = w.server.head();
  a.block_down_until = w.sim.now + 7000;  // a hears nothing for 7 s (its event and acks are lost)
  std::vector<std::string> ids = a.insert(nullptr, 1, 100000);
  w.sim.run_for(20000);
  std::string why;
  CHECK_MSG(w.converged(&why), why);
  CHECK_EQ(w.server.head(), head0 + 1);  // appended once
  CHECK(w.server.dup_acks >= 1);         // the resend was acknowledged as a duplicate
  CHECK(S(a).stats().resends >= 1);
  std::vector<std::string> q = a.queue_ids();
  CHECK_EQ(std::count(q.begin(), q.end(), ids[0]), 1);
}

TEST(advance_audio_longer_than_metadata_no_cut) {
  World w(23);
  setup(w, 3, sim::LinkParams{50, 50, 20});
  SimClient& a = *w.clients[0];
  std::vector<std::string> ids = a.insert(nullptr, 3, 8000);
  for (auto& c : w.clients) {
    for (const auto& id : ids) c->player.extra_ms[id] = 700;  // real audio is 0.7 s longer
  }
  w.sim.run_for(1000);
  a.play();
  w.sim.run_for(12000);
  for (auto& c : w.clients) {
    CHECK_MSG(c->player.cut_items.empty(), c->name);  // the tail was not cut
    REQUIRE(!c->player.ended_items.empty());
    CHECK_EQ(c->player.ended_items[0], ids[0]);
  }
  // First advanced re-anchors, the duplicates are no-ops: one epoch bump per boundary.
  int advanced_records = 0;
  for (const auto& op : w.server.parsed_log()) advanced_records += op.kind == lt::Kind::Advanced;
  CHECK(advanced_records >= 1);
  w.sim.run_for(1500);
  CHECK_EQ(w.reference(w.sim.now).item, ids[1]);
  CHECK_MSG(spread(w) <= 100, std::to_string(spread(w)));
}

TEST(advance_audio_shorter_than_metadata) {
  World w(24);
  setup(w, 3, sim::LinkParams{50, 50, 20});
  SimClient& a = *w.clients[0];
  std::vector<std::string> ids = a.insert(nullptr, 3, 8000);
  for (auto& c : w.clients) {
    for (const auto& id : ids) c->player.extra_ms[id] = -1200;  // real audio ends 1.2 s early
  }
  w.sim.run_for(1000);
  a.play();
  w.sim.run_for(8000);
  lt::State st = w.server.replay();
  CHECK_EQ(st.cur, ids[1]);  // re-anchored at the real end
  w.sim.run_for(1000);
  CHECK_MSG(ref_error(w) <= 100, std::to_string(ref_error(w)));
  for (auto& c : w.clients) CHECK_EQ(c->player.ended_items.size(), static_cast<size_t>(1));
}

TEST(interrupt_and_catch_up) {
  World w(25);
  setup(w, 2, sim::LinkParams{40, 40, 10});
  SimClient& a = *w.clients[0];
  SimClient& b = *w.clients[1];
  a.insert(nullptr, 1, 600000);
  w.sim.run_for(1000);
  a.play();
  w.sim.run_for(3000);
  lt_core_set_interrupted(b.core(), b.local_now(), 1);
  b.pump();
  CHECK(b.player.st == sim::FakePlayer::Paused);
  w.sim.run_for(10000);
  CHECK(w.reference(w.sim.now).playing);  // others continue
  CHECK(a.player.audible());
  CHECK(a.state_json().find("\"state\":\"interrupted\"") != std::string::npos);
  lt_core_set_interrupted(b.core(), b.local_now(), 0);  // "Catch up"
  b.pump();
  w.sim.run_for(1500);
  CHECK_MSG(ref_error(w) <= 60, std::to_string(ref_error(w)));
}

TEST(leave_kick_and_end) {
  World w(26);
  setup(w, 4, sim::LinkParams{40, 40, 10});
  SimClient& a = *w.clients[0];
  a.insert(nullptr, 2, 600000);
  w.sim.run_for(1000);
  a.play();
  w.sim.run_for(3000);
  w.clients[1]->leave();
  CHECK(w.clients[1]->player.st == sim::FakePlayer::Idle);  // stops at once
  w.sim.run_for(2000);
  CHECK_EQ(w.clients[1]->conn_state(), std::string("closed"));
  CHECK(w.clients[1]->fatal_errors.empty());
  CHECK_EQ(static_cast<int>(S(a).view().participants.size()), 3);
  CHECK_EQ(w.clients[2]->kick("u0"), LT_ERR_STATE);  // only the creator can kick
  a.kick("u2");
  w.sim.run_for(2000);
  CHECK_EQ(S(*w.clients[2]).terminal_code(), std::string("kicked"));
  CHECK(w.clients[2]->player.st == sim::FakePlayer::Idle);
  a.end_session();
  w.sim.run_for(2000);
  CHECK_EQ(S(*w.clients[3]).terminal_code(), std::string("session_ended"));
  CHECK(w.clients[3]->player.st == sim::FakePlayer::Idle);
  CHECK(w.server.ended());
}

TEST(capi_chat_username_reaches_remote_players) {
  World w(27);
  setup(w, 2, sim::LinkParams{40, 40, 10});
  SimClient& a = *w.clients[0];
  SimClient& b = *w.clients[1];
  std::vector<std::string> pub = a.insert(nullptr, 1, 100000, "tunegram_music");  // public channel track
  std::vector<std::string> priv = a.insert(&pub[0], 1, 100000);                  // host-chat track, no username
  REQUIRE(pub.size() == 1 && priv.size() == 1);
  w.sim.run_for(1000);
  a.play();
  w.sim.run_for(3000);
  REQUIRE(!b.player.load_usernames.empty());
  CHECK_EQ(b.player.load_usernames.back(), std::string("tunegram_music"));
  CHECK(b.state_json().find("\"chat_username\":\"tunegram_music\"") != std::string::npos);
  lt::State st = w.server.replay();
  CHECK_EQ(lt::find_entry(st, pub[0])->chat_username, std::string("tunegram_music"));
  CHECK(lt::find_entry(st, priv[0])->chat_username.empty());
  b.next();
  w.sim.run_for(2000);
  CHECK_EQ(b.player.load_usernames.back(), std::string(""));
  // Invalid usernames are refused locally.
  lt_entry bad{nullptr, "-1001", 1, "2", "t", "p", 1000, "not a username!"};
  CHECK_EQ(lt_core_insert(a.core(), a.local_now(), nullptr, &bad, 1, nullptr), LT_ERR_INVALID);
  a.pump();
}

TEST(join_item_unavailable_on_one_device) {
  World w(28);
  setup(w, 3, sim::LinkParams{40, 40, 10});
  SimClient& a = *w.clients[0];
  SimClient& c = *w.clients[2];
  std::vector<std::string> ids = a.insert(nullptr, 2, 10000);
  c.player.fail_items.insert(ids[0]);  // c can't fetch the first message
  w.sim.run_for(1000);
  a.play();
  w.sim.run_for(4000);
  CHECK(std::find(c.errors.begin(), c.errors.end(), "item_unavailable") != c.errors.end());
  CHECK(c.fatal_errors.empty());
  // One attempt when the item became current (paused) and one retry when play
  // re-anchored it; then no retry loop while the item stays current.
  int loads = c.player.loads;
  CHECK(loads <= 2);
  CHECK_EQ(audible_on(w, ids[0]), 2);  // the others keep playing
  w.sim.run_for(3000);
  CHECK_EQ(c.player.loads, loads);
  w.sim.run_for(6000);  // auto-advance to the second item
  CHECK_EQ(w.reference(w.sim.now).item, ids[1]);
  CHECK_EQ(audible_on(w, ids[1]), 3);  // c rejoins at the next item, in sync
  CHECK_MSG(ref_error(w) <= 100, std::to_string(ref_error(w)));
}

namespace {
std::string participant_json(SimClient& viewer, const std::string& user) {
  lt::json::Value v;
  if (!lt::json::parse(viewer.state_json(), v)) return {};
  for (const auto& p : v.get("participants")->items()) {
    if (p.get_str("user_id") == user) return lt::json::dump(p);
  }
  return {};
}
}  // namespace

TEST(interrupt_other_audio_rejoins_automatically) {
  World w(29);
  setup(w, 3, sim::LinkParams{40, 40, 10});
  SimClient& a = *w.clients[0];
  SimClient& b = *w.clients[1];
  a.insert(nullptr, 2, 600000);
  w.sim.run_for(1000);
  a.play();
  w.sim.run_for(3000);
  REQUIRE(ref_error(w) <= 50);
  int loads_before = b.player.loads;

  // b plays a voice message: the platform's player is taken over.
  lt_core_interruption_began(b.core(), b.local_now(), LT_INTERRUPT_OTHER_AUDIO);
  b.pump();
  CHECK(b.player.st == sim::FakePlayer::Paused);
  b.player_stop();  // the voice message displaces the session item in the (single) player
  w.sim.run_for(500);
  std::string seen = participant_json(a, "u1");
  CHECK_MSG(seen.find("\"state\":\"interrupted\"") != std::string::npos, seen);
  CHECK_MSG(seen.find("\"reason\":\"other_audio\"") != std::string::npos, seen);
  CHECK(b.state_json().find("\"interruption\":{\"reason\":\"other_audio\",\"awaiting_catch_up\":false}") !=
        std::string::npos);
  w.sim.run_for(8000);
  CHECK(w.reference(w.sim.now).playing);  // nobody else paused
  CHECK(a.player.audible() && w.clients[2]->player.audible());

  // Voice message over: back to the live position with no tap.
  lt_core_interruption_ended(b.core(), b.local_now());
  b.pump();
  CHECK(b.player.loads > loads_before);  // the item is prepared again
  w.sim.run_for(1500);
  CHECK(b.player.audible());
  CHECK_MSG(ref_error(w) <= 100, std::to_string(ref_error(w)));
  CHECK(b.state_json().find("\"interruption\":null") != std::string::npos);
  w.sim.run_for(500);
  seen = participant_json(a, "u1");
  CHECK_MSG(seen.find("\"state\":\"active\"") != std::string::npos, seen);
  CHECK(seen.find("reason") == std::string::npos);
}

TEST(interrupt_call_waits_for_catch_up) {
  World w(30);
  setup(w, 2, sim::LinkParams{40, 40, 10});
  SimClient& a = *w.clients[0];
  SimClient& b = *w.clients[1];
  a.insert(nullptr, 1, 600000);
  w.sim.run_for(1000);
  a.play();
  w.sim.run_for(3000);
  lt_core_interruption_began(b.core(), b.local_now(), LT_INTERRUPT_CALL);
  b.pump();
  CHECK(b.player.st == sim::FakePlayer::Paused);
  w.sim.run_for(1000);
  CHECK(participant_json(a, "u1").find("\"reason\":\"call\"") != std::string::npos);
  // Other audio during the call keeps the call reason.
  lt_core_interruption_began(b.core(), b.local_now(), LT_INTERRUPT_OTHER_AUDIO);
  b.pump();
  CHECK_EQ(S(b).interrupt_reason(), static_cast<int>(lt::kInterruptCall));
  w.sim.run_for(5000);
  lt_core_interruption_ended(b.core(), b.local_now());  // call over
  b.pump();
  w.sim.run_for(3000);
  CHECK(b.player.st == sim::FakePlayer::Paused);  // waits for the user
  CHECK(S(b).awaiting_catch_up());
  CHECK(b.state_json().find("\"awaiting_catch_up\":true") != std::string::npos);
  CHECK(participant_json(a, "u1").find("\"state\":\"interrupted\"") != std::string::npos);
  lt_core_catch_up(b.core(), b.local_now());  // "Catch up"
  b.pump();
  w.sim.run_for(1500);
  CHECK(b.player.audible());
  CHECK_MSG(ref_error(w) <= 100, std::to_string(ref_error(w)));
  CHECK(participant_json(a, "u1").find("\"state\":\"active\"") != std::string::npos);
}

TEST(reconnect_after_track_ended_offline) {
  // The offline device's own track ends during the outage (it queues an
  // `advanced`), while someone else pressed next. On return it must land on the
  // live position of the item the others are playing.
  World w(31);
  setup(w, 3, sim::LinkParams{30, 30, 5});
  SimClient& a = *w.clients[0];
  SimClient& c = *w.clients[2];
  std::vector<std::string> ids = a.insert(nullptr, 3, 20000);
  w.sim.run_for(1000);
  a.play();
  w.sim.run_for(10000);
  w.set_net(2, false);
  w.sim.run_for(500);
  a.next();
  w.sim.run_for(11000);  // c's first track ends while it is offline
  CHECK(!c.player.ended_items.empty());
  w.set_net(2, true);
  for (int i = 0; i < 200 && !c.live(); ++i) w.sim.run_for(50);
  REQUIRE(c.live());
  // Re-aligned right after catch-up (not only by the slower 2-check correction).
  w.sim.run_for(700);
  CHECK_MSG(ref_error(w) <= 100, std::to_string(ref_error(w)));
  CHECK_EQ(S(c).stats().corrections, static_cast<uint64_t>(0));
  w.sim.run_for(5000);
  std::string why;
  CHECK_MSG(w.converged(&why), why);
  CHECK_MSG(ref_error(w) <= 100, std::to_string(ref_error(w)));
}
