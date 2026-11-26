// Deterministic simulation harness: fake clock, fake network (latency, jitter,
// loss, duplication, reordering, disconnects), an in-process reference
// sequencer (same rules as the Durable Object), a fake audio player, and
// simulated clients driving real lt_core instances through the C API.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <string>
#include <vector>

#include "json.hpp"
#include "model.hpp"
#include "reducer.hpp"
#include "tunegram/lt_core.h"

namespace sim {

struct Rng {
  uint64_t s;
  explicit Rng(uint64_t seed = 1) : s(seed * 0x9E3779B97F4A7C15ull + 7) {}
  uint64_t next() { return lt::splitmix64(s); }
  int64_t range(int64_t lo, int64_t hi) { return hi <= lo ? lo : lo + static_cast<int64_t>(next() % static_cast<uint64_t>(hi - lo + 1)); }
  double unit() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
  bool chance(double p) { return p > 0 && unit() < p; }
  template <typename T>
  const T& pick(const std::vector<T>& v) { return v[static_cast<size_t>(next() % v.size())]; }
};

class Sim {
 public:
  int64_t now = 0;
  void at(int64_t t, std::function<void()> fn);
  void after(int64_t d, std::function<void()> fn) { at(now + d, std::move(fn)); }
  void run_until(int64_t t);
  void run_for(int64_t d) { run_until(now + d); }
  uint64_t steps = 0;

 private:
  struct Task {
    int64_t t;
    uint64_t seq;
    std::function<void()> fn;
  };
  struct Cmp {
    bool operator()(const Task& a, const Task& b) const { return a.t != b.t ? a.t > b.t : a.seq > b.seq; }
  };
  std::priority_queue<Task, std::vector<Task>, Cmp> q_;
  uint64_t seq_ = 0;
};

struct LinkParams {
  int64_t up_ms = 30;    // client -> server base one-way latency
  int64_t down_ms = 30;  // server -> client
  int64_t jitter_ms = 10;
  double loss = 0;       // per-frame drop probability (both directions)
  double dup = 0;        // per-frame duplicate probability
  bool reorder = false;  // frames may overtake each other
};

// Reference sequencer, mirroring backend/src/session.ts.
class SimServer {
 public:
  struct Hooks {
    std::function<void(int conn, const std::string&)> send;
    std::function<void(int conn, int code)> close;
  };

  SimServer(Sim& sim, uint64_t seed) : sim_(sim), rng_(seed) {}
  void set_hooks(Hooks h) { hooks_ = std::move(h); }
  void create(const std::string& creator, const std::string& host_chat_id, const std::string& title = "");

  void on_open(int conn);
  void on_message(int conn, const std::string& data);
  void on_close(int conn);

  int64_t head() const { return static_cast<int64_t>(log_.size()); }
  const std::vector<std::string>& log() const { return log_; }
  std::vector<lt::Op> parsed_log() const;
  lt::State replay() const;
  size_t participants() const { return participants_.size(); }
  bool ended() const { return ended_; }
  uint64_t dup_acks = 0;
  uint64_t rejects = 0;

  int max_participants = 10;
  int min_v = 1;
  int max_v = 1;
  bool allow_all_members = true;
  std::set<std::string> members;
  std::set<std::string> kicked;
  std::string creator;

 private:
  struct Sock {
    bool authed = false;
    std::string user;
    std::string name;
    std::string device;
    std::string state = "active";
    std::string reason;
  };
  void fatal(int conn, const std::string& code, int close_code, const std::string& msg = {});
  void send(int conn, const lt::json::Value& v);
  void broadcast(const lt::json::Value& v, int except = -1);
  int64_t append(lt::json::Value op, const std::string& by);
  void send_log(int conn, int64_t since);
  void presence(const Sock& s, bool online);
  bool user_online(const std::string& user, int except) const;
  void close_user(const std::string& user, const std::string& code, int close_code);

  Sim& sim_;
  Rng rng_;
  Hooks hooks_;
  std::map<int, Sock> socks_;
  std::vector<std::string> log_;
  std::map<std::string, int64_t> op_ids_;
  std::vector<std::string> participants_;
  bool ended_ = false;
};

class World;

struct FakePlayer {
  // Behaviour.
  int64_t load_ms = 250;
  int64_t load_jitter_ms = 150;
  int64_t cached_load_ms = 20;
  int64_t start_ms = 60;
  int64_t seek_ms = 40;
  bool extrapolate = true;  // adapter starts at pos + (now - ref)
  double rate = 1.0;        // playback speed (drift injection)
  int64_t report_noise_ms = 0;
  std::map<std::string, int64_t> extra_ms;  // real duration = metadata + extra
  std::set<std::string> fail_items;         // loads of these report "failed"
  std::vector<std::string> load_usernames;  // chat_username seen in each PLAYER_LOAD

  enum St { Idle, Loading, Ready, Starting, Playing, Paused, Ended };
  St st = Idle;
  std::string item;
  int64_t meta_dur = 0;
  int64_t pos_base = 0;
  int64_t t_base = 0;
  uint64_t gen = 0;
  std::set<std::string> cached;

  // Counters for assertions.
  int loads = 0, plays = 0, pauses = 0, seeks = 0, stops = 0, ends = 0, queries = 0;
  std::vector<std::string> ended_items;
  std::vector<std::string> cut_items;  // replaced by a load while still audibly playing near the end
  int64_t last_start_error = 0;        // |pos - reference live position| when audio last started
  bool has_start_error = false;

  int64_t actual_dur() const;
  int64_t pos_at(int64_t t) const;
  bool audible() const { return st == Playing; }
};

class SimClient {
 public:
  SimClient(World& w, int idx, std::string user, std::string name, uint64_t seed, int protocol_version = 0);
  ~SimClient();

  int64_t local(int64_t t) const;
  int64_t local_now() const;
  int64_t to_true(int64_t local_deadline) const;
  void pump();
  lt_core* core() { return core_; }

  void start();
  void destroy_core();

  // Commands (each followed by pump()).
  int play();
  int pause();
  int seek(int64_t pos);
  int next();
  int previous();
  int jump(const std::string& item);
  std::vector<std::string> insert(const std::string* after, int n, int64_t duration_ms = 180000,
                                  const char* chat_username = nullptr);
  int remove(const std::string& item);
  int move(const std::string& item, const std::string* after);
  int set_repeat(int mode);
  int set_shuffle(bool on);
  int leave();
  int kick(const std::string& user);
  int end_session();

  // Introspection.
  std::string state_json();
  lt_playback playback();
  bool live();
  std::string conn_state();
  size_t pending();
  int64_t rev() { return lt_core_rev(core_); }
  std::vector<std::string> queue_ids();

  World& w;
  int idx;
  std::string user;
  std::string name;
  std::string device;
  int64_t clock_offset = 0;
  double drift_ppm = 0;
  LinkParams link;
  bool net_up = true;
  int conn = -1;
  FakePlayer player;
  std::vector<std::string> errors;
  std::vector<std::string> fatal_errors;
  int connects = 0;
  int state_changes = 0;
  int64_t block_down_until = 0;  // drop server->client frames until this true time
  std::function<void(const lt_event&)> on_event;

  // Player plumbing.
  void player_load(const std::string& item, int64_t dur);
  void player_play(const std::string& item, int64_t pos, int64_t ref);
  void player_pause(const std::string& item, int64_t pos);
  void player_seek(const std::string& item, int64_t pos, int64_t ref);
  void player_stop();
  void player_report();
  void schedule_end();
  // Inject: shift the audible position by delta (a glitch).
  void glitch(int64_t delta_ms);

 private:
  lt_core* core_ = nullptr;
  uint64_t timer_gen_ = 0;
  uint64_t seed_;
  int protocol_version_;
  int64_t entry_counter_ = 0;
  Rng rng_;
};

class World {
 public:
  explicit World(uint64_t seed);
  ~World();

  SimClient& add_client(const std::string& user, const std::string& name, int protocol_version = 0);
  void connect(int ci);
  void client_close(int ci);
  void send_up(int ci, const std::string& data);
  void send_down(int conn, const std::string& data);
  void server_close(int conn, int code);
  void set_net(int ci, bool up);  // false = link failure (both ends notice)
  int64_t latency(const LinkParams& l, bool up);

  // Reference timeline from the server log at true time t.
  lt::Timeline reference(int64_t t) const;
  bool converged(std::string* why = nullptr);
  void heal();

  Sim sim;
  Rng rng;
  SimServer server;
  std::vector<std::unique_ptr<SimClient>> clients;

  struct Conn {
    int client = -1;
    bool open = false;         // established and not closed
    bool server_alive = false; // server has the socket
    int64_t last_up = 0;
    int64_t last_down = 0;
  };
  std::map<int, Conn> conns;
  int next_conn = 1;
  const uint64_t uid;  // distinguishes worlds for the reference cache
};

}  // namespace sim
