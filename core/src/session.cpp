#include "session.hpp"

#include <algorithm>

#include "protocol.hpp"

namespace lt {

namespace {

constexpr int64_t kConnectTimeoutMs = 10000;
constexpr int64_t kHandshakeTimeoutMs = 8000;
constexpr int64_t kPingBurstGapMs = 150;
constexpr int kPingBurst = 5;
constexpr int64_t kPingIntervalMs = 15000;
constexpr int64_t kResendAfterMs = 5000;
constexpr int64_t kGapWaitMs = 1000;
constexpr int64_t kSyncRepeatMs = 2000;
constexpr int64_t kLeaveTimeoutMs = 3000;
constexpr size_t kMaxPending = 256;
constexpr size_t kMaxBuffered = 4096;
constexpr size_t kMaxBatch = 100;

int64_t iabs(int64_t v) { return v < 0 ? -v : v; }

uint64_t fnv1a(std::string_view s) {
  uint64_t h = 1469598103934665603ull;
  for (char c : s) {
    h ^= static_cast<unsigned char>(c);
    h *= 1099511628211ull;
  }
  return h;
}

std::string base36(uint64_t v) {
  static const char* digits = "0123456789abcdefghijklmnopqrstuvwxyz";
  char buf[16];
  int n = 0;
  do {
    buf[n++] = digits[v % 36];
    v /= 36;
  } while (v);
  std::string s;
  while (n) s.push_back(buf[--n]);
  return s;
}

bool id_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

bool terminal_error(const std::string& code) {
  return code == "version_unsupported" || code == "session_full" || code == "session_ended" || code == "kicked" ||
         code == "not_member" || code == "not_found" || code == "left";
}

}  // namespace

const char* interrupt_reason_name(int reason) {
  switch (reason) {
    case kInterruptCall: return "call";
    case kInterruptOtherAudio: return "other_audio";
    default: return "";
  }
}

const char* conn_state_name(ConnState s) {
  switch (s) {
    case ConnState::Idle: return "idle";
    case ConnState::Connecting: return "connecting";
    case ConnState::Handshaking: return "handshaking";
    case ConnState::Live: return "live";
    case ConnState::Backoff: return "reconnecting";
    case ConnState::AuthRequired: return "auth_required";
    case ConnState::Terminal: return "closed";
  }
  return "?";
}

Session::Session(Config cfg) : cfg_(std::move(cfg)) {
  for (char c : cfg_.device_id) {
    if (tag_.size() >= 8) break;
    if (id_char(c)) tag_.push_back(c);
  }
  if (tag_.empty()) tag_ = "d";
  rng_ = cfg_.seed ^ fnv1a(cfg_.device_id) ^ 0x5DEECE66Dull;
  if (cfg_.protocol_version <= 0) cfg_.protocol_version = kProtocolVersion;
  if (cfg_.correction_checks < 1) cfg_.correction_checks = 1;
  if (cfg_.check_interval_ms < 100) cfg_.check_interval_ms = 100;
}

std::string Session::new_id(char sep) {
  std::string s = tag_;
  s.push_back(sep);
  s += base36(++counter_);
  s.push_back(sep);
  s += base36(splitmix64(rng_) & 0xFFFFFFFFull);
  return s;
}

// ---------------------------------------------------------------------------
// Connection

void Session::send(std::string data) {
  Event ev;
  ev.type = EvType::Send;
  ev.data = std::move(data);
  emit(std::move(ev));
}

void Session::emit_error(const std::string& code, bool fatal, const std::string& message) {
  Event ev;
  ev.type = EvType::Error;
  ev.code = code;
  ev.fatal = fatal;
  ev.data = message;
  emit(std::move(ev));
}

void Session::start(int64_t now) {
  if (conn_ == ConnState::Idle) begin_connect(now);
  after_input(now);
}

void Session::begin_connect(int64_t now) {
  conn_ = ConnState::Connecting;
  conn_deadline_ = now + kConnectTimeoutMs;
  Event ev;
  ev.type = EvType::Connect;
  emit(std::move(ev));
  ui_dirty_ = true;
}

void Session::mark_pending_unsent() {
  for (auto& p : pending_) p.sent_at = -1;
}

void Session::schedule_reconnect(int64_t now) {
  conn_ = ConnState::Backoff;
  int shift = attempt_ < 5 ? attempt_ : 5;
  int64_t base = std::min<int64_t>(8000, int64_t{250} << shift);
  int64_t jitter_pct = 75 + static_cast<int64_t>(splitmix64(rng_) % 51);  // 75..125 %
  conn_deadline_ = now + base * jitter_pct / 100;
  ++attempt_;
  ++stats_.reconnects;
  log_in_progress_ = false;
  mark_pending_unsent();
  ui_dirty_ = true;
}

void Session::go_terminal(const std::string& code, bool emit_err, const std::string& message) {
  if (conn_ == ConnState::Terminal) return;
  conn_ = ConnState::Terminal;
  terminal_code_ = code;
  conn_deadline_ = -1;
  next_ping_ = -1;
  if (emit_err) emit_error(code, true, message);
  ui_dirty_ = true;
}

void Session::handle_close_code(int64_t now, int code) {
  switch (code) {
    case 4001:
      conn_ = ConnState::AuthRequired;
      conn_deadline_ = -1;
      mark_pending_unsent();
      emit_error("unauthorized", false, "token rejected; supply a new token");
      ui_dirty_ = true;
      return;
    case 4003: go_terminal("not_member", true); return;
    case 4004: go_terminal("not_found", true); return;
    case 4009: go_terminal("version_unsupported", true, "Please update the app"); return;
    case 4010: go_terminal("session_full", true); return;
    case 4011: go_terminal("session_ended", true); return;
    case 4012: go_terminal("kicked", true); return;
    case 4013: go_terminal("left", !leaving_); return;
    default: schedule_reconnect(now); return;
  }
}

void Session::on_open(int64_t now) {
  if (conn_ == ConnState::Connecting) {
    conn_ = ConnState::Handshaking;
    conn_deadline_ = now + kHandshakeTimeoutMs;
    hello_t0_ = now;
    send(encode_hello(cfg_.protocol_version, cfg_.token, cfg_.device_id, confirmed_rev_, now));
    ui_dirty_ = true;
  }
  after_input(now);
}

void Session::on_close(int64_t now, int code) {
  if (conn_ == ConnState::Connecting || conn_ == ConnState::Handshaking || conn_ == ConnState::Live) {
    handle_close_code(now, code);
  }
  after_input(now);
}

void Session::set_token(int64_t now, std::string token) {
  cfg_.token = std::move(token);
  if (conn_ == ConnState::AuthRequired) {
    attempt_ = 0;
    begin_connect(now);
  }
  after_input(now);
}

void Session::on_welcome(int64_t now, const json::Value& m) {
  if (conn_ == ConnState::Handshaking) {
    conn_ = ConnState::Live;
    conn_deadline_ = -1;
    attempt_ = 0;
  }
  int64_t t0 = m.get_int("t0", -1);
  int64_t st = m.get_int("server_time");
  if (t0 == hello_t0_) clock_.add(t0, m.get_int("t1", st), m.get_int("t2", st), now);
  user_id_ = m.get_str("user_id");
  max_participants_ = m.get_int("max_participants");
  int64_t head = m.get_int("head_rev");
  if (head < confirmed_rev_) {
    // The server's log is not the one we folded (should not happen with random
    // session ids). Rebuild from scratch; the server sends the full log.
    confirmed_ = State{};
    confirmed_rev_ = 0;
    buffered_.clear();
    view_dirty_ = true;
  }
  known_head_ = head;
  online_.clear();
  if (const json::Value* on = m.get("online"); on && on->is_array()) {
    for (const auto& p : on->items()) {
      online_.push_back(
          Presence{p.get_str("user_id"), p.get_str("device_id"), p.get_str("state", "active"), p.get_str("reason")});
    }
  }
  burst_left_ = kPingBurst;
  next_ping_ = now + kPingBurstGapMs;
  for (auto& p : pending_) {
    if (p.op.by != user_id_) {
      p.op.by = user_id_;
      view_dirty_ = true;
    }
    send(encode_op_message(p.op));
    p.sent_at = now;
    ++stats_.resends;
  }
  if (interrupted_) send_status();
  ui_dirty_ = true;
}

void Session::on_message(int64_t now, std::string_view data) {
  if (conn_ != ConnState::Handshaking && conn_ != ConnState::Live) {
    after_input(now);
    return;
  }
  json::Value m;
  if (!json::parse(data, m) || !m.is_object()) {
    after_input(now);
    return;
  }
  const std::string type = m.get_str("type");
  if (type == "welcome") {
    on_welcome(now, m);
  } else if (type == "log") {
    if (const json::Value* ops = m.get("ops"); ops && ops->is_array()) {
      for (const auto& o : ops->items()) {
        Op rec;
        if (op_from_json(o, rec) && rec.rev > 0) ingest(rec);
      }
    }
    known_head_ = std::max(known_head_, m.get_int("head_rev"));
    log_in_progress_ = m.get_bool("more");
  } else if (type == "event") {
    Op rec;
    if (const json::Value* o = m.get("op"); o && op_from_json(*o, rec) && rec.rev > 0) ingest(rec);
  } else if (type == "pong") {
    clock_.add(m.get_int("t0"), m.get_int("t1"), m.get_int("t2"), now);
    known_head_ = std::max(known_head_, m.get_int("head_rev"));
  } else if (type == "ack") {
    const std::string op_id = m.get_str("op_id");
    int64_t rev = m.get_int("rev");
    known_head_ = std::max(known_head_, rev);
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
      if (it->op.op_id == op_id) {
        if (rev > 0 && rev <= confirmed_rev_) {
          pending_.erase(it);
          view_dirty_ = true;
        } else {
          it->acked_rev = rev;
        }
        break;
      }
    }
  } else if (type == "reject") {
    const std::string op_id = m.get_str("op_id");
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
      if (it->op.op_id == op_id) {
        pending_.erase(it);
        view_dirty_ = true;
        break;
      }
    }
    emit_error(m.get_str("code", "rejected"), false, m.get_str("message"));
  } else if (type == "presence") {
    const std::string uid = m.get_str("user_id");
    const std::string dev = m.get_str("device_id");
    online_.erase(std::remove_if(online_.begin(), online_.end(),
                                 [&](const Presence& p) { return p.user_id == uid && p.device_id == dev; }),
                  online_.end());
    if (m.get_bool("online")) online_.push_back(Presence{uid, dev, m.get_str("state", "active"), m.get_str("reason")});
    ui_dirty_ = true;
  } else if (type == "error") {
    const std::string code = m.get_str("code");
    const std::string msg = m.get_str("message");
    if (m.get_bool("fatal") && code == "left" && leaving_) {
      finish_leave();  // our own leave, confirmed by the server closing us
    } else if (m.get_bool("fatal")) {
      Event dc;
      dc.type = EvType::Disconnect;
      if (code == "unauthorized") {
        conn_ = ConnState::AuthRequired;
        conn_deadline_ = -1;
        mark_pending_unsent();
        emit_error(code, false, msg);
        ui_dirty_ = true;
      } else if (terminal_error(code)) {
        go_terminal(code, true, msg);
      } else {
        emit_error(code, true, msg);
        schedule_reconnect(now);
      }
      emit(std::move(dc));
    } else {
      emit_error(code, false, msg);
    }
  }
  after_input(now);
}

// ---------------------------------------------------------------------------
// Log handling

void Session::ingest(const Op& rec) {
  if (rec.rev <= confirmed_rev_) return;
  if (rec.rev > confirmed_rev_ + 1) {
    if (buffered_.size() < kMaxBuffered) buffered_.emplace(rec.rev, rec);
    known_head_ = std::max(known_head_, rec.rev);
    return;
  }
  apply_confirmed(rec);
  while (!buffered_.empty()) {
    auto it = buffered_.begin();
    if (it->first <= confirmed_rev_) {
      buffered_.erase(it);
      continue;
    }
    if (it->first != confirmed_rev_ + 1) break;
    Op next = std::move(it->second);
    buffered_.erase(it);
    apply_confirmed(next);
  }
}

void Session::apply_confirmed(const Op& rec) {
  apply(confirmed_, rec, false);
  confirmed_rev_ = rec.rev;
  known_head_ = std::max(known_head_, rec.rev);
  for (auto it = pending_.begin(); it != pending_.end(); ++it) {
    if (it->op.op_id == rec.op_id) {
      pending_.erase(it);
      break;
    }
  }
  pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                [&](const Pending& p) { return p.acked_rev > 0 && p.acked_rev <= confirmed_rev_; }),
                 pending_.end());
  view_dirty_ = true;
  if (leaving_ && rec.kind == Kind::Leave && rec.by == user_id_) finish_leave();
}

void Session::rebuild_view() {
  view_ = confirmed_;
  for (const auto& p : pending_) apply(view_, p.op, true);
  view_dirty_ = false;
  ui_dirty_ = true;
}

int Session::submit(int64_t now, Op op) {
  if (conn_ == ConnState::Terminal || leaving_) return kErrState;
  if (pending_.size() >= kMaxPending) return kErrFull;
  if (view_dirty_) rebuild_view();
  op.op_id = new_id(':');
  op.base_rev = confirmed_rev_;
  op.has_at = true;
  op.at = server_now(now);
  op.by = user_id_;
  apply(view_, op, true);
  Pending p;
  p.op = std::move(op);
  if (conn_ == ConnState::Live) {
    send(encode_op_message(p.op));
    p.sent_at = now;
    ++stats_.ops_sent;
  }
  pending_.push_back(std::move(p));
  ui_dirty_ = true;
  return kOk;
}

void Session::finish_leave() {
  if (conn_ == ConnState::Terminal) return;
  bool was_connected = conn_ == ConnState::Live || conn_ == ConnState::Handshaking || conn_ == ConnState::Connecting;
  go_terminal("left", false);
  leave_deadline_ = -1;
  if (was_connected) {
    Event ev;
    ev.type = EvType::Disconnect;
    emit(std::move(ev));
  }
}

// ---------------------------------------------------------------------------
// Commands

int64_t Session::local_pos(int64_t now, const Timeline& d) const {
  if (player_.item == d.item && (player_.st == PlayerModel::Playing || player_.st == PlayerModel::Paused)) {
    return std::max<int64_t>(0, player_est(now));
  }
  return d.pos;
}

int Session::cmd_play(int64_t now) {
  if (view_dirty_) rebuild_view();
  if (view_.ended) return kErrState;
  Timeline d = playback(now);
  if (d.item.empty()) return kErrNoop;
  if (d.playing) return kErrNoop;
  Op op;
  op.kind = Kind::Play;
  op.has_item = true;
  op.item_id = d.item;
  op.pos = local_pos(now, d);
  int r = submit(now, std::move(op));
  after_input(now);
  return r;
}

int Session::cmd_pause(int64_t now) {
  if (view_dirty_) rebuild_view();
  if (view_.ended) return kErrState;
  Timeline d = playback(now);
  if (d.item.empty() || !d.playing) return kErrNoop;
  Op op;
  op.kind = Kind::Pause;
  op.has_item = true;
  op.item_id = d.item;
  op.pos = local_pos(now, d);
  int r = submit(now, std::move(op));
  after_input(now);
  return r;
}

int Session::cmd_seek(int64_t now, int64_t pos) {
  if (view_dirty_) rebuild_view();
  if (view_.ended) return kErrState;
  if (pos < 0) return kErrInvalid;
  Timeline d = playback(now);
  if (d.item.empty()) return kErrNoop;
  Op op;
  op.kind = Kind::Seek;
  op.has_item = true;
  op.item_id = d.item;
  op.pos = pos;
  int r = submit(now, std::move(op));
  after_input(now);
  return r;
}

int Session::cmd_next(int64_t now) {
  if (view_dirty_) rebuild_view();
  if (view_.ended) return kErrState;
  Timeline d = playback(now);
  if (d.item.empty()) return kErrNoop;
  Op op;
  op.kind = Kind::Next;
  op.from_item = d.item;
  int r = submit(now, std::move(op));
  after_input(now);
  return r;
}

int Session::cmd_previous(int64_t now) {
  if (view_dirty_) rebuild_view();
  if (view_.ended) return kErrState;
  Timeline d = playback(now);
  if (d.item.empty()) return kErrNoop;
  Op op;
  op.kind = Kind::Previous;
  op.from_item = d.item;
  op.pos = local_pos(now, d);
  int r = submit(now, std::move(op));
  after_input(now);
  return r;
}

int Session::cmd_jump(int64_t now, const std::string& item) {
  if (view_dirty_) rebuild_view();
  if (view_.ended) return kErrState;
  if (!find_alive(view_, item)) return kErrInvalid;
  Op op;
  op.kind = Kind::Jump;
  op.has_item = true;
  op.item_id = item;
  int r = submit(now, std::move(op));
  after_input(now);
  return r;
}

int Session::cmd_insert(int64_t now, const std::string* after, std::vector<Entry> entries,
                        std::vector<std::string>* out_ids) {
  if (entries.empty()) return kErrInvalid;
  for (auto& e : entries) {
    if (e.item_id.empty()) e.item_id = new_id('.');
    if (e.item_id.size() > 64 || e.chat_id.empty() || e.chat_id.size() > 32 || e.doc_id.empty() ||
        e.doc_id.size() > 32 || e.duration_ms < 0 || e.msg_id < 0 || e.chat_username.size() > 32) {
      return kErrInvalid;
    }
    for (char c : e.chat_username) {
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return kErrInvalid;
    }
    if (e.title.size() > 256) e.title.resize(256);
    if (e.performer.size() > 256) e.performer.resize(256);
    e.added_by.clear();
    e.alive = true;
  }
  if ((entries.size() + kMaxBatch - 1) / kMaxBatch + pending_.size() > kMaxPending) return kErrFull;
  if (out_ids) {
    out_ids->clear();
    for (const auto& e : entries) out_ids->push_back(e.item_id);
  }
  // Chunk into ops of <= 100 entries, each chained after the previous chunk.
  bool after_null = after == nullptr;
  std::string anchor = after ? *after : std::string();
  int r = kOk;
  for (size_t i = 0; i < entries.size() && r == kOk; i += kMaxBatch) {
    Op op;
    op.kind = Kind::Insert;
    op.after_null = after_null;
    op.after = anchor;
    size_t end = std::min(entries.size(), i + kMaxBatch);
    op.entries.assign(entries.begin() + static_cast<std::ptrdiff_t>(i),
                      entries.begin() + static_cast<std::ptrdiff_t>(end));
    anchor = op.entries.back().item_id;
    after_null = false;
    r = submit(now, std::move(op));
  }
  after_input(now);
  return r;
}

int Session::cmd_remove(int64_t now, std::vector<std::string> ids) {
  if (ids.empty()) return kErrInvalid;
  int r = kOk;
  for (size_t i = 0; i < ids.size() && r == kOk; i += kMaxBatch) {
    Op op;
    op.kind = Kind::Remove;
    size_t end = std::min(ids.size(), i + kMaxBatch);
    op.item_ids.assign(ids.begin() + static_cast<std::ptrdiff_t>(i), ids.begin() + static_cast<std::ptrdiff_t>(end));
    r = submit(now, std::move(op));
  }
  after_input(now);
  return r;
}

int Session::cmd_move(int64_t now, const std::string& item, const std::string* after) {
  if (item.empty()) return kErrInvalid;
  Op op;
  op.kind = Kind::Move;
  op.has_item = true;
  op.item_id = item;
  op.after_null = after == nullptr;
  if (after) op.after = *after;
  int r = submit(now, std::move(op));
  after_input(now);
  return r;
}

int Session::cmd_set_repeat(int64_t now, Repeat mode) {
  Op op;
  op.kind = Kind::SetRepeat;
  op.mode = mode;
  int r = submit(now, std::move(op));
  after_input(now);
  return r;
}

int Session::cmd_set_shuffle(int64_t now, bool on) {
  Op op;
  op.kind = Kind::SetShuffle;
  op.on = on;
  int r = submit(now, std::move(op));
  after_input(now);
  return r;
}

int Session::cmd_kick(int64_t now, const std::string& user) {
  if (user.empty()) return kErrInvalid;
  if (!user_id_.empty() && view_.creator != user_id_) return kErrState;
  Op op;
  op.kind = Kind::Kick;
  op.user_id = user;
  int r = submit(now, std::move(op));
  after_input(now);
  return r;
}

int Session::cmd_end(int64_t now) {
  if (!user_id_.empty() && view_.creator != user_id_) return kErrState;
  Op op;
  op.kind = Kind::End;
  int r = submit(now, std::move(op));
  after_input(now);
  return r;
}

int Session::cmd_leave(int64_t now) {
  if (conn_ == ConnState::Terminal || leaving_) return kErrState;
  if (conn_ != ConnState::Live) {
    leaving_ = true;
    finish_leave();
    after_input(now);
    return kOk;
  }
  Op op;
  op.kind = Kind::Leave;
  int r = submit(now, std::move(op));
  if (r == kOk) {
    leaving_ = true;
    leave_deadline_ = now + kLeaveTimeoutMs;
  }
  after_input(now);
  return r;
}

void Session::send_status() {
  if (conn_ != ConnState::Live) return;
  if (interrupted_) send(encode_status("interrupted", interrupt_reason_name(interrupt_reason_)));
  else send(encode_status("active"));
}

void Session::interruption_began(int64_t now, int reason) {
  if (reason != kInterruptCall && reason != kInterruptOtherAudio) reason = kInterruptCall;
  bool changed = !interrupted_ || !interrupt_active_ || interrupt_reason_ != reason;
  // A call during other audio upgrades the reason; other audio during a call keeps "call".
  if (!(interrupted_ && interrupt_active_ && interrupt_reason_ == kInterruptCall)) interrupt_reason_ = reason;
  interrupted_ = true;
  interrupt_active_ = true;
  if (player_.st == PlayerModel::Playing) player_pause(now, std::max<int64_t>(0, player_est(now)));
  if (reason == kInterruptOtherAudio && player_.st != PlayerModel::None) {
    // Other audio may take over the platform's (often single) player: forget
    // what we had loaded so the item is prepared again when we rejoin.
    std::string preloaded = player_.preloaded;
    player_ = PlayerModel{};
    player_.preloaded = preloaded;
  }
  if (changed) {
    send_status();
    ui_dirty_ = true;
  }
  after_input(now);
}

void Session::interruption_ended(int64_t now) {
  if (interrupted_ && interrupt_active_) {
    interrupt_active_ = false;
    if (interrupt_reason_ == kInterruptOtherAudio) {
      catch_up(now);  // automatic rejoin at the live position, no tap
      return;
    }
    ui_dirty_ = true;  // call over: the UI offers "Catch up"
  }
  after_input(now);
}

void Session::catch_up(int64_t now) {
  if (interrupted_) {
    interrupted_ = false;
    interrupt_active_ = false;
    interrupt_reason_ = kInterruptNone;
    player_.seen = AnchorSig{};  // force a fresh alignment to the live position
    send_status();
    ui_dirty_ = true;
  }
  after_input(now);
}

void Session::set_interrupted(int64_t now, bool interrupted) {
  if (interrupted) interruption_began(now, kInterruptCall);
  else catch_up(now);
}

// ---------------------------------------------------------------------------
// Player driver

int64_t Session::player_est(int64_t now) const {
  if (player_.st == PlayerModel::Playing && !player_.buffering) return player_.ref_pos + (now - player_.ref_local);
  return player_.ref_pos;
}

void Session::player_load(const std::string& item) {
  Event ev;
  ev.type = EvType::PlayerLoad;
  ev.item_id = item;
  if (const Entry* e = find_entry(view_, item)) ev.entry = *e;
  emit(std::move(ev));
  std::string preloaded = player_.preloaded;
  player_ = PlayerModel{};
  player_.item = item;
  player_.st = PlayerModel::Loading;
  player_.preloaded = preloaded;
  ++stats_.loads;
}

void Session::player_play(int64_t now, int64_t pos) {
  Event ev;
  ev.type = EvType::PlayerPlay;
  ev.item_id = player_.item;
  ev.pos = pos;
  ev.ref = now;
  emit(std::move(ev));
  player_.st = PlayerModel::Playing;
  player_.ref_pos = pos;
  player_.ref_local = now;
  player_.awaiting_start = true;
  player_.off_count = 0;
  player_.next_query = now + cfg_.check_interval_ms;
  player_.ended_item.clear();
  maybe_preload();
}

void Session::player_pause(int64_t now, int64_t pos) {
  Event ev;
  ev.type = EvType::PlayerPause;
  ev.item_id = player_.item;
  ev.pos = pos;
  ev.ref = now;
  emit(std::move(ev));
  player_.st = PlayerModel::Paused;
  player_.ref_pos = pos;
  player_.ref_local = now;
  player_.awaiting_start = false;
  player_.off_count = 0;
  player_.next_query = -1;
}

void Session::player_seek(int64_t now, int64_t pos) {
  Event ev;
  ev.type = EvType::PlayerSeek;
  ev.item_id = player_.item;
  ev.pos = pos;
  ev.ref = now;
  emit(std::move(ev));
  player_.ref_pos = pos;
  player_.ref_local = now;
  player_.off_count = 0;
}

void Session::maybe_preload() {
  if (player_.item.empty()) return;
  std::string nxt = succ_auto(view_, player_.item);
  if (nxt.empty() || nxt == player_.item || nxt == player_.preloaded) return;
  Event ev;
  ev.type = EvType::PlayerPreload;
  ev.item_id = nxt;
  if (const Entry* e = find_entry(view_, nxt)) ev.entry = *e;
  emit(std::move(ev));
  player_.preloaded = nxt;
}

bool Session::should_hold(int64_t now, const Timeline& d) {
  if (player_.st != PlayerModel::Playing || player_.item.empty()) return false;
  const Entry* pe = find_alive(view_, player_.item);
  if (!pe || pe->duration_ms <= 0) return false;
  // Only natural boundaries: the next item has just started, or the queue ran out.
  std::string nxt = succ_auto(view_, player_.item);
  bool natural = d.playing ? (nxt == d.item && d.pos <= cfg_.end_hold_ms) : nxt.empty();
  if (!natural) return false;
  if (player_est(now) < pe->duration_ms - 1500) return false;  // player is not finishing that track
  if (player_.hold_from != player_.item) {
    player_.hold_from = player_.item;
    player_.hold_until = now + cfg_.end_hold_ms;
  }
  return now < player_.hold_until;
}

void Session::reconcile(int64_t now) {
  if (log_in_progress_) return;  // wait for the whole catch-up before touching the player
  bool active = conn_ != ConnState::Terminal && !leaving_ && !view_.ended;
  Timeline d = timeline_at(view_, server_now(now));
  if (d.item != ui_item_) {
    ui_item_ = d.item;
    ui_dirty_ = true;
  }
  if (!active || d.item.empty()) {
    if (player_.st != PlayerModel::None) {
      Event ev;
      ev.type = EvType::PlayerStop;
      ev.item_id = player_.item;
      emit(std::move(ev));
      player_ = PlayerModel{};
    }
    return;
  }
  if (interrupted_) return;

  if (player_.item != d.item) {
    if (should_hold(now, d)) return;
    player_load(d.item);
    return;
  }
  player_.hold_from.clear();
  player_.hold_until = -1;

  switch (player_.st) {
    case PlayerModel::None:
      player_load(d.item);
      return;
    case PlayerModel::Loading:
      return;
    case PlayerModel::Failed:
      // Unplayable here: stay silent; retry only if someone re-anchors this item.
      if (!(player_.failed_sig == anchor_sig())) player_load(d.item);
      return;
    case PlayerModel::Ready:
      if (d.playing) {
        // The player finished this item but the log hasn't moved on yet.
        if (player_.ended_item == d.item && player_.ended_sig == anchor_sig()) return;
        player_play(now, d.pos);
      } else {
        player_pause(now, d.pos);
      }
      player_.seen = anchor_sig();
      return;
    case PlayerModel::Playing:
      if (!d.playing) {
        player_pause(now, d.pos);
        player_.seen = anchor_sig();
        return;
      }
      break;
    case PlayerModel::Paused:
      if (d.playing) {
        player_play(now, d.pos);
        player_.seen = anchor_sig();
        return;
      }
      break;
  }

  if (!(player_.seen == anchor_sig())) {
    player_.seen = anchor_sig();
    if (iabs(player_est(now) - d.pos) > cfg_.reanchor_tolerance_ms) {
      player_seek(now, d.pos);
      ++stats_.reanchor_seeks;
    }
  }
}

void Session::player_ready(int64_t now, const std::string& item) {
  if (item == player_.item && player_.st == PlayerModel::Loading) player_.st = PlayerModel::Ready;
  after_input(now);
}

void Session::player_started(int64_t now, const std::string& item, int64_t pos) {
  if (item == player_.item && player_.st == PlayerModel::Playing) {
    player_.awaiting_start = false;
    player_.ref_pos = pos;
    player_.ref_local = now;
    player_.last_check = now;
    player_.off_count = 0;
    if (view_dirty_) rebuild_view();
    Timeline d = playback(now);
    if (!interrupted_ && d.item == item && d.playing && iabs(pos - d.pos) > cfg_.start_tolerance_ms) {
      player_seek(now, d.pos);
      ++stats_.start_seeks;
    }
  }
  after_input(now);
}

void Session::player_position(int64_t now, const std::string& item, int64_t pos, bool playing) {
  if (item != player_.item || (player_.st != PlayerModel::Playing && player_.st != PlayerModel::Paused)) {
    after_input(now);
    return;
  }
  player_.ref_pos = pos;
  player_.ref_local = now;
  bool was_awaiting = player_.awaiting_start;
  if (playing) player_.awaiting_start = false;
  if (interrupted_ || player_.buffering || player_.st != PlayerModel::Playing || was_awaiting || !playing) {
    player_.off_count = 0;
    after_input(now);
    return;
  }
  if (now - player_.last_check < cfg_.check_interval_ms - 100) {
    after_input(now);
    return;
  }
  player_.last_check = now;
  if (view_dirty_) rebuild_view();
  Timeline d = playback(now);
  if (d.item != item || !d.playing) {
    player_.off_count = 0;
  } else if (iabs(pos - d.pos) > cfg_.correction_threshold_ms) {
    if (++player_.off_count >= cfg_.correction_checks) {
      player_seek(now, d.pos);
      ++stats_.corrections;
    }
  } else {
    player_.off_count = 0;
  }
  after_input(now);
}

void Session::player_ended(int64_t now, const std::string& item) {
  if (item != player_.item || player_.st == PlayerModel::None || player_.st == PlayerModel::Loading) {
    after_input(now);
    return;
  }
  if (view_dirty_) rebuild_view();
  const Entry* e = find_entry(view_, item);
  player_.st = PlayerModel::Ready;
  player_.awaiting_start = false;
  player_.ref_pos = e ? e->duration_ms : player_.ref_pos;
  player_.next_query = -1;
  player_.ended_item = item;
  player_.ended_sig = anchor_sig();
  if (!interrupted_ && conn_ != ConnState::Terminal && !leaving_) {
    Timeline d = playback(now);
    std::string nxt = succ_auto(view_, item);
    if (d.playing && (d.item == item || (!nxt.empty() && nxt == d.item))) {
      Op op;
      op.kind = Kind::Advanced;
      op.from_item = item;
      op.to_item = nxt;
      submit(now, std::move(op));
    }
  }
  after_input(now);
}

void Session::player_failed(int64_t now, const std::string& item) {
  if (item == player_.item && player_.st != PlayerModel::None && player_.st != PlayerModel::Failed) {
    if (view_dirty_) rebuild_view();
    player_.st = PlayerModel::Failed;
    player_.failed_sig = anchor_sig();
    player_.awaiting_start = false;
    player_.next_query = -1;
    emit_error("item_unavailable", false, item);
  }
  after_input(now);
}

void Session::player_buffering(int64_t now, const std::string& item, bool buffering) {
  if (item == player_.item) {
    if (buffering && !player_.buffering) {
      player_.ref_pos = player_est(now);
      player_.ref_local = now;
      player_.buffering = true;
    } else if (!buffering && player_.buffering) {
      player_.buffering = false;
      player_.ref_local = now;
    }
    player_.off_count = 0;
  }
  after_input(now);
}

// ---------------------------------------------------------------------------
// Timers

int64_t Session::next_timeout() const {
  int64_t best = -1;
  auto consider = [&](int64_t t) {
    if (t >= 0 && (best < 0 || t < best)) best = t;
  };
  if (conn_ == ConnState::Connecting || conn_ == ConnState::Handshaking || conn_ == ConnState::Backoff) {
    consider(conn_deadline_);
  }
  if (conn_ == ConnState::Live) {
    consider(next_ping_);
    for (const auto& p : pending_) {
      if (p.sent_at >= 0) consider(p.sent_at + kResendAfterMs);
    }
    if (gap_since_ >= 0) {
      int64_t t = gap_since_ + kGapWaitMs;
      if (last_sync_ >= 0) t = std::max(t, last_sync_ + kSyncRepeatMs);
      consider(t);
    }
  }
  if (player_.st == PlayerModel::Playing) consider(player_.next_query);
  if (player_.hold_until >= 0) consider(player_.hold_until);
  if (leaving_) consider(leave_deadline_);
  // Next derived track boundary, so automatic advance is acted on in time.
  if (conn_ != ConnState::Terminal) consider(boundary_local_);
  return best;
}

void Session::tick(int64_t now) {
  switch (conn_) {
    case ConnState::Connecting:
    case ConnState::Handshaking:
      if (conn_deadline_ >= 0 && now >= conn_deadline_) {
        Event ev;
        ev.type = EvType::Disconnect;
        emit(std::move(ev));
        schedule_reconnect(now);
      }
      break;
    case ConnState::Backoff:
      if (conn_deadline_ >= 0 && now >= conn_deadline_) begin_connect(now);
      break;
    default:
      break;
  }
  if (conn_ == ConnState::Live) {
    if (next_ping_ >= 0 && now >= next_ping_) {
      send(encode_ping(now));
      if (burst_left_ > 0) --burst_left_;
      next_ping_ = now + (burst_left_ > 0 ? kPingBurstGapMs : kPingIntervalMs);
    }
    for (auto& p : pending_) {
      if (p.sent_at >= 0 && now - p.sent_at >= kResendAfterMs) {
        send(encode_op_message(p.op));
        p.sent_at = now;
        ++stats_.resends;
      }
    }
    if (gap_since_ >= 0 && now - gap_since_ >= kGapWaitMs && (last_sync_ < 0 || now - last_sync_ >= kSyncRepeatMs)) {
      send(encode_sync(confirmed_rev_));
      last_sync_ = now;
      ++stats_.syncs;
    }
  }
  if (player_.st == PlayerModel::Playing && player_.next_query >= 0 && now >= player_.next_query) {
    Event ev;
    ev.type = EvType::PlayerQuery;
    ev.item_id = player_.item;
    emit(std::move(ev));
    player_.next_query = now + cfg_.check_interval_ms;
  }
  if (leaving_ && leave_deadline_ >= 0 && now >= leave_deadline_) finish_leave();
  after_input(now);
}

void Session::after_input(int64_t now) {
  if (view_dirty_) rebuild_view();
  bool gap = !buffered_.empty() || known_head_ > confirmed_rev_;
  if (gap && conn_ == ConnState::Live) {
    if (gap_since_ < 0) gap_since_ = now;
  } else {
    gap_since_ = -1;
  }
  reconcile(now);
  // Local time of the next derived track boundary.
  boundary_local_ = -1;
  if (view_.playing && !view_.cur.empty()) {
    int64_t snow = server_now(now);
    Timeline d = timeline_at(view_, snow);
    if (d.playing) {
      const Entry* e = find_entry(view_, d.item);
      if (e && e->duration_ms > 0) boundary_local_ = now + (e->duration_ms - d.pos) + 1;
    }
  }
  if (ui_dirty_) {
    Event ev;
    ev.type = EvType::StateChanged;
    emit(std::move(ev));
    ui_dirty_ = false;
  }
}

bool Session::poll(Event& out) {
  if (out_.empty()) return false;
  out = std::move(out_.front());
  out_.pop_front();
  return true;
}

// ---------------------------------------------------------------------------
// UI state

std::string Session::debug_pending() const {
  std::string s;
  for (const auto& p : pending_) {
    s += std::string(kind_name(p.op.kind)) + " " + p.op.op_id + " sent_at=" + std::to_string(p.sent_at) +
         " acked=" + std::to_string(p.acked_rev) + "; ";
  }
  s += "conn=" + std::string(conn_state_name(conn_)) + " rev=" + std::to_string(confirmed_rev_) +
       " head=" + std::to_string(known_head_) + " buffered=" + std::to_string(buffered_.size());
  return s;
}

std::string Session::state_json(int64_t now) const {
  json::Value v = json::Value::object();
  v.set("conn", conn_state_name(conn_));
  v.set("error", terminal_code_.empty() ? json::Value() : json::Value(terminal_code_));
  v.set("user_id", user_id_);
  v.set("rev", confirmed_rev_);
  v.set("pending", static_cast<int64_t>(pending_.size()));
  json::Value sess = json::Value::object();
  sess.set("creator", view_.creator);
  sess.set("host_chat_id", view_.host_chat_id);
  sess.set("title", view_.title);
  sess.set("ended", view_.ended);
  sess.set("max_participants", max_participants_);
  v.set("session", std::move(sess));
  v.set("repeat", repeat_name(view_.repeat));
  v.set("shuffle", view_.shuffle);
  int64_t snow = server_now(now);
  Timeline d = timeline_at(view_, snow);
  json::Value np = json::Value::object();
  np.set("item_id", d.item.empty() ? json::Value() : json::Value(d.item));
  np.set("pos_ms", d.pos);
  np.set("playing", d.playing);
  np.set("server_time", snow);
  const Entry* cur = find_entry(view_, d.item);
  np.set("duration_ms", cur ? cur->duration_ms : 0);
  v.set("now_playing", std::move(np));
  json::Value q = json::Value::array();
  for (const auto& id : play_order(view_)) {
    if (const Entry* e = find_entry(view_, id)) q.push(entry_to_json(*e, true));
  }
  v.set("queue", std::move(q));
  v.set("history_len", static_cast<int64_t>(view_.history.size()));
  json::Value ps = json::Value::array();
  for (const auto& p : view_.participants) {
    json::Value j = json::Value::object();
    j.set("user_id", p.user_id);
    j.set("name", p.name);
    bool online = false;
    std::string state = "offline";
    std::string reason;
    for (const auto& o : online_) {
      if (o.user_id == p.user_id) {
        online = true;
        if (state != "active") {
          state = o.state;
          reason = o.state == "interrupted" ? o.reason : std::string();
        }
      }
    }
    if (p.user_id == user_id_ && conn_ == ConnState::Live) {
      online = true;
      state = interrupted_ ? "interrupted" : "active";
      reason = interrupted_ ? interrupt_reason_name(interrupt_reason_) : "";
    }
    j.set("online", online);
    j.set("state", state);
    if (!reason.empty()) j.set("reason", reason);
    ps.push(std::move(j));
  }
  v.set("participants", std::move(ps));
  if (interrupted_) {
    json::Value li = json::Value::object();
    li.set("reason", interrupt_reason_name(interrupt_reason_));
    li.set("awaiting_catch_up", !interrupt_active_);
    v.set("interruption", std::move(li));
  } else {
    v.set("interruption", json::Value());
  }
  json::Value c = json::Value::object();
  c.set("offset_ms", clock_.offset());
  c.set("rtt_ms", clock_.rtt());
  c.set("samples", static_cast<int64_t>(clock_.total_samples()));
  v.set("clock", std::move(c));
  return json::dump(v);
}

}  // namespace lt
