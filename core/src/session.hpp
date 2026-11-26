// Client session state machine: connection, clock sync, confirmed log +
// pending ops (local-first rebase), and the player driver (PROTOCOL.md §7, §10).
// No I/O: inputs are bytes, player reports and "now"; outputs are Events.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "clock.hpp"
#include "json.hpp"
#include "model.hpp"
#include "reducer.hpp"

namespace lt {

enum class EvType : int32_t {
  Send = 1,
  Connect = 2,
  Disconnect = 3,
  PlayerLoad = 10,
  PlayerPlay = 11,
  PlayerPause = 12,
  PlayerSeek = 13,
  PlayerStop = 14,
  PlayerPreload = 15,
  PlayerQuery = 16,
  StateChanged = 20,
  Error = 21,
};

struct Event {
  EvType type = EvType::StateChanged;
  std::string data;     // Send: frame. Error: message.
  std::string code;     // Error: code.
  bool fatal = false;   // Error.
  std::string item_id;  // Player*.
  int64_t pos = 0;      // Player*: position in ms, valid at local time `ref`.
  int64_t ref = 0;
  Entry entry;          // PlayerLoad / PlayerPreload.
};

enum class ConnState : uint8_t { Idle, Connecting, Handshaking, Live, Backoff, AuthRequired, Terminal };
const char* conn_state_name(ConnState s);

enum : int { kInterruptNone = 0, kInterruptCall = 1, kInterruptOtherAudio = 2 };
const char* interrupt_reason_name(int reason);

// Result codes shared with the C API.
enum : int { kOk = 0, kErrInvalid = -1, kErrState = -2, kErrNoop = -3, kErrFull = -4 };

struct Config {
  std::string device_id;
  std::string token;
  int protocol_version = kProtocolVersion;
  uint64_t seed = 0;
  // Playback policy (PROTOCOL.md §10).
  int64_t correction_threshold_ms = 400;
  int correction_checks = 2;
  int64_t check_interval_ms = 1000;
  int64_t reanchor_tolerance_ms = 100;
  int64_t start_tolerance_ms = 100;
  int64_t end_hold_ms = 2000;
};

struct Stats {
  uint64_t corrections = 0;     // seeks from the 400 ms / 2-check rule
  uint64_t reanchor_seeks = 0;  // seeks because the anchor changed
  uint64_t start_seeks = 0;     // seeks right after audio started
  uint64_t loads = 0;
  uint64_t syncs = 0;
  uint64_t resends = 0;
  uint64_t reconnects = 0;
  uint64_t ops_sent = 0;
};

class Session {
 public:
  explicit Session(Config cfg);

  // Transport.
  void start(int64_t now);
  void on_open(int64_t now);
  void on_close(int64_t now, int code);
  void on_message(int64_t now, std::string_view data);
  void tick(int64_t now);
  int64_t next_timeout() const;  // absolute local ms, or -1
  void set_token(int64_t now, std::string token);

  // Player reports.
  void player_ready(int64_t now, const std::string& item);
  void player_started(int64_t now, const std::string& item, int64_t pos);
  void player_position(int64_t now, const std::string& item, int64_t pos, bool playing);
  void player_ended(int64_t now, const std::string& item);
  void player_buffering(int64_t now, const std::string& item, bool buffering);
  void player_failed(int64_t now, const std::string& item);

  // User commands (local-first). Return kOk or a negative error.
  int cmd_play(int64_t now);
  int cmd_pause(int64_t now);
  int cmd_seek(int64_t now, int64_t pos);
  int cmd_next(int64_t now);
  int cmd_previous(int64_t now);
  int cmd_jump(int64_t now, const std::string& item);
  int cmd_insert(int64_t now, const std::string* after, std::vector<Entry> entries,
                 std::vector<std::string>* out_ids = nullptr);
  int cmd_remove(int64_t now, std::vector<std::string> ids);
  int cmd_move(int64_t now, const std::string& item, const std::string* after);
  int cmd_set_repeat(int64_t now, Repeat mode);
  int cmd_set_shuffle(int64_t now, bool on);
  int cmd_kick(int64_t now, const std::string& user);
  int cmd_end(int64_t now);
  int cmd_leave(int64_t now);
  // Local interruptions (PROTOCOL.md §10). reason: kInterruptCall or kInterruptOtherAudio.
  void interruption_began(int64_t now, int reason);
  void interruption_ended(int64_t now);  // other_audio: rejoin at once; call: wait for catch_up
  void catch_up(int64_t now);
  void set_interrupted(int64_t now, bool interrupted);  // legacy: true = began(call), false = catch_up

  bool poll(Event& out);

  // Introspection.
  const State& view() const { return view_; }
  const State& confirmed() const { return confirmed_; }
  int64_t confirmed_rev() const { return confirmed_rev_; }
  size_t pending_count() const { return pending_.size(); }
  ConnState conn_state() const { return conn_; }
  const std::string& terminal_code() const { return terminal_code_; }
  const ClockSync& clock() const { return clock_; }
  int64_t server_now(int64_t now) const { return now + clock_.offset(); }
  Timeline playback(int64_t now) const { return timeline_at(view_, server_now(now)); }
  const std::string& user_id() const { return user_id_; }
  const Stats& stats() const { return stats_; }
  bool interrupted() const { return interrupted_; }
  bool awaiting_catch_up() const { return interrupted_ && !interrupt_active_; }
  int interrupt_reason() const { return interrupt_reason_; }
  std::string state_json(int64_t now) const;
  std::string debug_pending() const;  // for tests/diagnostics

 private:
  struct Pending {
    Op op;
    int64_t sent_at = -1;
    int64_t acked_rev = 0;
  };
  struct Presence {
    std::string user_id;
    std::string device_id;
    std::string state;
    std::string reason;
  };
  // Identity of the playback anchor. The epoch counter alone is not enough:
  // after a rebase two different histories can reach the same epoch value.
  struct AnchorSig {
    std::string cur;
    int64_t pos = -1;
    int64_t time = -1;
    bool playing = false;
    uint64_t epoch = UINT64_MAX;
    bool operator==(const AnchorSig&) const = default;
  };
  struct PlayerModel {
    enum St : uint8_t { None, Loading, Ready, Playing, Paused, Failed };
    std::string item;
    St st = None;
    int64_t ref_pos = 0;
    int64_t ref_local = 0;
    bool buffering = false;
    bool awaiting_start = false;
    AnchorSig seen;  // anchor the player was last aligned to
    int off_count = 0;
    int64_t last_check = INT64_MIN / 2;
    int64_t next_query = -1;
    std::string preloaded;
    std::string hold_from;
    int64_t hold_until = -1;
    std::string ended_item;
    AnchorSig ended_sig;
    AnchorSig failed_sig;
  };

  // Connection.
  void begin_connect(int64_t now);
  void schedule_reconnect(int64_t now);
  void handle_close_code(int64_t now, int code);
  void go_terminal(const std::string& code, bool emit_error, const std::string& message = {});
  void mark_pending_unsent();
  void send(std::string data);

  // Messages.
  void on_welcome(int64_t now, const json::Value& m);
  void ingest(const Op& rec);
  void apply_confirmed(const Op& rec);
  void rebuild_view();
  int submit(int64_t now, Op op);
  void finish_leave();

  // Player driver.
  AnchorSig anchor_sig() const {
    return AnchorSig{view_.cur, view_.anchor_pos, view_.anchor_time, view_.playing, view_.epoch};
  }
  int64_t player_est(int64_t now) const;
  int64_t local_pos(int64_t now, const Timeline& d) const;
  void reconcile(int64_t now);
  bool should_hold(int64_t now, const Timeline& d);
  void player_load(const std::string& item);
  void player_play(int64_t now, int64_t pos);
  void player_pause(int64_t now, int64_t pos);
  void player_seek(int64_t now, int64_t pos);
  void maybe_preload();

  void after_input(int64_t now);
  void emit(Event ev) { out_.push_back(std::move(ev)); }
  void emit_error(const std::string& code, bool fatal, const std::string& message);
  std::string new_id(char sep);

  Config cfg_;
  std::string tag_;
  uint64_t rng_ = 0;
  uint64_t counter_ = 0;

  ConnState conn_ = ConnState::Idle;
  std::string terminal_code_;
  int64_t conn_deadline_ = -1;
  int attempt_ = 0;
  int64_t hello_t0_ = 0;
  std::string user_id_;
  int64_t max_participants_ = 0;
  std::vector<Presence> online_;
  ClockSync clock_;
  int64_t next_ping_ = -1;
  int burst_left_ = 0;
  bool log_in_progress_ = false;

  State confirmed_;
  int64_t confirmed_rev_ = 0;
  std::map<int64_t, Op> buffered_;
  int64_t known_head_ = 0;
  int64_t gap_since_ = -1;
  int64_t last_sync_ = -1;
  std::deque<Pending> pending_;
  State view_;
  bool view_dirty_ = false;
  bool ui_dirty_ = true;
  std::string ui_item_;
  int64_t boundary_local_ = -1;

  bool leaving_ = false;
  int64_t leave_deadline_ = -1;
  bool interrupted_ = false;       // locally paused, not following the session
  bool interrupt_active_ = false;  // the interruption itself is still going on
  int interrupt_reason_ = kInterruptNone;
  void send_status();
  PlayerModel player_;

  std::deque<Event> out_;
  Stats stats_;
};

}  // namespace lt
