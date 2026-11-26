#include "reducer.hpp"

#include <algorithm>

namespace lt {

namespace {

int find_index(const State& s, const std::string& id) {
  for (size_t i = 0; i < s.entries.size(); ++i) {
    if (s.entries[i].item_id == id) return static_cast<int>(i);
  }
  return -1;
}

int index_in(const std::vector<std::string>& v, const std::string& id) {
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i] == id) return static_cast<int>(i);
  }
  return -1;
}

int64_t duration_of(const State& s, const std::string& id) {
  const Entry* e = find_entry(s, id);
  return e ? e->duration_ms : 0;
}

std::string succ_cmd_in(const std::vector<std::string>& order, Repeat rep, const std::string& x) {
  int i = index_in(order, x);
  if (i < 0) return {};
  if (static_cast<size_t>(i) + 1 < order.size()) return order[static_cast<size_t>(i) + 1];
  if (rep != Repeat::Off && !order.empty()) return order[0];
  return {};
}

std::string succ_auto_in(const std::vector<std::string>& order, Repeat rep, const std::string& x) {
  if (rep == Repeat::One) return index_in(order, x) >= 0 ? x : std::string();
  return succ_cmd_in(order, rep, x);
}

struct Walk {
  std::string item;
  int64_t pos = 0;
  bool playing = false;
  std::string prev;      // item played just before `item` during this walk
  bool crossed = false;  // walked past at least one track end
  bool ended = false;    // ran off the end of the play order
};

Walk walk(const State& s, int64_t t, std::vector<std::string>* passed) {
  Walk w;
  w.item = s.cur;
  w.pos = s.anchor_pos;
  w.playing = s.playing;
  if (s.cur.empty() || !s.playing) return w;
  int64_t elapsed = t - s.anchor_time;
  if (elapsed <= 0) {
    w.pos = std::max<int64_t>(0, s.anchor_pos + elapsed);
    return w;
  }
  const std::vector<std::string> order = play_order(s);
  int64_t pos = s.anchor_pos + elapsed;
  std::string item = s.cur;
  size_t steps = 0;
  const size_t cycle_after = order.size() * 2 + 4;
  while (true) {
    int64_t d = duration_of(s, item);
    if (d <= 0 || pos < d) {
      w.item = item;
      w.pos = pos;
      return w;
    }
    std::string nxt = succ_auto_in(order, s.repeat, item);
    if (passed) passed->push_back(item);
    w.prev = item;
    w.crossed = true;
    pos -= d;
    if (nxt.empty()) {
      w.ended = true;
      w.playing = false;
      w.item = order.empty() ? std::string() : order[0];
      w.pos = 0;
      return w;
    }
    if (s.repeat == Repeat::One) {
      pos %= d;  // nxt == item: skip whole repetitions
    } else if (s.repeat == Repeat::All && ++steps > cycle_after) {
      int64_t cycle = 0;
      bool ok = true;
      for (const auto& id : order) {
        int64_t dd = duration_of(s, id);
        if (dd <= 0) { ok = false; break; }
        cycle += dd;
      }
      if (ok && cycle > 0) pos %= cycle;  // a full cycle from nxt returns to nxt
      steps = 0;
    }
    item = std::move(nxt);
  }
}

void push_history(State& s, const std::string& id) {
  if (id.empty()) return;
  if (!s.history.empty() && s.history.back() == id) return;
  s.history.push_back(id);
  if (s.history.size() > kMaxHistory) s.history.erase(s.history.begin());
}

void materialize(State& s, int64_t t) {
  if (s.cur.empty() || !s.playing || t <= s.anchor_time) return;
  std::vector<std::string> passed;
  Walk w = walk(s, t, &passed);
  if (!w.crossed) return;
  for (const auto& id : passed) push_history(s, id);
  s.cur = w.item;
  s.anchor_time = t;
  if (w.ended) {
    s.playing = false;
    s.anchor_pos = 0;
    s.anchor_prev.clear();
  } else {
    s.anchor_pos = w.pos;
    s.anchor_prev = w.prev;
  }
}

void set_anchor(State& s, const std::string& item, int64_t pos, int64_t t, bool push) {
  if (push && !s.cur.empty() && s.cur != item) push_history(s, s.cur);
  s.cur = item;
  int64_t d = duration_of(s, item);
  if (pos < 0) pos = 0;
  if (d > 0 && pos > d) pos = d;
  s.anchor_pos = pos;
  s.anchor_time = t;
  s.anchor_prev.clear();
  ++s.epoch;
}

void end_of_queue(State& s, int64_t t, bool push) {
  if (push && !s.cur.empty()) push_history(s, s.cur);
  std::vector<std::string> order = play_order(s);
  s.cur = order.empty() ? std::string() : order[0];
  s.playing = false;
  s.anchor_pos = 0;
  s.anchor_time = t;
  s.anchor_prev.clear();
  ++s.epoch;
}

void erase_all(std::vector<std::string>& v, const std::string& id) {
  v.erase(std::remove(v.begin(), v.end(), id), v.end());
}

}  // namespace

uint64_t splitmix64(uint64_t& state) {
  uint64_t z = (state += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

const Entry* find_entry(const State& s, const std::string& id) {
  int i = find_index(s, id);
  return i >= 0 ? &s.entries[static_cast<size_t>(i)] : nullptr;
}

const Entry* find_alive(const State& s, const std::string& id) {
  const Entry* e = find_entry(s, id);
  return e && e->alive ? e : nullptr;
}

size_t alive_count(const State& s) {
  size_t n = 0;
  for (const auto& e : s.entries) n += e.alive ? 1 : 0;
  return n;
}

std::vector<std::string> play_order(const State& s) {
  if (s.shuffle) return s.shuffle_order;
  std::vector<std::string> ids;
  ids.reserve(s.entries.size());
  for (const auto& e : s.entries) {
    if (e.alive) ids.push_back(e.item_id);
  }
  return ids;
}

std::string succ_cmd(const State& s, const std::string& id) { return succ_cmd_in(play_order(s), s.repeat, id); }

std::string succ_auto(const State& s, const std::string& id) { return succ_auto_in(play_order(s), s.repeat, id); }

Timeline timeline_at(const State& s, int64_t t) {
  Walk w = walk(s, t, nullptr);
  return Timeline{w.item, w.pos, w.playing};
}

void apply(State& s, const Op& op, bool optimistic) {
  int64_t ts = op.ts;
  if (optimistic) ts = op.has_at ? op.at : op.ts;
  if (!optimistic && op.rev > 0) s.rev = op.rev;

  int64_t eff = ts;
  if (is_playback_kind(op.kind)) {
    int64_t at = op.has_at ? op.at : ts;
    if (ts - at > kStalePlaybackMs) return;  // stale: typed during a long disconnect
    eff = std::min(at, ts);
  }

  // `advanced` needs the walk (and its boundary information) before
  // materialization folds it into the anchor.
  Walk pre;
  std::string pre_prev;
  if (op.kind == Kind::Advanced) {
    pre = walk(s, eff, nullptr);
    pre_prev = pre.crossed ? pre.prev : s.anchor_prev;
  }

  materialize(s, eff);

  switch (op.kind) {
    case Kind::Unknown:
      break;

    case Kind::Init:
      s.creator = op.by;
      s.host_chat_id = op.host_chat_id;
      s.title = op.title;
      break;

    case Kind::Join: {
      if (op.by.empty()) break;
      for (auto& p : s.participants) {
        if (p.user_id == op.by) {
          p.name = op.name;
          return;
        }
      }
      s.participants.push_back(Participant{op.by, op.name});
      break;
    }

    case Kind::Leave:
      s.participants.erase(std::remove_if(s.participants.begin(), s.participants.end(),
                                          [&](const Participant& p) { return p.user_id == op.by; }),
                           s.participants.end());
      break;

    case Kind::Kick:
      s.participants.erase(std::remove_if(s.participants.begin(), s.participants.end(),
                                          [&](const Participant& p) { return p.user_id == op.user_id; }),
                           s.participants.end());
      break;

    case Kind::End: {
      Timeline tl = timeline_at(s, eff);
      s.anchor_pos = tl.pos;
      s.anchor_time = eff;
      s.playing = false;
      s.ended = true;
      ++s.epoch;
      break;
    }

    case Kind::Insert: {
      std::vector<Entry> add;
      for (const auto& e : op.entries) {
        if (e.item_id.empty() || find_index(s, e.item_id) >= 0) continue;
        bool dup = false;
        for (const auto& a : add) dup = dup || a.item_id == e.item_id;
        if (dup) continue;
        if (s.entries.size() + add.size() >= kMaxEntries) break;
        Entry n = e;
        n.added_by = op.by;
        n.alive = true;
        if (n.duration_ms < 0) n.duration_ms = 0;
        add.push_back(std::move(n));
      }
      if (add.empty()) break;
      size_t at_base = s.entries.size();
      if (op.after_null) {
        at_base = 0;
      } else {
        int j = find_index(s, op.after);
        if (j >= 0) at_base = static_cast<size_t>(j) + 1;
      }
      if (s.shuffle) {
        size_t at_sh = s.shuffle_order.size();
        if (op.after_null) {
          at_sh = 0;
        } else {
          int j = index_in(s.shuffle_order, op.after);
          if (j >= 0) at_sh = static_cast<size_t>(j) + 1;
        }
        std::vector<std::string> ids;
        for (const auto& a : add) ids.push_back(a.item_id);
        s.shuffle_order.insert(s.shuffle_order.begin() + static_cast<std::ptrdiff_t>(at_sh), ids.begin(), ids.end());
      }
      s.entries.insert(s.entries.begin() + static_cast<std::ptrdiff_t>(at_base), add.begin(), add.end());
      if (s.cur.empty()) {
        std::vector<std::string> order = play_order(s);
        if (!order.empty()) {
          s.cur = order[0];
          s.playing = false;
          s.anchor_pos = 0;
          s.anchor_time = eff;
          s.anchor_prev.clear();
          ++s.epoch;
        }
      }
      break;
    }

    case Kind::Remove: {
      const std::vector<std::string> before = play_order(s);
      bool cur_removed = false;
      for (const auto& id : op.item_ids) {
        int j = find_index(s, id);
        if (j < 0 || !s.entries[static_cast<size_t>(j)].alive) continue;
        s.entries[static_cast<size_t>(j)].alive = false;
        erase_all(s.shuffle_order, id);
        erase_all(s.history, id);
        if (id == s.cur) cur_removed = true;
      }
      if (cur_removed) {
        int i = index_in(before, s.cur);
        size_t n = before.size();
        std::string nxt;
        for (size_t k = 1; i >= 0 && k < n; ++k) {
          size_t j = static_cast<size_t>(i) + k;
          if (j >= n) {
            if (s.repeat == Repeat::Off) break;
            j -= n;
          }
          if (find_alive(s, before[j])) {
            nxt = before[j];
            break;
          }
        }
        if (!nxt.empty()) set_anchor(s, nxt, 0, eff, false);
        else end_of_queue(s, eff, false);
      }
      break;
    }

    case Kind::Move: {
      int j = find_index(s, op.item_id);
      if (j < 0 || !s.entries[static_cast<size_t>(j)].alive) break;
      if (!op.after_null && op.after == op.item_id) break;
      if (!s.shuffle) {
        Entry e = std::move(s.entries[static_cast<size_t>(j)]);
        s.entries.erase(s.entries.begin() + j);
        size_t at = s.entries.size();
        if (op.after_null) {
          at = 0;
        } else {
          int k = find_index(s, op.after);
          if (k >= 0) at = static_cast<size_t>(k) + 1;
        }
        s.entries.insert(s.entries.begin() + static_cast<std::ptrdiff_t>(at), std::move(e));
      } else {
        erase_all(s.shuffle_order, op.item_id);
        size_t at = s.shuffle_order.size();
        if (op.after_null) {
          at = 0;
        } else {
          int k = index_in(s.shuffle_order, op.after);
          if (k >= 0) at = static_cast<size_t>(k) + 1;
        }
        s.shuffle_order.insert(s.shuffle_order.begin() + static_cast<std::ptrdiff_t>(at), op.item_id);
      }
      break;
    }

    case Kind::Play:
    case Kind::Pause: {
      std::string target;
      int64_t p = 0;
      if (op.has_item && find_alive(s, op.item_id)) {
        target = op.item_id;
        p = op.pos;
      } else if (!s.cur.empty()) {
        target = s.cur;
        p = timeline_at(s, eff).pos;
      } else {
        break;
      }
      if (op.kind == Kind::Play) {
        int64_t d = duration_of(s, target);
        if (d > 0 && p >= d) p = 0;
      }
      set_anchor(s, target, p, eff, true);
      s.playing = op.kind == Kind::Play;
      break;
    }

    case Kind::Seek:
      if (!op.has_item || !find_alive(s, op.item_id)) break;
      set_anchor(s, op.item_id, op.pos, eff, true);
      break;

    case Kind::Jump:
      if (!find_alive(s, op.item_id)) break;
      set_anchor(s, op.item_id, 0, eff, true);
      s.playing = true;
      break;

    case Kind::Next: {
      if (s.cur.empty() || s.cur != op.from_item) break;
      std::string nxt = succ_cmd(s, s.cur);
      if (nxt.empty()) end_of_queue(s, eff, true);
      else set_anchor(s, nxt, 0, eff, true);
      break;
    }

    case Kind::Previous: {
      if (s.cur.empty() || s.cur != op.from_item) break;
      if (op.pos > kPreviousRestartMs) {
        set_anchor(s, s.cur, 0, eff, false);
        break;
      }
      bool done = false;
      while (!s.history.empty()) {
        std::string h = s.history.back();
        s.history.pop_back();
        if (h != s.cur && find_alive(s, h)) {
          set_anchor(s, h, 0, eff, false);
          done = true;
          break;
        }
      }
      if (done) break;
      std::vector<std::string> order = play_order(s);
      int i = index_in(order, s.cur);
      if (i > 0) set_anchor(s, order[static_cast<size_t>(i) - 1], 0, eff, false);
      else if (i == 0 && s.repeat == Repeat::All && order.size() > 1) set_anchor(s, order.back(), 0, eff, false);
      else set_anchor(s, s.cur, 0, eff, false);
      break;
    }

    case Kind::Advanced: {
      if (!s.playing) break;
      const std::string& c = pre.item;
      int64_t p = pre.pos;
      int64_t d = duration_of(s, c);
      bool real_end = op.from_item == c && (d <= 0 || p >= d - kAdvanceWindowMs);
      bool crossed = !pre_prev.empty() && op.from_item == pre_prev && p <= kAdvanceWindowMs;
      if (real_end) {
        std::string nxt = succ_auto(s, c);
        if (nxt.empty()) {
          end_of_queue(s, eff, true);
        } else {
          set_anchor(s, nxt, 0, eff, true);
          s.playing = true;
        }
      } else if (crossed) {
        set_anchor(s, c, 0, eff, true);
        s.playing = true;
      }
      break;
    }

    case Kind::SetRepeat:
      s.repeat = op.mode;
      break;

    case Kind::SetShuffle:
      if (op.on) {
        s.shuffle = true;
        s.seed = op.seed;
        std::vector<std::string> rest;
        for (const auto& e : s.entries) {
          if (e.alive && e.item_id != s.cur) rest.push_back(e.item_id);
        }
        uint64_t st = op.seed;
        for (size_t i = rest.size(); i-- > 1;) {
          size_t j = static_cast<size_t>(splitmix64(st) % (i + 1));
          std::swap(rest[i], rest[j]);
        }
        s.shuffle_order.clear();
        if (!s.cur.empty()) s.shuffle_order.push_back(s.cur);
        s.shuffle_order.insert(s.shuffle_order.end(), rest.begin(), rest.end());
      } else {
        s.shuffle = false;
        s.shuffle_order.clear();
      }
      break;
  }
}

}  // namespace lt
