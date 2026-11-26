// Property-style convergence tests: many random seeds, concurrent random edits
// from several clients over a hostile network, then heal and require every
// client to hold exactly the state obtained by replaying the server log.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "framework.hpp"
#include "internal.hpp"
#include "protocol.hpp"
#include "sim.hpp"

using sim::SimClient;
using sim::World;

namespace {

struct Profile {
  const char* name;
  int min_clients, max_clients;
  int64_t max_latency;
  int64_t max_jitter;
  double max_loss;
  double max_dup;
  double flap_rate;  // probability per action of a link flap
  int64_t activity_ms;
};

struct Totals {
  uint64_t ops = 0;
  uint64_t records = 0;
  uint64_t flaps = 0;
  uint64_t sim_ms = 0;
  uint64_t steps = 0;
  int failures = 0;
};

bool check_invariants(World& w, const std::vector<std::string>& generated, std::string* why) {
  lt::State st = w.server.replay();
  std::set<std::string> seen;
  for (const auto& e : st.entries) {
    if (!seen.insert(e.item_id).second) {
      *why = "duplicate item id " + e.item_id;
      return false;
    }
  }
  // Every insert the server accepted is present exactly once (alive or tombstone).
  std::set<std::string> logged;
  for (const auto& op : w.server.parsed_log()) {
    if (op.kind == lt::Kind::Insert) {
      for (const auto& e : op.entries) logged.insert(e.item_id);
    }
  }
  for (const auto& id : logged) {
    if (!seen.count(id)) {
      *why = "lost item " + id;
      return false;
    }
  }
  for (const auto& id : seen) {
    if (std::find(generated.begin(), generated.end(), id) == generated.end()) {
      *why = "unknown item " + id;
      return false;
    }
  }
  if (!st.cur.empty() && !lt::find_alive(st, st.cur)) {
    *why = "current item is not alive";
    return false;
  }
  if (st.shuffle) {
    std::vector<std::string> a = st.shuffle_order, b;
    for (const auto& e : st.entries) {
      if (e.alive) b.push_back(e.item_id);
    }
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    if (a != b) {
      *why = "shuffle order is not a permutation of the live queue";
      return false;
    }
  }
  return true;
}

void run_seed(uint64_t seed, const Profile& p, Totals& tot) {
  World w(seed);
  sim::Rng rng(seed * 7919 + 17);
  int n = static_cast<int>(rng.range(p.min_clients, p.max_clients));
  w.server.create("u0", "-1001");
  for (int i = 0; i < n; ++i) {
    SimClient& c = w.add_client("u" + std::to_string(i), "N" + std::to_string(i));
    c.link.up_ms = rng.range(5, p.max_latency);
    c.link.down_ms = rng.range(5, p.max_latency);
    c.link.jitter_ms = rng.range(0, p.max_jitter);
    c.link.loss = p.max_loss * rng.unit();
    c.link.dup = p.max_dup * rng.unit();
    c.link.reorder = rng.chance(0.5);
    c.player.load_ms = rng.range(20, 600);
  }
  for (auto& c : w.clients) c->start();
  w.sim.run_for(2000);

  std::vector<std::string> generated;
  auto add = [&](const std::vector<std::string>& ids) { generated.insert(generated.end(), ids.begin(), ids.end()); };
  add(w.clients[0]->insert(nullptr, 5, rng.range(20000, 90000)));

  int64_t end = w.sim.now + p.activity_ms;
  while (w.sim.now < end) {
    w.sim.run_for(rng.range(30, 600));
    SimClient& c = *w.clients[static_cast<size_t>(rng.range(0, n - 1))];
    if (!c.core()) continue;
    auto any_id = [&]() -> std::string {
      if (generated.empty() || rng.chance(0.08)) return "missing-" + std::to_string(rng.next() % 100);
      return rng.pick(generated);
    };
    int roll = static_cast<int>(rng.range(0, 99));
    ++tot.ops;
    if (roll < 33) {
      std::string after = any_id();
      add(c.insert(rng.chance(0.15) ? nullptr : &after, static_cast<int>(rng.range(1, 3)), rng.range(4000, 90000)));
    } else if (roll < 52) {
      c.remove(any_id());
    } else if (roll < 70) {
      std::string after = any_id();
      c.move(any_id(), rng.chance(0.15) ? nullptr : &after);
    } else if (roll < 90) {
      switch (rng.range(0, 6)) {
        case 0: c.play(); break;
        case 1: c.pause(); break;
        case 2: c.seek(rng.range(0, 60000)); break;
        case 3: c.next(); break;
        case 4: c.previous(); break;
        case 5: c.jump(any_id()); break;
        default: c.play(); break;
      }
    } else if (roll < 94) {
      c.set_shuffle(rng.chance(0.6));
    } else if (roll < 97) {
      c.set_repeat(static_cast<int>(rng.range(0, 2)));
    } else {
      --tot.ops;
    }
    if (rng.chance(p.flap_rate)) {
      int ci = static_cast<int>(rng.range(0, n - 1));
      if (w.clients[static_cast<size_t>(ci)]->net_up) {
        ++tot.flaps;
        w.set_net(ci, false);
        w.sim.after(rng.range(300, 8000), [&w, ci] { w.set_net(ci, true); });
      }
    }
  }
  w.heal();
  // Wait for everyone to be back, stop playback (so automatic advance stops
  // generating ops), then settle.
  for (int i = 0; i < 80; ++i) {
    bool all_live = true;
    for (auto& c : w.clients) all_live = all_live && c->live();
    if (all_live) break;
    w.sim.run_for(500);
  }
  w.sim.run_for(2000);
  w.clients[0]->pause();
  w.sim.run_for(25000);

  std::string why;
  bool ok = w.converged(&why) && check_invariants(w, generated, &why);
  if (!ok && tf::verbose()) {
    for (auto& c : w.clients) std::fprintf(stderr, "    client %d local_now=%lld conn=%s\n", c->idx, static_cast<long long>(c->local_now()), c->conn_state().c_str());
    const auto& log = w.server.log();
    for (size_t i = log.size() > 12 ? log.size() - 12 : 0; i < log.size(); ++i) std::fprintf(stderr, "    %s\n", log[i].c_str());
  }
  if (!ok) {
    ++tot.failures;
    if (tot.failures <= 5) std::fprintf(stderr, "  [%s] seed %llu: %s\n", p.name, static_cast<unsigned long long>(seed), why.c_str());
  }
  tot.records += static_cast<uint64_t>(w.server.head());
  tot.steps += w.sim.steps;
}

void run_profile(const Profile& p, int seeds, uint64_t seed_base) {
  Totals tot;
  auto t0 = std::chrono::steady_clock::now();
  int only = tf::env_int("LT_ONLY_SEED", 0);
  for (int s = 1; s <= seeds; ++s) {
    uint64_t seed = seed_base + static_cast<uint64_t>(s);
    if (only && static_cast<int>(seed) != only) continue;
    run_seed(seed, p, tot);
  }
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::printf("     convergence[%s]: %d seeds (%llu..%llu), %llu ops, %llu log records, %llu link flaps, "
              "%llu sim events, %d failure(s), %.0f ms\n",
              p.name, seeds, static_cast<unsigned long long>(seed_base + 1),
              static_cast<unsigned long long>(seed_base + static_cast<uint64_t>(seeds)),
              static_cast<unsigned long long>(tot.ops), static_cast<unsigned long long>(tot.records),
              static_cast<unsigned long long>(tot.flaps), static_cast<unsigned long long>(tot.steps), tot.failures, ms);
  CHECK_EQ(tot.failures, 0);
}

}  // namespace

TEST(convergence_two_clients) {
  Profile p{"2 clients", 2, 2, 200, 80, 0.02, 0.02, 0.02, 40000};
  run_profile(p, tf::env_int("LT_SEEDS", 200), 1000);
}

TEST(convergence_many_clients_hostile_network) {
  Profile p{"2-6 clients, hostile", 2, 6, 300, 150, 0.08, 0.05, 0.05, 60000};
  run_profile(p, tf::env_int("LT_SEEDS", 200), 5000);
}

TEST(convergence_ten_clients) {
  Profile p{"10 clients", 10, 10, 150, 60, 0.03, 0.02, 0.03, 40000};
  run_profile(p, std::max(1, tf::env_int("LT_SEEDS", 200) / 5), 9000);
}
