#include "protocol.hpp"

namespace lt {

namespace {
struct KindName {
  Kind kind;
  const char* name;
};
constexpr KindName kKinds[] = {
    {Kind::Init, "init"},         {Kind::Join, "join"},         {Kind::Leave, "leave"},
    {Kind::Kick, "kick"},         {Kind::End, "end"},           {Kind::Insert, "insert"},
    {Kind::Remove, "remove"},     {Kind::Move, "move"},         {Kind::Play, "play"},
    {Kind::Pause, "pause"},       {Kind::Seek, "seek"},         {Kind::Next, "next"},
    {Kind::Previous, "previous"}, {Kind::Jump, "jump"},         {Kind::Advanced, "advanced"},
    {Kind::SetRepeat, "set_repeat"}, {Kind::SetShuffle, "set_shuffle"},
};

json::Value nullable(bool is_null, const std::string& s) { return is_null ? json::Value() : json::Value(s); }
}  // namespace

const char* kind_name(Kind k) {
  for (const auto& kn : kKinds) {
    if (kn.kind == k) return kn.name;
  }
  return "unknown";
}

Kind kind_from_name(const std::string& s) {
  for (const auto& kn : kKinds) {
    if (s == kn.name) return kn.kind;
  }
  return Kind::Unknown;
}

bool is_playback_kind(Kind k) {
  switch (k) {
    case Kind::Play:
    case Kind::Pause:
    case Kind::Seek:
    case Kind::Next:
    case Kind::Previous:
    case Kind::Jump:
    case Kind::Advanced:
      return true;
    default:
      return false;
  }
}

const char* repeat_name(Repeat r) {
  switch (r) {
    case Repeat::All: return "all";
    case Repeat::One: return "one";
    default: return "off";
  }
}

bool repeat_from_name(const std::string& s, Repeat& out) {
  if (s == "off") { out = Repeat::Off; return true; }
  if (s == "all") { out = Repeat::All; return true; }
  if (s == "one") { out = Repeat::One; return true; }
  return false;
}

json::Value entry_to_json(const Entry& e, bool with_added_by) {
  json::Value v = json::Value::object();
  v.set("item_id", e.item_id);
  v.set("chat_id", e.chat_id);
  if (!e.chat_username.empty()) v.set("chat_username", e.chat_username);
  v.set("msg_id", e.msg_id);
  v.set("doc_id", e.doc_id);
  v.set("title", e.title);
  v.set("performer", e.performer);
  v.set("duration_ms", e.duration_ms);
  if (with_added_by) v.set("added_by", e.added_by);
  return v;
}

bool entry_from_json(const json::Value& v, Entry& out) {
  if (!v.is_object()) return false;
  out = Entry{};
  out.item_id = v.get_str("item_id");
  out.chat_id = v.get_str("chat_id");
  out.chat_username = v.get_str("chat_username");
  out.msg_id = v.get_int("msg_id");
  out.doc_id = v.get_str("doc_id");
  out.title = v.get_str("title");
  out.performer = v.get_str("performer");
  out.duration_ms = v.get_int("duration_ms");
  out.added_by = v.get_str("added_by");
  return !out.item_id.empty();
}

json::Value op_to_json(const Op& op) {
  json::Value v = json::Value::object();
  if (op.rev > 0) {
    v.set("rev", op.rev);
    v.set("ts", op.ts);
    v.set("by", op.by);
  }
  v.set("op_id", op.op_id);
  v.set("base_rev", op.base_rev);
  v.set("kind", kind_name(op.kind));
  if (op.has_at) v.set("at", op.at);
  switch (op.kind) {
    case Kind::Init:
      v.set("host_chat_id", op.host_chat_id);
      v.set("title", op.title);
      break;
    case Kind::Join:
      v.set("name", op.name);
      break;
    case Kind::Kick:
      v.set("user_id", op.user_id);
      break;
    case Kind::Insert: {
      v.set("after", nullable(op.after_null, op.after));
      json::Value arr = json::Value::array();
      for (const auto& e : op.entries) arr.push(entry_to_json(e, false));
      v.set("entries", std::move(arr));
      break;
    }
    case Kind::Remove: {
      json::Value arr = json::Value::array();
      for (const auto& id : op.item_ids) arr.push(json::Value(id));
      v.set("item_ids", std::move(arr));
      break;
    }
    case Kind::Move:
      v.set("item_id", op.item_id);
      v.set("after", nullable(op.after_null, op.after));
      break;
    case Kind::Play:
    case Kind::Pause:
      if (op.has_item) v.set("item_id", op.item_id);
      v.set("pos", op.pos);
      break;
    case Kind::Seek:
      v.set("item_id", op.item_id);
      v.set("pos", op.pos);
      break;
    case Kind::Next:
      v.set("from_item", op.from_item);
      break;
    case Kind::Previous:
      v.set("from_item", op.from_item);
      v.set("pos", op.pos);
      break;
    case Kind::Jump:
      v.set("item_id", op.item_id);
      break;
    case Kind::Advanced:
      v.set("from_item", op.from_item);
      v.set("to_item", nullable(op.to_item.empty(), op.to_item));
      break;
    case Kind::SetRepeat:
      v.set("mode", repeat_name(op.mode));
      break;
    case Kind::SetShuffle:
      v.set("on", op.on);
      if (op.rev > 0 && op.on) v.set("seed", static_cast<int64_t>(op.seed));
      break;
    default:
      break;
  }
  return v;
}

bool op_from_json(const json::Value& v, Op& out) {
  if (!v.is_object()) return false;
  out = Op{};
  out.rev = v.get_int("rev");
  out.ts = v.get_int("ts");
  out.by = v.get_str("by");
  out.op_id = v.get_str("op_id");
  out.base_rev = v.get_int("base_rev");
  out.kind = kind_from_name(v.get_str("kind"));
  if (const json::Value* at = v.get("at"); at && at->is_number()) {
    out.has_at = true;
    out.at = at->as_int();
  }
  if (const json::Value* it = v.get("item_id"); it && it->is_string()) {
    out.has_item = true;
    out.item_id = it->as_str();
  }
  if (const json::Value* af = v.get("after"); af && af->is_string()) {
    out.after_null = false;
    out.after = af->as_str();
  }
  if (const json::Value* es = v.get("entries"); es && es->is_array()) {
    for (const auto& e : es->items()) {
      Entry en;
      if (entry_from_json(e, en)) out.entries.push_back(std::move(en));
    }
  }
  if (const json::Value* ids = v.get("item_ids"); ids && ids->is_array()) {
    for (const auto& id : ids->items()) {
      if (id.is_string()) out.item_ids.push_back(id.as_str());
    }
  }
  out.from_item = v.get_str("from_item");
  out.to_item = v.get_str("to_item");
  out.pos = v.get_int("pos");
  repeat_from_name(v.get_str("mode"), out.mode);
  out.on = v.get_bool("on");
  out.seed = static_cast<uint32_t>(v.get_int("seed"));
  out.user_id = v.get_str("user_id");
  out.name = v.get_str("name");
  out.host_chat_id = v.get_str("host_chat_id");
  out.title = v.get_str("title");
  return true;
}

std::string encode_op_message(const Op& op) {
  json::Value v = json::Value::object();
  v.set("type", "op");
  json::Value body = op_to_json(op);
  for (auto& m : body.members()) v.members().push_back(std::move(m));
  return json::dump(v);
}

std::string encode_hello(int version, const std::string& token, const std::string& device_id, int64_t since_rev,
                         int64_t t0) {
  json::Value v = json::Value::object();
  v.set("type", "hello");
  v.set("v", version);
  v.set("token", token);
  v.set("device_id", device_id);
  v.set("since_rev", since_rev);
  v.set("t0", t0);
  return json::dump(v);
}

std::string encode_ping(int64_t t0) {
  json::Value v = json::Value::object();
  v.set("type", "ping");
  v.set("t0", t0);
  return json::dump(v);
}

std::string encode_sync(int64_t since_rev) {
  json::Value v = json::Value::object();
  v.set("type", "sync");
  v.set("since_rev", since_rev);
  return json::dump(v);
}

std::string encode_status(const char* state, const char* reason) {
  json::Value v = json::Value::object();
  v.set("type", "status");
  v.set("state", state);
  if (reason && *reason) v.set("reason", reason);
  return json::dump(v);
}

std::string state_digest(const State& s) {
  json::Value v = json::Value::object();
  v.set("creator", s.creator);
  v.set("host_chat_id", s.host_chat_id);
  v.set("title", s.title);
  v.set("ended", s.ended);
  json::Value es = json::Value::array();
  for (const auto& e : s.entries) {
    json::Value j = entry_to_json(e, true);
    j.set("alive", e.alive);
    es.push(std::move(j));
  }
  v.set("entries", std::move(es));
  v.set("shuffle", s.shuffle);
  v.set("seed", static_cast<int64_t>(s.seed));
  json::Value so = json::Value::array();
  for (const auto& id : s.shuffle_order) so.push(json::Value(id));
  v.set("shuffle_order", std::move(so));
  v.set("repeat", repeat_name(s.repeat));
  v.set("cur", s.cur);
  v.set("playing", s.playing);
  v.set("anchor_pos", s.anchor_pos);
  v.set("anchor_time", s.anchor_time);
  v.set("anchor_prev", s.anchor_prev);
  v.set("epoch", static_cast<int64_t>(s.epoch));
  json::Value h = json::Value::array();
  for (const auto& id : s.history) h.push(json::Value(id));
  v.set("history", std::move(h));
  json::Value ps = json::Value::array();
  for (const auto& p : s.participants) {
    json::Value j = json::Value::object();
    j.set("user_id", p.user_id);
    j.set("name", p.name);
    ps.push(std::move(j));
  }
  v.set("participants", std::move(ps));
  return json::dump(v);
}

}  // namespace lt
