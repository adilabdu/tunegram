// Replicated session model: queue entries, ops/records and reducer state.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lt {

constexpr int kProtocolVersion = 1;

// Reducer constants (PROTOCOL.md §9, §13). Changing any of them changes the
// reducer and therefore needs a protocol version bump.
constexpr int64_t kStalePlaybackMs = 5000;
constexpr int64_t kPreviousRestartMs = 3000;
constexpr int64_t kAdvanceWindowMs = 3000;
constexpr size_t kMaxEntries = 3000;
constexpr size_t kMaxHistory = 200;

enum class Repeat : uint8_t { Off = 0, All = 1, One = 2 };

struct Entry {
  std::string item_id;
  std::string chat_id;
  std::string chat_username;  // public channel username (no @) so non-members can resolve it; may be empty
  int64_t msg_id = 0;
  std::string doc_id;
  std::string title;
  std::string performer;
  int64_t duration_ms = 0;
  std::string added_by;
  bool alive = true;
};

struct Participant {
  std::string user_id;
  std::string name;
};

enum class Kind : uint8_t {
  Unknown,
  Init,
  Join,
  Leave,
  Kick,
  End,
  Insert,
  Remove,
  Move,
  Play,
  Pause,
  Seek,
  Next,
  Previous,
  Jump,
  Advanced,
  SetRepeat,
  SetShuffle,
};

const char* kind_name(Kind k);
Kind kind_from_name(const std::string& s);
bool is_playback_kind(Kind k);
const char* repeat_name(Repeat r);
bool repeat_from_name(const std::string& s, Repeat& out);

// A client op (rev == 0) or a server-stamped log record (rev > 0).
struct Op {
  int64_t rev = 0;
  int64_t ts = 0;
  std::string by;
  std::string op_id;
  int64_t base_rev = 0;
  Kind kind = Kind::Unknown;
  bool has_at = false;
  int64_t at = 0;

  // Kind-specific arguments.
  bool has_item = false;
  std::string item_id;           // play pause seek jump move
  bool after_null = true;        // insert move: null => head
  std::string after;
  std::vector<Entry> entries;    // insert
  std::vector<std::string> item_ids;  // remove
  std::string from_item;         // next previous advanced
  std::string to_item;           // advanced (advisory)
  int64_t pos = 0;               // play pause seek previous
  Repeat mode = Repeat::Off;     // set_repeat
  bool on = false;               // set_shuffle
  uint32_t seed = 0;             // set_shuffle (server)
  std::string user_id;           // kick
  std::string name;              // join
  std::string host_chat_id;      // init
  std::string title;             // init
};

struct State {
  int64_t rev = 0;
  std::string creator;
  std::string host_chat_id;
  std::string title;
  bool ended = false;

  std::vector<Entry> entries;  // base order, including tombstones
  bool shuffle = false;
  uint32_t seed = 0;
  std::vector<std::string> shuffle_order;
  Repeat repeat = Repeat::Off;

  // Playback anchor.
  std::string cur;
  bool playing = false;
  int64_t anchor_pos = 0;
  int64_t anchor_time = 0;
  uint64_t epoch = 0;
  std::string anchor_prev;  // set when materialization crossed a boundary into cur

  std::vector<std::string> history;
  std::vector<Participant> participants;
};

}  // namespace lt
