// lt_stdio: a real lt_core client driven over stdin/stdout, for end-to-end
// tests (backend/e2e bridges it to a WebSocket). Real time (steady clock),
// with a built-in fake audio player.
//
// Input: one JSON object per line.
//   {"t":"init","device_id":"…","token":"…","seed":1,"load_ms":150,"start_ms":30}   (first)
//   {"t":"start"} {"t":"open"} {"t":"close","code":1006} {"t":"msg","data":"<frame>"}
//   {"t":"cmd","name":"insert","after":null,"entries":[…]}   play pause seek{pos} next previous
//        jump{item_id} remove{item_ids} move{item_id,after} set_repeat{mode} set_shuffle{on}
//        leave kick{user_id} end interrupted{on}
//   {"t":"state"} {"t":"quit"}
// Output: one JSON object per line.
//   {"ev":"send","data":…} {"ev":"connect"} {"ev":"disconnect"} {"ev":"error",…}
//   {"ev":"state",…} {"ev":"result",…} {"ev":"player",…}
#include <poll.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "json.hpp"
#include "tunegram/lt_core.h"

using lt::json::Value;

namespace {

int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void out(const Value& v) {
  std::string s = lt::json::dump(v);
  s.push_back('\n');
  fwrite(s.data(), 1, s.size(), stdout);
  fflush(stdout);
}

struct Player {
  enum St { Idle, Loading, Ready, Starting, Playing, Paused, Ended } st = Idle;
  std::string item;
  int64_t dur = 0;
  int64_t pos_base = 0, t_base = 0;
  int64_t due = -1;  // next internal event
  int64_t pending_pos = 0, pending_ref = 0;
  int64_t load_ms = 150, start_ms = 30;

  int64_t pos(int64_t t) const { return st == Playing ? pos_base + (t - t_base) : pos_base; }
};

struct App {
  lt_core* core = nullptr;
  Player p;
  int64_t last_state_ms = 0;

  void emit_state(const char* why) {
    if (!core) return;
    int64_t t = now_ms();
    Value v = Value::object();
    v.set("ev", "state");
    v.set("why", why);
    Value st;
    lt::json::parse(lt_core_state_json(core, t), st);
    v.set("state", std::move(st));
    Value pl = Value::object();
    pl.set("item", p.item);
    pl.set("pos", p.pos(t));
    pl.set("audible", p.st == Player::Playing);
    v.set("player", std::move(pl));
    v.set("server_now", lt_core_server_time(core, t));
    out(v);
  }

  void player_event(const char* what, const lt_event& ev) {
    Value v = Value::object();
    v.set("ev", "player");
    v.set("what", what);
    v.set("item", ev.item_id ? ev.item_id : "");
    v.set("pos", ev.pos_ms);
    out(v);
  }

  void drain() {
    lt_event ev;
    while (core && lt_core_poll(core, &ev)) {
      int64_t t = now_ms();
      switch (ev.type) {
        case LT_EV_SEND: {
          Value v = Value::object();
          v.set("ev", "send");
          v.set("data", std::string(ev.data, ev.len));
          out(v);
          break;
        }
        case LT_EV_CONNECT: {
          Value v = Value::object();
          v.set("ev", "connect");
          out(v);
          break;
        }
        case LT_EV_DISCONNECT: {
          Value v = Value::object();
          v.set("ev", "disconnect");
          out(v);
          break;
        }
        case LT_EV_PLAYER_LOAD:
          player_event("load", ev);
          p.st = Player::Loading;
          p.item = ev.item_id;
          p.dur = ev.duration_ms;
          p.pos_base = 0;
          p.due = t + p.load_ms;
          break;
        case LT_EV_PLAYER_PLAY:
          player_event("play", ev);
          if (p.item == ev.item_id) {
            p.st = Player::Starting;
            p.pending_pos = ev.pos_ms;
            p.pending_ref = ev.ref_ms;
            p.pos_base = ev.pos_ms;
            p.due = t + p.start_ms;
          }
          break;
        case LT_EV_PLAYER_PAUSE:
          player_event("pause", ev);
          if (p.item == ev.item_id) {
            p.st = Player::Paused;
            p.pos_base = ev.pos_ms;
            p.due = -1;
          }
          break;
        case LT_EV_PLAYER_SEEK:
          player_event("seek", ev);
          if (p.item == ev.item_id) {
            if (p.st == Player::Playing || p.st == Player::Starting) {
              p.st = Player::Playing;
              p.pos_base = ev.pos_ms + (t - ev.ref_ms);
              p.t_base = t;
              p.due = p.dur > 0 ? t + (p.dur - p.pos_base) : -1;
            } else {
              p.pos_base = ev.pos_ms;
            }
          }
          break;
        case LT_EV_PLAYER_STOP:
          player_event("stop", ev);
          {
            int64_t lm = p.load_ms, sm = p.start_ms;
            p = Player{};
            p.load_ms = lm;
            p.start_ms = sm;
          }
          break;
        case LT_EV_PLAYER_PRELOAD:
          break;
        case LT_EV_PLAYER_QUERY:
          if (p.st == Player::Playing) lt_core_player_position(core, t, p.item.c_str(), p.pos(t), 1);
          break;
        case LT_EV_STATE_CHANGED:
          emit_state("changed");
          break;
        case LT_EV_ERROR: {
          Value v = Value::object();
          v.set("ev", "error");
          v.set("code", ev.code ? ev.code : "");
          v.set("message", std::string(ev.data, ev.len));
          v.set("fatal", ev.fatal != 0);
          out(v);
          break;
        }
        default:
          break;
      }
    }
  }

  void player_due(int64_t t) {
    if (p.due < 0 || t < p.due) return;
    p.due = -1;
    std::string item = p.item;
    switch (p.st) {
      case Player::Loading:
        p.st = Player::Ready;
        lt_core_player_ready(core, t, item.c_str());
        break;
      case Player::Starting: {
        int64_t start = p.pending_pos + (t - p.pending_ref);
        p.st = Player::Playing;
        p.pos_base = start < 0 ? 0 : start;
        p.t_base = t;
        p.due = p.dur > 0 ? t + (p.dur - p.pos_base) : -1;
        lt_core_player_started(core, t, item.c_str(), p.pos_base);
        break;
      }
      case Player::Playing:
        p.st = Player::Ended;
        p.pos_base = p.dur;
        lt_core_player_ended(core, t, item.c_str());
        break;
      default:
        break;
    }
  }

  int command(const Value& m, int64_t t, Value& extra) {
    const std::string name = m.get_str("name");
    if (name == "play") return lt_core_play(core, t);
    if (name == "pause") return lt_core_pause(core, t);
    if (name == "seek") return lt_core_seek(core, t, m.get_int("pos"));
    if (name == "next") return lt_core_next(core, t);
    if (name == "previous") return lt_core_previous(core, t);
    if (name == "jump") return lt_core_jump(core, t, m.get_str("item_id").c_str());
    if (name == "set_repeat") return lt_core_set_repeat(core, t, static_cast<int32_t>(m.get_int("mode")));
    if (name == "set_shuffle") return lt_core_set_shuffle(core, t, m.get_bool("on") ? 1 : 0);
    if (name == "leave") return lt_core_leave(core, t);
    if (name == "end") return lt_core_end_session(core, t);
    if (name == "kick") return lt_core_kick(core, t, m.get_str("user_id").c_str());
    if (name == "interrupted") {
      lt_core_set_interrupted(core, t, m.get_bool("on") ? 1 : 0);
      return LT_OK;
    }
    if (name == "move") {
      const Value* after = m.get("after");
      std::string a = after && after->is_string() ? after->as_str() : std::string();
      return lt_core_move(core, t, m.get_str("item_id").c_str(), after && after->is_string() ? a.c_str() : nullptr);
    }
    if (name == "remove") {
      std::vector<std::string> ids;
      if (const Value* arr = m.get("item_ids")) {
        for (const auto& x : arr->items()) ids.push_back(x.as_str());
      }
      std::vector<const char*> ptrs;
      for (const auto& s : ids) ptrs.push_back(s.c_str());
      return lt_core_remove(core, t, ptrs.data(), ptrs.size());
    }
    if (name == "insert") {
      const Value* es = m.get("entries");
      if (!es || !es->is_array()) return LT_ERR_INVALID;
      std::vector<Value> keep = es->items();
      std::vector<std::string> strs;
      strs.reserve(keep.size() * 5);
      std::vector<lt_entry> entries;
      for (const auto& e : keep) {
        lt_entry le{};
        auto s = [&](const char* k) -> const char* {
          const Value* v = e.get(k);
          if (!v || !v->is_string()) return nullptr;
          strs.push_back(v->as_str());
          return strs.back().c_str();
        };
        le.item_id = s("item_id");
        le.chat_id = s("chat_id");
        le.doc_id = s("doc_id");
        le.title = s("title");
        le.performer = s("performer");
        le.msg_id = e.get_int("msg_id");
        le.duration_ms = e.get_int("duration_ms");
        entries.push_back(le);
      }
      const Value* after = m.get("after");
      std::string a = after && after->is_string() ? after->as_str() : std::string();
      const char* ids = nullptr;
      int r = lt_core_insert(core, t, after && after->is_string() ? a.c_str() : nullptr, entries.data(),
                             entries.size(), &ids);
      Value idv;
      if (ids && lt::json::parse(ids, idv)) extra = std::move(idv);
      return r;
    }
    return LT_ERR_INVALID;
  }

  bool handle(const std::string& line) {
    Value m;
    if (!lt::json::parse(line, m) || !m.is_object()) return true;
    const std::string t = m.get_str("t");
    int64_t now = now_ms();
    if (t == "init") {
      std::string dev = m.get_str("device_id"), tok = m.get_str("token");
      lt_config cfg{dev.c_str(), tok.c_str(), static_cast<int32_t>(m.get_int("protocol_version")),
                    static_cast<uint64_t>(m.get_int("seed", 1))};
      core = lt_core_create(&cfg);
      p.load_ms = m.get_int("load_ms", 150);
      p.start_ms = m.get_int("start_ms", 30);
      Value v = Value::object();
      v.set("ev", "ready");
      v.set("ok", core != nullptr);
      out(v);
      return core != nullptr;
    }
    if (!core) return true;
    if (t == "start") lt_core_start(core, now);
    else if (t == "open") lt_core_on_open(core, now);
    else if (t == "close") lt_core_on_close(core, now, static_cast<int32_t>(m.get_int("code", 1006)));
    else if (t == "msg") {
      const std::string& d = m.get("data") ? m.get("data")->as_str() : std::string();
      lt_core_on_message(core, now, d.data(), d.size());
    } else if (t == "cmd") {
      Value extra;
      int r = command(m, now, extra);
      Value v = Value::object();
      v.set("ev", "result");
      v.set("id", m.get_int("id"));
      v.set("name", m.get_str("name"));
      v.set("result", r);
      if (!extra.is_null()) v.set("ids", std::move(extra));
      out(v);
    } else if (t == "state") {
      drain();
      emit_state("request");
    } else if (t == "quit") {
      return false;
    }
    drain();
    return true;
  }
};

}  // namespace

int main() {
  App app;
  std::string buf;
  char chunk[65536];
  bool running = true;
  while (running) {
    int64_t t = now_ms();
    if (app.core) {
      int64_t ct = lt_core_next_timeout(app.core);
      if (ct >= 0 && ct <= t) {
        lt_core_tick(app.core, t);
        app.drain();
      }
      if (app.p.due >= 0 && app.p.due <= t) {
        app.player_due(t);
        app.drain();
      }
    }
    int timeout = 50;
    if (app.core) {
      int64_t ct = lt_core_next_timeout(app.core);
      int64_t next = -1;
      if (ct >= 0) next = ct;
      if (app.p.due >= 0 && (next < 0 || app.p.due < next)) next = app.p.due;
      if (next >= 0) timeout = static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(next - now_ms(), 50)));
    }
    pollfd pfd{STDIN_FILENO, POLLIN, 0};
    int r = poll(&pfd, 1, timeout);
    if (r > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
      ssize_t n = read(STDIN_FILENO, chunk, sizeof chunk);
      if (n <= 0) break;
      buf.append(chunk, static_cast<size_t>(n));
      size_t pos;
      while (running && (pos = buf.find('\n')) != std::string::npos) {
        std::string line = buf.substr(0, pos);
        buf.erase(0, pos + 1);
        running = app.handle(line);
      }
    }
  }
  if (app.core) lt_core_destroy(app.core);
  return 0;
}
