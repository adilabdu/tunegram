// Reducer unit tests: deterministic conflict resolution and timeline rules.
#include <string>
#include <vector>

#include "framework.hpp"
#include "protocol.hpp"
#include "reducer.hpp"

namespace {

struct R {
  lt::State s;
  int64_t rev = 0;

  void ap(lt::Op op, int64_t ts) {
    op.rev = ++rev;
    op.ts = ts;
    if (op.op_id.empty()) op.op_id = "o" + std::to_string(rev);
    if (op.by.empty()) op.by = "u1";
    lt::apply(s, op);
  }
  std::vector<std::string> order() const { return lt::play_order(s); }
  std::string digest() const { return lt::state_digest(s); }
};

lt::Entry entry(const std::string& id, int64_t dur = 10000) {
  lt::Entry e;
  e.item_id = id;
  e.chat_id = "-1001";
  e.msg_id = 1;
  e.doc_id = "42";
  e.title = id;
  e.duration_ms = dur;
  return e;
}

lt::Op ins(const char* after, std::vector<std::string> ids, int64_t dur = 10000) {
  lt::Op op;
  op.kind = lt::Kind::Insert;
  op.after_null = after == nullptr;
  if (after) op.after = after;
  for (auto& id : ids) op.entries.push_back(entry(id, dur));
  return op;
}

lt::Op rm(std::vector<std::string> ids) {
  lt::Op op;
  op.kind = lt::Kind::Remove;
  op.item_ids = std::move(ids);
  return op;
}

lt::Op mv(const std::string& item, const char* after) {
  lt::Op op;
  op.kind = lt::Kind::Move;
  op.item_id = item;
  op.has_item = true;
  op.after_null = after == nullptr;
  if (after) op.after = after;
  return op;
}

lt::Op pb(lt::Kind k, const std::string& item, int64_t pos, int64_t at) {
  lt::Op op;
  op.kind = k;
  op.has_item = !item.empty();
  op.item_id = item;
  op.from_item = item;
  op.pos = pos;
  op.has_at = true;
  op.at = at;
  return op;
}

lt::Op adv(const std::string& from, int64_t at) {
  lt::Op op;
  op.kind = lt::Kind::Advanced;
  op.from_item = from;
  op.has_at = true;
  op.at = at;
  return op;
}

using V = std::vector<std::string>;

}  // namespace

TEST(reducer_queue_edits_and_missing_anchors) {
  R r;
  r.ap(ins(nullptr, {"a", "b", "c"}), 1000);
  CHECK(r.order() == (V{"a", "b", "c"}));
  CHECK_EQ(r.s.cur, std::string("a"));  // first insert selects a current item, paused
  CHECK(!r.s.playing);
  CHECK_EQ(r.s.entries[0].added_by, std::string("u1"));

  r.ap(rm({"b"}), 1001);
  std::string after_first = r.digest();
  r.ap(rm({"b"}), 1002);  // removing a removed item: no-op
  CHECK_EQ(r.digest(), after_first);
  CHECK(r.order() == (V{"a", "c"}));

  r.ap(ins("b", {"x"}), 1003);  // anchor is a tombstone: lands where b was
  CHECK(r.order() == (V{"a", "x", "c"}));
  r.ap(ins("nope", {"y"}), 1004);  // unknown anchor: end
  CHECK(r.order() == (V{"a", "x", "c", "y"}));
  r.ap(mv("a", "zz"), 1005);  // move after unknown anchor: end
  CHECK(r.order() == (V{"x", "c", "y", "a"}));
  r.ap(mv("y", nullptr), 1006);  // move to head
  CHECK(r.order() == (V{"y", "x", "c", "a"}));
  r.ap(mv("c", "b"), 1007);  // move after tombstone b (which sits before x)
  CHECK(r.order() == (V{"y", "c", "x", "a"}));
  std::string d = r.digest();
  r.ap(mv("b", "y"), 1008);  // moving a removed item: no-op
  r.ap(mv("c", "c"), 1009);  // after itself: no-op
  r.ap(ins(nullptr, {"x", "a"}), 1010);  // existing ids are skipped
  CHECK_EQ(r.digest(), d);
}

TEST(reducer_remove_current_moves_to_successor) {
  R r;
  r.ap(ins(nullptr, {"a", "b", "c"}), 1000);
  r.ap(pb(lt::Kind::Jump, "b", 0, 2000), 2000);
  CHECK(r.s.playing);
  r.ap(rm({"b"}), 3000);
  CHECK_EQ(r.s.cur, std::string("c"));
  CHECK(r.s.playing);
  CHECK_EQ(r.s.anchor_pos, 0);
  r.ap(rm({"c"}), 4000);  // last item removed, repeat off: end of queue
  CHECK_EQ(r.s.cur, std::string("a"));
  CHECK(!r.s.playing);
  r.ap(rm({"a"}), 5000);
  CHECK(r.s.cur.empty());
  CHECK(lt::timeline_at(r.s, 9000).item.empty());
}

TEST(reducer_timeline_auto_advance_and_repeat) {
  R r;
  r.ap(ins(nullptr, {"a", "b", "c"}, 10000), 1000);
  r.ap(pb(lt::Kind::Play, "a", 0, 100000), 100000);
  lt::Timeline t = lt::timeline_at(r.s, 100000 + 10500);
  CHECK_EQ(t.item, std::string("b"));
  CHECK_EQ(t.pos, 500);
  t = lt::timeline_at(r.s, 100000 + 30000);  // past the end, repeat off
  CHECK_EQ(t.item, std::string("a"));
  CHECK(!t.playing);
  CHECK_EQ(t.pos, 0);

  lt::Op rep;
  rep.kind = lt::Kind::SetRepeat;
  rep.mode = lt::Repeat::All;
  r.ap(rep, 100001);
  t = lt::timeline_at(r.s, 100000 + 30000 + 2500);  // wrapped
  CHECK_EQ(t.item, std::string("a"));
  CHECK(t.playing);
  CHECK_EQ(t.pos, 2500);
  t = lt::timeline_at(r.s, 100000 + 3000000 * 10 + 12345);  // long walk is bounded
  CHECK_EQ(t.item, std::string("b"));
  CHECK_EQ(t.pos, 2345);

  rep.mode = lt::Repeat::One;
  r.ap(rep, 100002);
  t = lt::timeline_at(r.s, 100000 + 10000 * 7 + 1);
  CHECK_EQ(t.item, std::string("a"));
  CHECK_EQ(t.pos, 1);
}

TEST(reducer_materialization_keeps_past_stable) {
  R r;
  r.ap(ins(nullptr, {"a", "b", "c"}, 10000), 1000);
  r.ap(pb(lt::Kind::Play, "a", 0, 100000), 100000);
  // At 112000 b is playing (pos 2000). A "play next after a" insert must not
  // rewrite that past: b keeps playing and x waits.
  r.ap(ins("a", {"x"}), 112000);
  CHECK_EQ(r.s.cur, std::string("b"));
  CHECK_EQ(r.s.anchor_prev, std::string("a"));
  lt::Timeline t = lt::timeline_at(r.s, 113000);
  CHECK_EQ(t.item, std::string("b"));
  CHECK_EQ(t.pos, 3000);
  CHECK(r.s.history == (V{"a"}));
}

TEST(reducer_stale_playback_ops_are_noops) {
  R r;
  r.ap(ins(nullptr, {"a", "b"}, 100000), 1000);
  r.ap(pb(lt::Kind::Play, "a", 0, 10000), 10000);
  std::string d = r.digest();
  r.ap(pb(lt::Kind::Pause, "a", 3000, 20000), 26000);  // 6 s old
  CHECK_EQ(r.digest(), d);
  r.ap(pb(lt::Kind::Pause, "a", 13000, 23000), 26000);  // 3 s old: applies at `at`
  CHECK(!r.s.playing);
  CHECK_EQ(r.s.anchor_pos, 13000);
  CHECK_EQ(r.s.anchor_time, 23000);
  r.ap(pb(lt::Kind::Seek, "a", 50000, 30500), 30000);  // `at` in the future: clamped to ts
  CHECK_EQ(r.s.anchor_time, 30000);
}

TEST(navigation_concurrent_next_skips_once) {
  R r;
  r.ap(ins(nullptr, {"a", "b", "c"}, 100000), 1000);
  r.ap(pb(lt::Kind::Play, "a", 0, 10000), 10000);
  r.ap(pb(lt::Kind::Next, "a", 0, 20000), 20050);
  r.ap(pb(lt::Kind::Next, "a", 0, 20010), 20060);  // same intent from another device
  CHECK_EQ(r.s.cur, std::string("b"));
  CHECK(r.s.playing);
  r.ap(pb(lt::Kind::Next, "b", 0, 21000), 21000);
  r.ap(pb(lt::Kind::Next, "c", 0, 22000), 22000);  // next on the last item, repeat off
  CHECK_EQ(r.s.cur, std::string("a"));
  CHECK(!r.s.playing);
}

TEST(navigation_previous_restart_or_back) {
  R r;
  r.ap(ins(nullptr, {"a", "b", "c"}, 100000), 1000);
  r.ap(pb(lt::Kind::Play, "a", 0, 10000), 10000);
  r.ap(pb(lt::Kind::Next, "a", 0, 11000), 11000);
  r.ap(pb(lt::Kind::Next, "b", 0, 12000), 12000);
  CHECK_EQ(r.s.cur, std::string("c"));
  // Past 3 s: restart the track.
  r.ap(pb(lt::Kind::Previous, "c", 4500, 16500), 16500);
  CHECK_EQ(r.s.cur, std::string("c"));
  CHECK_EQ(r.s.anchor_pos, 0);
  CHECK_EQ(r.s.anchor_time, 16500);
  // Within 3 s: previous history entry (b), then a.
  r.ap(pb(lt::Kind::Previous, "c", 1200, 17700), 17700);
  CHECK_EQ(r.s.cur, std::string("b"));
  r.ap(pb(lt::Kind::Previous, "b", 100, 17800), 17800);
  CHECK_EQ(r.s.cur, std::string("a"));
  // History exhausted at the first item: restart.
  r.ap(pb(lt::Kind::Previous, "a", 100, 17900), 17900);
  CHECK_EQ(r.s.cur, std::string("a"));
  CHECK(r.s.playing);
  // Jump to c with empty history, previous -> play-order predecessor b.
  r.s.history.clear();
  r.ap(pb(lt::Kind::Jump, "c", 0, 18000), 18000);
  r.s.history.clear();
  r.ap(pb(lt::Kind::Previous, "c", 0, 18100), 18100);
  CHECK_EQ(r.s.cur, std::string("b"));
  // Stale `from_item` (someone else already moved on): no-op.
  std::string d = r.digest();
  r.ap(pb(lt::Kind::Previous, "c", 0, 18200), 18200);
  CHECK_EQ(r.digest(), d);
}

TEST(advance_first_wins_and_window) {
  R r;
  r.ap(ins(nullptr, {"a", "b", "c"}, 10000), 1000);
  r.ap(pb(lt::Kind::Play, "a", 0, 100000), 100000);
  uint64_t e0 = r.s.epoch;
  // Real end 50 ms after the derived boundary (audio slightly longer).
  r.ap(adv("a", 110050), 110080);
  CHECK_EQ(r.s.cur, std::string("b"));
  CHECK_EQ(r.s.anchor_time, 110050);
  CHECK_EQ(r.s.anchor_pos, 0);
  CHECK_EQ(r.s.epoch, e0 + 1);
  std::string d = r.digest();
  r.ap(adv("a", 110070), 110100);  // duplicate from another device
  CHECK_EQ(r.digest(), d);
  // Real end 2 s before the metadata end: accepted (window 3 s).
  r.ap(adv("b", 118050), 118100);
  CHECK_EQ(r.s.cur, std::string("c"));
  // 5 s early: rejected (protects against bogus early ends).
  d = r.digest();
  r.ap(adv("c", 123050), 123100);
  CHECK_EQ(r.digest(), d);
  // End of the queue (repeat off): paused at the first item.
  r.ap(adv("c", 128050), 128100);
  CHECK_EQ(r.s.cur, std::string("a"));
  CHECK(!r.s.playing);
}

TEST(advance_after_materialized_boundary) {
  R r;
  r.ap(ins(nullptr, {"a", "b", "c"}, 10000), 1000);
  r.ap(pb(lt::Kind::Play, "a", 0, 100000), 100000);
  r.ap(ins(nullptr, {"z"}), 110500);  // an edit materializes the a->b boundary first
  CHECK_EQ(r.s.cur, std::string("b"));
  r.ap(adv("a", 110700), 110750);  // still accepted via anchor_prev: re-anchors b at the real end
  CHECK_EQ(r.s.cur, std::string("b"));
  CHECK_EQ(r.s.anchor_time, 110700);
  CHECK_EQ(r.s.anchor_pos, 0);
}

TEST(shuffle_order_is_seeded_and_stable) {
  auto build = [](uint32_t seed) {
    R r;
    r.ap(ins(nullptr, {"a", "b", "c", "d", "e", "f", "g", "h"}), 1000);
    r.ap(pb(lt::Kind::Jump, "c", 0, 2000), 2000);
    lt::Op sh;
    sh.kind = lt::Kind::SetShuffle;
    sh.on = true;
    sh.seed = seed;
    r.ap(sh, 3000);
    return r;
  };
  R r1 = build(12345), r2 = build(12345), r3 = build(999);
  CHECK(r1.order() == r2.order());
  CHECK(r1.order() != r3.order());
  CHECK_EQ(r1.order()[0], std::string("c"));  // current item first
  CHECK_EQ(r1.order().size(), static_cast<size_t>(8));
  // Insert while shuffled goes after the anchor in the play order.
  std::string second = r1.order()[1];
  r1.ap(ins("c", {"n"}), 4000);
  CHECK_EQ(r1.order()[1], std::string("n"));
  CHECK_EQ(r1.order()[2], second);
  // Move acts on the shuffled order only.
  r1.ap(mv("n", nullptr), 4100);
  CHECK_EQ(r1.order()[0], std::string("n"));
  lt::Op off;
  off.kind = lt::Kind::SetShuffle;
  off.on = false;
  r1.ap(off, 5000);
  CHECK(r1.order() == (V{"a", "b", "c", "n", "d", "e", "f", "g", "h"}));
}
