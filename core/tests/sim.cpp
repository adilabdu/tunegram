#include "sim.hpp"

#include <algorithm>
#include <cmath>

#include "internal.hpp"
#include "protocol.hpp"

namespace sim {

using lt::json::Value;

namespace {
constexpr int64_t kEpoch = 1764000000000;  // sim starts at a realistic Unix ms time

bool is_client_kind(lt::Kind k) { return k != lt::Kind::Unknown && k != lt::Kind::Init && k != lt::Kind::Join; }
}  // namespace

// ---------------------------------------------------------------------------
// Sim

void Sim::at(int64_t t, std::function<void()> fn) {
  if (t < now) t = now;
  q_.push(Task{t, seq_++, std::move(fn)});
}

void Sim::run_until(int64_t t) {
  while (!q_.empty() && q_.top().t <= t) {
    Task task = q_.top();
    q_.pop();
    now = task.t;
    ++steps;
    task.fn();
  }
  if (now < t) now = t;
}

// ---------------------------------------------------------------------------
// SimServer

void SimServer::send(int conn, const Value& v) {
  if (hooks_.send) hooks_.send(conn, lt::json::dump(v));
}

void SimServer::broadcast(const Value& v, int except) {
  std::string s = lt::json::dump(v);
  std::vector<int> targets;
  for (const auto& [c, sock] : socks_) {
    if (sock.authed && c != except) targets.push_back(c);
  }
  for (int c : targets) hooks_.send(c, s);
}

void SimServer::fatal(int conn, const std::string& code, int close_code, const std::string& msg) {
  Value e = Value::object();
  e.set("type", "error");
  e.set("code", code);
  e.set("message", msg.empty() ? code : msg);
  e.set("fatal", true);
  if (code == "version_unsupported") {
    e.set("min_v", min_v);
    e.set("max_v", max_v);
  }
  send(conn, e);
  socks_.erase(conn);
  if (hooks_.close) hooks_.close(conn, close_code);
}

void SimServer::create(const std::string& creator_user, const std::string& host_chat_id, const std::string& title) {
  creator = creator_user;
  members.insert(creator_user);
  Value op = Value::object();
  op.set("kind", "init");
  op.set("host_chat_id", host_chat_id);
  op.set("title", title);
  append(op, creator_user);
}

int64_t SimServer::append(Value op, const std::string& by) {
  int64_t rev = head() + 1;
  Value rec = Value::object();
  rec.set("rev", rev);
  rec.set("ts", sim_.now);
  rec.set("by", by);
  for (auto& m : op.members()) {
    if (m.first == "type" || m.first == "rev" || m.first == "ts" || m.first == "by" || m.first == "seed") continue;
    rec.members().push_back(m);
  }
  std::string op_id = rec.get_str("op_id");
  if (op_id.empty()) {
    op_id = "s:" + std::to_string(rev);
    rec.set("op_id", op_id);
  }
  if (rec.get_str("kind") == "set_shuffle" && rec.get_bool("on")) {
    rec.set("seed", static_cast<int64_t>(rng_.next() & 0xFFFFFFFFull));
  }
  log_.push_back(lt::json::dump(rec));
  op_ids_[op_id] = rev;
  Value ev = Value::object();
  ev.set("type", "event");
  ev.set("server_time", sim_.now);
  ev.set("op", std::move(rec));
  broadcast(ev);
  return rev;
}

void SimServer::send_log(int conn, int64_t since) {
  if (since < 0 || since > head()) since = 0;
  int64_t from = since;
  do {
    int64_t to = std::min(head(), from + 500);
    std::string s = "{\"type\":\"log\",\"since_rev\":" + std::to_string(from) + ",\"head_rev\":" +
                    std::to_string(head()) + ",\"server_time\":" + std::to_string(sim_.now) +
                    ",\"more\":" + (to < head() ? "true" : "false") + ",\"ops\":[";
    for (int64_t r = from + 1; r <= to; ++r) {
      if (r > from + 1) s.push_back(',');
      s += log_[static_cast<size_t>(r - 1)];
    }
    s += "]}";
    hooks_.send(conn, s);
    from = to;
  } while (from < head());
}

bool SimServer::user_online(const std::string& user, int except) const {
  for (const auto& [c, s] : socks_) {
    if (c != except && s.authed && s.user == user) return true;
  }
  return false;
}

void SimServer::presence(const Sock& s, bool online) {
  Value p = Value::object();
  p.set("type", "presence");
  p.set("user_id", s.user);
  p.set("device_id", s.device);
  p.set("online", online);
  p.set("state", s.state);
  if (s.state == "interrupted" && !s.reason.empty()) p.set("reason", s.reason);
  p.set("server_time", sim_.now);
  broadcast(p);
}

void SimServer::close_user(const std::string& user, const std::string& code, int close_code) {
  std::vector<int> targets;
  for (const auto& [c, s] : socks_) {
    if (s.authed && s.user == user) targets.push_back(c);
  }
  for (int c : targets) fatal(c, code, close_code);
}

void SimServer::on_open(int conn) { socks_[conn] = Sock{}; }

void SimServer::on_close(int conn) {
  auto it = socks_.find(conn);
  if (it == socks_.end()) return;
  Sock s = it->second;
  socks_.erase(it);
  if (s.authed && !user_online(s.user, -1)) presence(s, false);
}

void SimServer::on_message(int conn, const std::string& data) {
  auto it = socks_.find(conn);
  if (it == socks_.end()) return;
  if (data.size() > 65536) {
    Value e = Value::object();
    e.set("type", "error");
    e.set("code", "too_large");
    e.set("fatal", false);
    send(conn, e);
    return;
  }
  Value m;
  if (!lt::json::parse(data, m) || !m.is_object()) {
    Value e = Value::object();
    e.set("type", "error");
    e.set("code", "bad_request");
    e.set("fatal", false);
    send(conn, e);
    return;
  }
  const std::string type = m.get_str("type");
  Sock& s = it->second;
  if (!s.authed) {
    if (type != "hello") return fatal(conn, "unauthorized", 4001, "hello first");
    int64_t v = m.get_int("v");
    if (v < min_v || v > max_v) return fatal(conn, "version_unsupported", 4009, "Please update the app");
    const std::string token = m.get_str("token");
    if (token.rfind("u:", 0) != 0) return fatal(conn, "unauthorized", 4001);
    size_t colon = token.find(':', 2);
    std::string user = token.substr(2, colon == std::string::npos ? std::string::npos : colon - 2);
    std::string name = colon == std::string::npos ? std::string() : token.substr(colon + 1);
    if (user.empty()) return fatal(conn, "unauthorized", 4001);
    if (ended_) return fatal(conn, "session_ended", 4011);
    if (kicked.count(user)) return fatal(conn, "kicked", 4012);
    if (!allow_all_members && !members.count(user)) return fatal(conn, "not_member", 4003);
    bool participant = std::find(participants_.begin(), participants_.end(), user) != participants_.end();
    if (!participant) {
      if (static_cast<int>(participants_.size()) >= max_participants) return fatal(conn, "session_full", 4010);
      participants_.push_back(user);
      Value op = Value::object();
      op.set("kind", "join");
      op.set("name", name);
      append(op, user);
    }
    bool was_online = user_online(user, conn);
    s.authed = true;
    s.user = user;
    s.name = name;
    s.device = m.get_str("device_id");
    Value w = Value::object();
    w.set("type", "welcome");
    w.set("v", v);
    w.set("session_id", "sim");
    w.set("user_id", user);
    w.set("creator", creator);
    w.set("head_rev", head());
    w.set("server_time", sim_.now);
    if (const Value* t0 = m.get("t0"); t0 && t0->is_number()) w.set("t0", t0->as_int());
    w.set("t1", sim_.now);
    w.set("t2", sim_.now);
    w.set("max_participants", max_participants);
    Value online = Value::array();
    for (const auto& [c, o] : socks_) {
      if (c == conn || !o.authed) continue;
      Value p = Value::object();
      p.set("user_id", o.user);
      p.set("device_id", o.device);
      p.set("state", o.state);
      if (o.state == "interrupted" && !o.reason.empty()) p.set("reason", o.reason);
      online.push(std::move(p));
    }
    w.set("online", std::move(online));
    send(conn, w);
    send_log(conn, m.get_int("since_rev"));
    if (!was_online) presence(s, true);
    return;
  }

  if (type == "ping") {
    Value p = Value::object();
    p.set("type", "pong");
    p.set("t0", m.get_int("t0"));
    p.set("t1", sim_.now);
    p.set("t2", sim_.now);
    p.set("head_rev", head());
    send(conn, p);
  } else if (type == "sync") {
    send_log(conn, m.get_int("since_rev"));
  } else if (type == "status") {
    s.state = m.get_str("state") == "interrupted" ? "interrupted" : "active";
    std::string reason = m.get_str("reason");
    s.reason = s.state == "interrupted" && (reason == "call" || reason == "other_audio") ? reason : "";
    presence(s, true);
  } else if (type == "op") {
    const std::string op_id = m.get_str("op_id");
    auto reject = [&](const char* code) {
      ++rejects;
      Value r = Value::object();
      r.set("type", "reject");
      r.set("op_id", op_id);
      r.set("code", code);
      r.set("message", code);
      send(conn, r);
    };
    if (op_id.empty() || op_id.size() > 64) return reject("bad_request");
    if (auto d = op_ids_.find(op_id); d != op_ids_.end()) {
      ++dup_acks;
      Value a = Value::object();
      a.set("type", "ack");
      a.set("op_id", op_id);
      a.set("rev", d->second);
      a.set("dup", true);
      send(conn, a);
      return;
    }
    lt::Kind k = lt::kind_from_name(m.get_str("kind"));
    if (k == lt::Kind::Unknown) return reject("bad_request");
    if (!is_client_kind(k)) return reject("forbidden");
    if (lt::is_playback_kind(k)) {
      const Value* at = m.get("at");
      if (!at || !at->is_number()) return reject("bad_request");
    }
    if ((k == lt::Kind::Kick || k == lt::Kind::End) && s.user != creator) return reject("forbidden");
    std::string by = s.user;
    std::string target = m.get_str("user_id");
    append(m, by);
    if (k == lt::Kind::Leave) {
      participants_.erase(std::remove(participants_.begin(), participants_.end(), by), participants_.end());
      close_user(by, "left", 4013);
      if (participants_.empty()) {
        ended_ = true;
        Value op = Value::object();
        op.set("kind", "end");
        append(op, "");
      }
    } else if (k == lt::Kind::Kick) {
      participants_.erase(std::remove(participants_.begin(), participants_.end(), target), participants_.end());
      kicked.insert(target);
      close_user(target, "kicked", 4012);
    } else if (k == lt::Kind::End) {
      ended_ = true;
      std::vector<int> all;
      for (const auto& [c, so] : socks_) all.push_back(c);
      for (int c : all) fatal(c, "session_ended", 4011);
    }
  } else {
    Value e = Value::object();
    e.set("type", "error");
    e.set("code", "bad_request");
    e.set("fatal", false);
    send(conn, e);
  }
}

std::vector<lt::Op> SimServer::parsed_log() const {
  std::vector<lt::Op> ops;
  for (const auto& r : log_) {
    Value v;
    lt::Op op;
    if (lt::json::parse(r, v) && lt::op_from_json(v, op)) ops.push_back(std::move(op));
  }
  return ops;
}

lt::State SimServer::replay() const {
  lt::State s;
  for (const auto& op : parsed_log()) lt::apply(s, op);
  return s;
}

// ---------------------------------------------------------------------------
// FakePlayer

int64_t FakePlayer::actual_dur() const {
  auto it = extra_ms.find(item);
  return meta_dur + (it == extra_ms.end() ? 0 : it->second);
}

int64_t FakePlayer::pos_at(int64_t t) const {
  if (st == Playing) return pos_base + static_cast<int64_t>(std::llround(static_cast<double>(t - t_base) * rate));
  return pos_base;
}

// ---------------------------------------------------------------------------
// SimClient

SimClient::SimClient(World& world, int index, std::string u, std::string n, uint64_t seed, int protocol_version)
    : w(world), idx(index), user(std::move(u)), name(std::move(n)), seed_(seed), protocol_version_(protocol_version),
      rng_(seed) {
  device = "dev" + std::to_string(idx) + "x" + user;
  clock_offset = 100000 + static_cast<int64_t>(rng_.next() % 1000000000ull);  // device uptime-ish
  drift_ppm = static_cast<double>(static_cast<int64_t>(rng_.next() % 101) - 50);
  std::string token = "u:" + user + ":" + name;
  lt_config cfg{device.c_str(), token.c_str(), protocol_version_, seed_};
  core_ = lt_core_create(&cfg);
}

SimClient::~SimClient() { destroy_core(); }

void SimClient::destroy_core() {
  if (core_) lt_core_destroy(core_);
  core_ = nullptr;
  ++timer_gen_;
  player.gen++;
}

int64_t SimClient::local(int64_t t) const {
  int64_t rel = t - kEpoch;
  return rel + clock_offset + static_cast<int64_t>(static_cast<double>(rel) * drift_ppm * 1e-6);
}

int64_t SimClient::local_now() const { return local(w.sim.now); }

int64_t SimClient::to_true(int64_t deadline) const {
  double guess = static_cast<double>(deadline - clock_offset) / (1.0 + drift_ppm * 1e-6);
  int64_t g = kEpoch + static_cast<int64_t>(guess) - 3;
  while (local(g) < deadline) ++g;
  return g;
}

void SimClient::start() {
  lt_core_start(core_, local_now());
  pump();
}

void SimClient::pump() {
  if (!core_) return;
  lt_event ev;
  while (lt_core_poll(core_, &ev)) {
    if (on_event) on_event(ev);
    switch (ev.type) {
      case LT_EV_SEND:
        w.send_up(idx, std::string(ev.data, ev.len));
        break;
      case LT_EV_CONNECT:
        w.connect(idx);
        break;
      case LT_EV_DISCONNECT:
        w.client_close(idx);
        break;
      case LT_EV_PLAYER_LOAD:
        player.load_usernames.emplace_back(ev.chat_username ? ev.chat_username : "");
        player_load(ev.item_id, ev.duration_ms);
        break;
      case LT_EV_PLAYER_PLAY:
        player_play(ev.item_id, ev.pos_ms, ev.ref_ms);
        break;
      case LT_EV_PLAYER_PAUSE:
        player_pause(ev.item_id, ev.pos_ms);
        break;
      case LT_EV_PLAYER_SEEK:
        player_seek(ev.item_id, ev.pos_ms, ev.ref_ms);
        break;
      case LT_EV_PLAYER_STOP:
        player_stop();
        break;
      case LT_EV_PLAYER_PRELOAD:
        player.cached.insert(ev.item_id);
        break;
      case LT_EV_PLAYER_QUERY: {
        uint64_t g = timer_gen_;
        (void)g;
        w.sim.after(0, [this] { player_report(); });
        break;
      }
      case LT_EV_STATE_CHANGED:
        ++state_changes;
        break;
      case LT_EV_ERROR:
        errors.emplace_back(ev.code);
        if (ev.fatal) fatal_errors.emplace_back(ev.code);
        break;
      default:
        break;
    }
  }
  ++timer_gen_;
  int64_t t = lt_core_next_timeout(core_);
  if (t >= 0) {
    uint64_t g = timer_gen_;
    int64_t tt = std::max(w.sim.now, to_true(t));
    w.sim.at(tt, [this, g] {
      if (g != timer_gen_ || !core_) return;
      lt_core_tick(core_, local_now());
      pump();
    });
  }
}

void SimClient::player_load(const std::string& item, int64_t dur) {
  FakePlayer& p = player;
  if (p.st == FakePlayer::Playing && !p.item.empty()) {
    int64_t left = p.actual_dur() - p.pos_at(w.sim.now);
    if (left > 0 && left < 1500) p.cut_items.push_back(p.item);
  }
  p.gen++;
  p.loads++;
  p.st = FakePlayer::Loading;
  p.item = item;
  p.meta_dur = dur;
  p.pos_base = 0;
  int64_t delay = p.cached.count(item) ? p.cached_load_ms : p.load_ms + rng_.range(0, p.load_jitter_ms);
  uint64_t g = p.gen;
  w.sim.after(delay, [this, g, item] {
    if (!core_ || player.gen != g) return;
    if (player.fail_items.count(item)) {
      player.st = FakePlayer::Idle;
      lt_core_player_failed(core_, local_now(), item.c_str());
    } else {
      player.st = FakePlayer::Ready;
      lt_core_player_ready(core_, local_now(), item.c_str());
    }
    pump();
  });
}

void SimClient::player_play(const std::string& item, int64_t pos, int64_t ref) {
  FakePlayer& p = player;
  if (item != p.item) return;
  p.gen++;
  p.plays++;
  p.st = FakePlayer::Starting;
  p.pos_base = pos;
  uint64_t g = p.gen;
  w.sim.after(p.start_ms, [this, g, item, pos, ref] {
    if (!core_ || player.gen != g) return;
    int64_t L = local_now();
    int64_t start = player.extrapolate ? pos + (L - ref) : pos;
    if (start < 0) start = 0;
    player.st = FakePlayer::Playing;
    player.pos_base = start;
    player.t_base = w.sim.now;
    lt::Timeline ref_tl = w.reference(w.sim.now);
    if (ref_tl.item == item && ref_tl.playing) {
      player.last_start_error = std::llabs(start - ref_tl.pos);
      player.has_start_error = true;
    }
    schedule_end();
    lt_core_player_started(core_, L, item.c_str(), start);
    pump();
  });
}

void SimClient::player_pause(const std::string& item, int64_t pos) {
  FakePlayer& p = player;
  if (item != p.item) return;
  p.pauses++;
  if (p.st == FakePlayer::Loading) {
    p.pos_base = pos;
    return;
  }
  p.gen++;
  p.st = FakePlayer::Paused;
  p.pos_base = pos;
}

void SimClient::player_seek(const std::string& item, int64_t pos, int64_t ref) {
  FakePlayer& p = player;
  if (item != p.item) return;
  p.seeks++;
  if (p.st == FakePlayer::Playing || p.st == FakePlayer::Starting) {
    p.gen++;
    p.st = FakePlayer::Starting;
    p.pos_base = pos;
    uint64_t g = p.gen;
    w.sim.after(p.seek_ms, [this, g, pos, ref] {
      if (!core_ || player.gen != g) return;
      int64_t start = player.extrapolate ? pos + (local_now() - ref) : pos;
      player.st = FakePlayer::Playing;
      player.pos_base = std::max<int64_t>(0, start);
      player.t_base = w.sim.now;
      schedule_end();
    });
  } else {
    p.pos_base = pos;
  }
}

void SimClient::player_stop() {
  player.gen++;
  player.stops++;
  player.st = FakePlayer::Idle;
  player.item.clear();
}

void SimClient::player_report() {
  if (!core_) return;
  FakePlayer& p = player;
  p.queries++;
  int64_t L = local_now();
  if (p.st == FakePlayer::Playing) {
    int64_t noise = p.report_noise_ms ? rng_.range(-p.report_noise_ms, p.report_noise_ms) : 0;
    lt_core_player_position(core_, L, p.item.c_str(), p.pos_at(w.sim.now) + noise, 1);
  } else if (p.st == FakePlayer::Starting || p.st == FakePlayer::Paused) {
    lt_core_player_position(core_, L, p.item.c_str(), p.pos_base, 0);
  } else {
    return;
  }
  pump();
}

void SimClient::schedule_end() {
  FakePlayer& p = player;
  if (p.st != FakePlayer::Playing || p.actual_dur() <= 0) return;
  int64_t remaining = std::max<int64_t>(0, p.actual_dur() - p.pos_base);
  int64_t dt = static_cast<int64_t>(std::ceil(static_cast<double>(remaining) / p.rate));
  uint64_t g = p.gen;
  w.sim.after(dt, [this, g] {
    if (!core_ || player.gen != g) return;
    player.st = FakePlayer::Ended;
    player.ends++;
    player.ended_items.push_back(player.item);
    player.pos_base = player.actual_dur();
    lt_core_player_ended(core_, local_now(), player.item.c_str());
    pump();
  });
}

void SimClient::glitch(int64_t delta_ms) {
  FakePlayer& p = player;
  if (p.st != FakePlayer::Playing) return;
  p.pos_base = p.pos_at(w.sim.now) + delta_ms;
  p.t_base = w.sim.now;
  p.gen++;
  schedule_end();
}

int SimClient::play() { int r = lt_core_play(core_, local_now()); pump(); return r; }
int SimClient::pause() { int r = lt_core_pause(core_, local_now()); pump(); return r; }
int SimClient::seek(int64_t pos) { int r = lt_core_seek(core_, local_now(), pos); pump(); return r; }
int SimClient::next() { int r = lt_core_next(core_, local_now()); pump(); return r; }
int SimClient::previous() { int r = lt_core_previous(core_, local_now()); pump(); return r; }
int SimClient::jump(const std::string& item) { int r = lt_core_jump(core_, local_now(), item.c_str()); pump(); return r; }
int SimClient::set_repeat(int mode) { int r = lt_core_set_repeat(core_, local_now(), mode); pump(); return r; }
int SimClient::set_shuffle(bool on) { int r = lt_core_set_shuffle(core_, local_now(), on ? 1 : 0); pump(); return r; }
int SimClient::leave() { int r = lt_core_leave(core_, local_now()); pump(); return r; }
int SimClient::kick(const std::string& u) { int r = lt_core_kick(core_, local_now(), u.c_str()); pump(); return r; }
int SimClient::end_session() { int r = lt_core_end_session(core_, local_now()); pump(); return r; }

int SimClient::remove(const std::string& item) {
  const char* ids[1] = {item.c_str()};
  int r = lt_core_remove(core_, local_now(), ids, 1);
  pump();
  return r;
}

int SimClient::move(const std::string& item, const std::string* after) {
  int r = lt_core_move(core_, local_now(), item.c_str(), after ? after->c_str() : nullptr);
  pump();
  return r;
}

std::vector<std::string> SimClient::insert(const std::string* after, int n, int64_t duration_ms,
                                           const char* chat_username) {
  std::vector<std::string> titles, docs, chats;
  std::vector<lt_entry> es(static_cast<size_t>(n));
  titles.reserve(static_cast<size_t>(n));
  docs.reserve(static_cast<size_t>(n));
  std::string chat = "-100" + std::to_string(1000 + idx);
  for (int i = 0; i < n; ++i) {
    ++entry_counter_;
    titles.push_back("T" + std::to_string(idx) + "." + std::to_string(entry_counter_));
    docs.push_back(std::to_string(5000000000000000000ull + static_cast<uint64_t>(idx) * 100000 +
                                  static_cast<uint64_t>(entry_counter_)));
  }
  for (int i = 0; i < n; ++i) {
    lt_entry& e = es[static_cast<size_t>(i)];
    e.item_id = nullptr;
    e.chat_id = chat.c_str();
    e.msg_id = entry_counter_ - n + i + 1;
    e.doc_id = docs[static_cast<size_t>(i)].c_str();
    e.title = titles[static_cast<size_t>(i)].c_str();
    e.performer = "P";
    e.duration_ms = duration_ms;
    e.chat_username = chat_username;
  }
  const char* ids_json = nullptr;
  int r = lt_core_insert(core_, local_now(), after ? after->c_str() : nullptr, es.data(), es.size(), &ids_json);
  std::vector<std::string> ids;
  if (r == LT_OK && ids_json) {
    Value v;
    if (lt::json::parse(ids_json, v)) {
      for (const auto& x : v.items()) ids.push_back(x.as_str());
    }
  }
  pump();
  return ids;
}

std::string SimClient::state_json() { return lt_core_state_json(core_, local_now()); }

lt_playback SimClient::playback() { return lt_core_playback(core_, local_now()); }

bool SimClient::live() { return lt::session_of(core_)->conn_state() == lt::ConnState::Live; }

std::string SimClient::conn_state() { return lt::conn_state_name(lt::session_of(core_)->conn_state()); }

size_t SimClient::pending() { return lt::session_of(core_)->pending_count(); }

std::vector<std::string> SimClient::queue_ids() { return lt::play_order(lt::session_of(core_)->view()); }

// ---------------------------------------------------------------------------
// World

namespace {
uint64_t g_world_uid = 0;
}

World::World(uint64_t seed) : rng(seed ^ 0xABCDEF12345ull), server(sim, seed * 31 + 7), uid(++g_world_uid) {
  sim.now = kEpoch;
  server.set_hooks(SimServer::Hooks{[this](int c, const std::string& d) { send_down(c, d); },
                                    [this](int c, int code) { server_close(c, code); }});
}

World::~World() {
  for (auto& c : clients) c->destroy_core();
}

SimClient& World::add_client(const std::string& user, const std::string& name, int protocol_version) {
  int idx = static_cast<int>(clients.size());
  clients.push_back(std::make_unique<SimClient>(*this, idx, user, name, rng.next(), protocol_version));
  return *clients.back();
}

int64_t World::latency(const LinkParams& l, bool up) { return (up ? l.up_ms : l.down_ms) + rng.range(0, l.jitter_ms); }

void World::connect(int ci) {
  SimClient& c = *clients[static_cast<size_t>(ci)];
  ++c.connects;
  if (c.conn >= 0) client_close(ci);
  int64_t delay = c.link.up_ms + c.link.down_ms + rng.range(0, c.link.jitter_ms);
  int attempt = c.connects;
  if (!c.net_up) {
    sim.after(std::max<int64_t>(delay, 1000), [this, ci, attempt] {
      SimClient& cc = *clients[static_cast<size_t>(ci)];
      if (!cc.core() || cc.connects != attempt || cc.conn >= 0) return;
      lt_core_on_close(cc.core(), cc.local_now(), 1006);
      cc.pump();
    });
    return;
  }
  int id = next_conn++;
  conns[id] = Conn{ci, false, false, 0, 0};
  c.conn = id;
  sim.after(delay, [this, id, ci] {
    SimClient& cc = *clients[static_cast<size_t>(ci)];
    auto it = conns.find(id);
    if (it == conns.end() || cc.conn != id || !cc.core()) return;
    if (!cc.net_up) {
      cc.conn = -1;
      lt_core_on_close(cc.core(), cc.local_now(), 1006);
      cc.pump();
      return;
    }
    it->second.open = true;
    it->second.server_alive = true;
    it->second.last_up = it->second.last_down = sim.now;
    server.on_open(id);
    lt_core_on_open(cc.core(), cc.local_now());
    cc.pump();
  });
}

void World::client_close(int ci) {
  SimClient& c = *clients[static_cast<size_t>(ci)];
  int id = c.conn;
  if (id < 0) return;
  c.conn = -1;
  auto it = conns.find(id);
  if (it == conns.end()) return;
  it->second.open = false;
  if (it->second.server_alive) {
    sim.after(latency(c.link, true), [this, id] {
      auto it2 = conns.find(id);
      if (it2 != conns.end() && it2->second.server_alive) {
        it2->second.server_alive = false;
        server.on_close(id);
      }
    });
  }
}

void World::send_up(int ci, const std::string& data) {
  SimClient& c = *clients[static_cast<size_t>(ci)];
  int id = c.conn;
  if (id < 0) return;
  auto it = conns.find(id);
  if (it == conns.end() || !it->second.open) return;
  int copies = 1 + (rng.chance(c.link.dup) ? 1 : 0);
  for (int k = 0; k < copies; ++k) {
    if (rng.chance(c.link.loss)) continue;
    int64_t t = sim.now + latency(c.link, true);
    if (!c.link.reorder) {
      t = std::max(t, it->second.last_up);
      it->second.last_up = t;
    }
    sim.at(t, [this, id, data] {
      auto it2 = conns.find(id);
      if (it2 == conns.end() || !it2->second.server_alive) return;
      server.on_message(id, data);
    });
  }
}

void World::send_down(int conn, const std::string& data) {
  auto it = conns.find(conn);
  if (it == conns.end()) return;
  int ci = it->second.client;
  SimClient& c = *clients[static_cast<size_t>(ci)];
  if (c.block_down_until > sim.now) return;
  int copies = 1 + (rng.chance(c.link.dup) ? 1 : 0);
  for (int k = 0; k < copies; ++k) {
    if (rng.chance(c.link.loss)) continue;
    int64_t t = sim.now + latency(c.link, false);
    if (!c.link.reorder) {
      t = std::max(t, it->second.last_down);
      it->second.last_down = t;
    }
    sim.at(t, [this, conn, ci, data] {
      SimClient& cc = *clients[static_cast<size_t>(ci)];
      auto it2 = conns.find(conn);
      if (cc.conn != conn || it2 == conns.end() || !it2->second.open || !cc.core()) return;
      lt_core_on_message(cc.core(), cc.local_now(), data.data(), data.size());
      cc.pump();
    });
  }
}

void World::server_close(int conn, int code) {
  auto it = conns.find(conn);
  if (it == conns.end()) return;
  it->second.server_alive = false;
  int ci = it->second.client;
  SimClient& c = *clients[static_cast<size_t>(ci)];
  int64_t t = std::max(sim.now + latency(c.link, false), it->second.last_down + 1);
  it->second.last_down = t;
  sim.at(t, [this, conn, ci, code] {
    SimClient& cc = *clients[static_cast<size_t>(ci)];
    if (cc.conn != conn || !cc.core()) return;
    cc.conn = -1;
    conns[conn].open = false;
    lt_core_on_close(cc.core(), cc.local_now(), code);
    cc.pump();
  });
}

void World::set_net(int ci, bool up) {
  SimClient& c = *clients[static_cast<size_t>(ci)];
  c.net_up = up;
  if (up || c.conn < 0) return;
  int id = c.conn;
  c.conn = -1;
  conns[id].open = false;
  sim.after(1000, [this, id] {
    auto it = conns.find(id);
    if (it != conns.end() && it->second.server_alive) {
      it->second.server_alive = false;
      server.on_close(id);
    }
  });
  int attempt = c.connects;
  sim.after(1500, [this, ci, attempt] {
    SimClient& cc = *clients[static_cast<size_t>(ci)];
    if (!cc.core() || cc.connects != attempt) return;
    lt_core_on_close(cc.core(), cc.local_now(), 1006);
    cc.pump();
  });
}

lt::Timeline World::reference(int64_t t) const {
  static thread_local uint64_t cached_world = 0;
  static thread_local int64_t cached_head = -1;
  static thread_local lt::State cached;
  if (cached_world != uid || cached_head != server.head()) {
    cached = server.replay();
    cached_world = uid;
    cached_head = server.head();
  }
  return lt::timeline_at(cached, t);
}

void World::heal() {
  for (auto& c : clients) {
    c->link.loss = 0;
    c->link.dup = 0;
    if (!c->net_up) set_net(c->idx, true);
  }
}

bool World::converged(std::string* why) {
  std::string ref = lt::state_digest(server.replay());
  for (auto& c : clients) {
    if (!c->core()) continue;
    lt::Session* s = lt::session_of(c->core());
    if (s->conn_state() == lt::ConnState::Terminal) continue;
    auto fail = [&](const std::string& m) {
      if (why) *why = "client " + std::to_string(c->idx) + ": " + m;
      return false;
    };
    if (s->conn_state() != lt::ConnState::Live) return fail(std::string("not live: ") + lt::conn_state_name(s->conn_state()));
    if (s->pending_count() != 0) return fail("pending: " + s->debug_pending());
    if (s->confirmed_rev() != server.head()) {
      return fail("rev " + std::to_string(s->confirmed_rev()) + " != head " + std::to_string(server.head()));
    }
    if (lt::state_digest(s->confirmed()) != ref) return fail("confirmed state differs from server replay");
    if (lt::state_digest(s->view()) != ref) return fail("view differs from server replay");
  }
  return true;
}

}  // namespace sim
