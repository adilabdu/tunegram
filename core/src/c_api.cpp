// extern "C" wrapper around lt::Session. No exception crosses this boundary:
// the core is written without throwing, and when exceptions are enabled every
// entry point also catches (std::bad_alloc etc.) and reports LT_ERR_INTERNAL.
#include "tunegram/lt_core.h"

#include <new>
#include <string>
#include <vector>

#include "internal.hpp"
#include "json.hpp"
#include "session.hpp"

struct lt_core {
  explicit lt_core(lt::Config c) : session(std::move(c)) {}
  lt::Session session;
  lt::Event current;
  std::string state_buf;
  std::string ids_buf;
  std::string playback_item;
};

#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
#define LT_TRY try {
#define LT_CATCH(ret) \
  }                   \
  catch (...) {       \
    return ret;       \
  }
#define LT_CATCH_VOID \
  }                   \
  catch (...) {       \
  }
#else
#define LT_TRY {
#define LT_CATCH(ret) }
#define LT_CATCH_VOID }
#endif

namespace {
std::string str(const char* s) { return s ? std::string(s) : std::string(); }
}  // namespace

lt::Session* lt::session_of(lt_core* core) { return core ? &core->session : nullptr; }

extern "C" {

const char* lt_core_version(void) { return "tunegram-lt-core/1 (protocol 1)"; }

lt_core* lt_core_create(const lt_config* config) {
  LT_TRY
  if (!config || !config->device_id || !config->device_id[0]) return nullptr;
  lt::Config c;
  c.device_id = config->device_id;
  if (c.device_id.size() > 64) return nullptr;
  c.token = str(config->token);
  c.protocol_version = config->protocol_version > 0 ? config->protocol_version : LT_PROTOCOL_VERSION;
  c.seed = config->seed;
  return new (std::nothrow) lt_core(std::move(c));
  LT_CATCH(nullptr)
}

void lt_core_destroy(lt_core* core) { delete core; }

void lt_core_start(lt_core* core, int64_t now_ms) {
  if (!core) return;
  LT_TRY core->session.start(now_ms);
  LT_CATCH_VOID
}

void lt_core_on_open(lt_core* core, int64_t now_ms) {
  if (!core) return;
  LT_TRY core->session.on_open(now_ms);
  LT_CATCH_VOID
}

void lt_core_on_close(lt_core* core, int64_t now_ms, int32_t close_code) {
  if (!core) return;
  LT_TRY core->session.on_close(now_ms, close_code);
  LT_CATCH_VOID
}

void lt_core_on_message(lt_core* core, int64_t now_ms, const char* data, size_t len) {
  if (!core || !data) return;
  LT_TRY core->session.on_message(now_ms, std::string_view(data, len));
  LT_CATCH_VOID
}

void lt_core_tick(lt_core* core, int64_t now_ms) {
  if (!core) return;
  LT_TRY core->session.tick(now_ms);
  LT_CATCH_VOID
}

int64_t lt_core_next_timeout(const lt_core* core) { return core ? core->session.next_timeout() : -1; }

void lt_core_set_token(lt_core* core, int64_t now_ms, const char* token) {
  if (!core) return;
  LT_TRY core->session.set_token(now_ms, str(token));
  LT_CATCH_VOID
}

void lt_core_player_ready(lt_core* core, int64_t now_ms, const char* item_id) {
  if (!core) return;
  LT_TRY core->session.player_ready(now_ms, str(item_id));
  LT_CATCH_VOID
}

void lt_core_player_started(lt_core* core, int64_t now_ms, const char* item_id, int64_t pos_ms) {
  if (!core) return;
  LT_TRY core->session.player_started(now_ms, str(item_id), pos_ms);
  LT_CATCH_VOID
}

void lt_core_player_position(lt_core* core, int64_t now_ms, const char* item_id, int64_t pos_ms, int32_t playing) {
  if (!core) return;
  LT_TRY core->session.player_position(now_ms, str(item_id), pos_ms, playing != 0);
  LT_CATCH_VOID
}

void lt_core_player_ended(lt_core* core, int64_t now_ms, const char* item_id) {
  if (!core) return;
  LT_TRY core->session.player_ended(now_ms, str(item_id));
  LT_CATCH_VOID
}

void lt_core_player_buffering(lt_core* core, int64_t now_ms, const char* item_id, int32_t buffering) {
  if (!core) return;
  LT_TRY core->session.player_buffering(now_ms, str(item_id), buffering != 0);
  LT_CATCH_VOID
}

void lt_core_player_failed(lt_core* core, int64_t now_ms, const char* item_id) {
  if (!core) return;
  LT_TRY core->session.player_failed(now_ms, str(item_id));
  LT_CATCH_VOID
}

int32_t lt_core_play(lt_core* core, int64_t now_ms) {
  if (!core) return LT_ERR_INVALID;
  LT_TRY return core->session.cmd_play(now_ms);
  LT_CATCH(LT_ERR_INTERNAL)
}

int32_t lt_core_pause(lt_core* core, int64_t now_ms) {
  if (!core) return LT_ERR_INVALID;
  LT_TRY return core->session.cmd_pause(now_ms);
  LT_CATCH(LT_ERR_INTERNAL)
}

int32_t lt_core_seek(lt_core* core, int64_t now_ms, int64_t pos_ms) {
  if (!core) return LT_ERR_INVALID;
  LT_TRY return core->session.cmd_seek(now_ms, pos_ms);
  LT_CATCH(LT_ERR_INTERNAL)
}

int32_t lt_core_next(lt_core* core, int64_t now_ms) {
  if (!core) return LT_ERR_INVALID;
  LT_TRY return core->session.cmd_next(now_ms);
  LT_CATCH(LT_ERR_INTERNAL)
}

int32_t lt_core_previous(lt_core* core, int64_t now_ms) {
  if (!core) return LT_ERR_INVALID;
  LT_TRY return core->session.cmd_previous(now_ms);
  LT_CATCH(LT_ERR_INTERNAL)
}

int32_t lt_core_jump(lt_core* core, int64_t now_ms, const char* item_id) {
  if (!core || !item_id) return LT_ERR_INVALID;
  LT_TRY return core->session.cmd_jump(now_ms, item_id);
  LT_CATCH(LT_ERR_INTERNAL)
}

int32_t lt_core_insert(lt_core* core, int64_t now_ms, const char* after_item_id, const lt_entry* entries,
                       size_t count, const char** out_ids_json) {
  if (!core || !entries || count == 0) return LT_ERR_INVALID;
  LT_TRY
  std::vector<lt::Entry> es;
  es.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    lt::Entry e;
    e.item_id = str(entries[i].item_id);
    e.chat_id = str(entries[i].chat_id);
    e.chat_username = str(entries[i].chat_username);
    e.msg_id = entries[i].msg_id;
    e.doc_id = str(entries[i].doc_id);
    e.title = str(entries[i].title);
    e.performer = str(entries[i].performer);
    e.duration_ms = entries[i].duration_ms;
    es.push_back(std::move(e));
  }
  std::string after = str(after_item_id);
  std::vector<std::string> ids;
  int32_t r = core->session.cmd_insert(now_ms, after_item_id ? &after : nullptr, std::move(es), &ids);
  if (out_ids_json) {
    lt::json::Value arr = lt::json::Value::array();
    for (const auto& id : ids) arr.push(lt::json::Value(id));
    core->ids_buf = lt::json::dump(arr);
    *out_ids_json = core->ids_buf.c_str();
  }
  return r;
  LT_CATCH(LT_ERR_INTERNAL)
}

int32_t lt_core_remove(lt_core* core, int64_t now_ms, const char* const* item_ids, size_t count) {
  if (!core || !item_ids || count == 0) return LT_ERR_INVALID;
  LT_TRY
  std::vector<std::string> ids;
  for (size_t i = 0; i < count; ++i) {
    if (item_ids[i]) ids.emplace_back(item_ids[i]);
  }
  return core->session.cmd_remove(now_ms, std::move(ids));
  LT_CATCH(LT_ERR_INTERNAL)
}

int32_t lt_core_move(lt_core* core, int64_t now_ms, const char* item_id, const char* after_item_id) {
  if (!core || !item_id) return LT_ERR_INVALID;
  LT_TRY
  std::string after = str(after_item_id);
  return core->session.cmd_move(now_ms, item_id, after_item_id ? &after : nullptr);
  LT_CATCH(LT_ERR_INTERNAL)
}

int32_t lt_core_set_repeat(lt_core* core, int64_t now_ms, int32_t mode) {
  if (!core || mode < 0 || mode > 2) return LT_ERR_INVALID;
  LT_TRY return core->session.cmd_set_repeat(now_ms, static_cast<lt::Repeat>(mode));
  LT_CATCH(LT_ERR_INTERNAL)
}

int32_t lt_core_set_shuffle(lt_core* core, int64_t now_ms, int32_t on) {
  if (!core) return LT_ERR_INVALID;
  LT_TRY return core->session.cmd_set_shuffle(now_ms, on != 0);
  LT_CATCH(LT_ERR_INTERNAL)
}

int32_t lt_core_kick(lt_core* core, int64_t now_ms, const char* user_id) {
  if (!core || !user_id) return LT_ERR_INVALID;
  LT_TRY return core->session.cmd_kick(now_ms, user_id);
  LT_CATCH(LT_ERR_INTERNAL)
}

int32_t lt_core_end_session(lt_core* core, int64_t now_ms) {
  if (!core) return LT_ERR_INVALID;
  LT_TRY return core->session.cmd_end(now_ms);
  LT_CATCH(LT_ERR_INTERNAL)
}

int32_t lt_core_leave(lt_core* core, int64_t now_ms) {
  if (!core) return LT_ERR_INVALID;
  LT_TRY return core->session.cmd_leave(now_ms);
  LT_CATCH(LT_ERR_INTERNAL)
}

void lt_core_interruption_began(lt_core* core, int64_t now_ms, int32_t reason) {
  if (!core) return;
  LT_TRY core->session.interruption_began(now_ms, reason);
  LT_CATCH_VOID
}

void lt_core_interruption_ended(lt_core* core, int64_t now_ms) {
  if (!core) return;
  LT_TRY core->session.interruption_ended(now_ms);
  LT_CATCH_VOID
}

void lt_core_catch_up(lt_core* core, int64_t now_ms) {
  if (!core) return;
  LT_TRY core->session.catch_up(now_ms);
  LT_CATCH_VOID
}

void lt_core_set_interrupted(lt_core* core, int64_t now_ms, int32_t interrupted) {
  if (!core) return;
  LT_TRY core->session.set_interrupted(now_ms, interrupted != 0);
  LT_CATCH_VOID
}

int32_t lt_core_poll(lt_core* core, lt_event* out) {
  if (!core || !out) return 0;
  LT_TRY
  if (!core->session.poll(core->current)) return 0;
  const lt::Event& e = core->current;
  *out = lt_event{};
  out->type = static_cast<int32_t>(e.type);
  out->data = e.data.c_str();
  out->len = e.data.size();
  out->code = e.code.c_str();
  out->fatal = e.fatal ? 1 : 0;
  out->item_id = e.item_id.c_str();
  out->pos_ms = e.pos;
  out->ref_ms = e.ref;
  out->chat_id = e.entry.chat_id.c_str();
  out->chat_username = e.entry.chat_username.c_str();
  out->msg_id = e.entry.msg_id;
  out->doc_id = e.entry.doc_id.c_str();
  out->title = e.entry.title.c_str();
  out->performer = e.entry.performer.c_str();
  out->duration_ms = e.entry.duration_ms;
  return 1;
  LT_CATCH(0)
}

const char* lt_core_state_json(lt_core* core, int64_t now_ms) {
  if (!core) return "{}";
  LT_TRY
  core->state_buf = core->session.state_json(now_ms);
  return core->state_buf.c_str();
  LT_CATCH("{}")
}

lt_playback lt_core_playback(lt_core* core, int64_t now_ms) {
  lt_playback pb{};
  if (!core) return pb;
  LT_TRY
  lt::Timeline t = core->session.playback(now_ms);
  core->playback_item = t.item;
  pb.item_id = t.item.empty() ? nullptr : core->playback_item.c_str();
  pb.pos_ms = t.pos;
  pb.playing = t.playing ? 1 : 0;
  pb.server_time_ms = core->session.server_now(now_ms);
  return pb;
  LT_CATCH(pb)
}

int64_t lt_core_server_time(const lt_core* core, int64_t now_ms) {
  return core ? core->session.server_now(now_ms) : now_ms;
}

int64_t lt_core_clock_offset(const lt_core* core) { return core ? core->session.clock().offset() : 0; }

int64_t lt_core_rev(const lt_core* core) { return core ? core->session.confirmed_rev() : 0; }

}  // extern "C"
