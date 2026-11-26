/*
 * Listen Together session core — C API.
 *
 * The core does no I/O. The platform:
 *   - supplies "now" (a monotonic clock in milliseconds) on every call;
 *   - owns the WebSocket and the timers;
 *   - drains output events with lt_core_poll() after every call and acts on
 *     them (send a frame, connect, drive the player, refresh the UI);
 *   - calls lt_core_tick() at or after lt_core_next_timeout().
 *
 * Threading: an lt_core is not thread-safe. Call it from one thread (or
 * serialize calls). Distinct instances are independent.
 *
 * Strings are UTF-8, NUL-terminated. Pointers in an lt_event (and the string
 * returned by lt_core_state_json) stay valid until the next call on the same
 * lt_core. No function throws; errors are returned as LT_ERR_* codes.
 *
 * Protocol: docs/PROTOCOL.md (version LT_PROTOCOL_VERSION).
 */
#ifndef TUNEGRAM_LT_CORE_H
#define TUNEGRAM_LT_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LT_PROTOCOL_VERSION 1

#if defined(_WIN32)
#define LT_API
#elif defined(__GNUC__) || defined(__clang__)
#define LT_API __attribute__((visibility("default")))
#else
#define LT_API
#endif

typedef struct lt_core lt_core;

/* Result codes. */
enum {
  LT_OK = 0,
  LT_ERR_INVALID = -1, /* bad argument */
  LT_ERR_STATE = -2,   /* not possible now (session closed/ended, not creator, leaving) */
  LT_ERR_NOOP = -3,    /* nothing to do (e.g. pause while paused, empty queue) */
  LT_ERR_FULL = -4,    /* too many unconfirmed ops */
  LT_ERR_INTERNAL = -5
};

/* Output event types. */
typedef enum lt_event_type {
  LT_EV_NONE = 0,
  LT_EV_SEND = 1,        /* send `data`/`len` as one WebSocket text frame */
  LT_EV_CONNECT = 2,     /* open the session WebSocket; report lt_core_on_open / lt_core_on_close */
  LT_EV_DISCONNECT = 3,  /* close the socket; ignore its further callbacks */
  LT_EV_PLAYER_LOAD = 10,    /* prepare `item_id` (entry fields set) without playing; report ready */
  LT_EV_PLAYER_PLAY = 11,    /* play `item_id` from `pos_ms` (valid at local time `ref_ms`) */
  LT_EV_PLAYER_PAUSE = 12,   /* pause and park at `pos_ms` */
  LT_EV_PLAYER_SEEK = 13,    /* seek to `pos_ms` (valid at `ref_ms`), keep play/pause */
  LT_EV_PLAYER_STOP = 14,    /* stop and release the session item */
  LT_EV_PLAYER_PRELOAD = 15, /* hint: prefetch `item_id` (entry fields set) */
  LT_EV_PLAYER_QUERY = 16,   /* report the position now with lt_core_player_position */
  LT_EV_STATE_CHANGED = 20,  /* re-read lt_core_state_json */
  LT_EV_ERROR = 21           /* `code` (+ `data` message); `fatal` = session over for this device */
} lt_event_type;

typedef struct lt_event {
  int32_t type; /* lt_event_type */
  const char* data;
  size_t len;
  const char* code;
  int32_t fatal;
  const char* item_id;
  int64_t pos_ms;
  int64_t ref_ms;
  /* Queue entry, for PLAYER_LOAD / PLAYER_PRELOAD. */
  const char* chat_id;
  int64_t msg_id;
  const char* doc_id;
  const char* title;
  const char* performer;
  int64_t duration_ms;
} lt_event;

typedef struct lt_config {
  const char* device_id; /* stable per install, <= 64 chars */
  const char* token;     /* session token from POST /v1/auth */
  int32_t protocol_version; /* 0 = LT_PROTOCOL_VERSION */
  uint64_t seed;            /* randomness for ids/backoff; any value */
} lt_config;

typedef struct lt_entry {
  const char* item_id; /* NULL = generate */
  const char* chat_id;
  int64_t msg_id;
  const char* doc_id;
  const char* title;
  const char* performer;
  int64_t duration_ms;
} lt_entry;

typedef enum lt_repeat { LT_REPEAT_OFF = 0, LT_REPEAT_ALL = 1, LT_REPEAT_ONE = 2 } lt_repeat;

typedef struct lt_playback {
  const char* item_id; /* NULL if nothing is current */
  int64_t pos_ms;
  int32_t playing;
  int64_t server_time_ms;
} lt_playback;

/* Lifecycle */
LT_API lt_core* lt_core_create(const lt_config* config); /* NULL on bad config */
LT_API void lt_core_destroy(lt_core* core);
LT_API const char* lt_core_version(void);

/* Transport */
LT_API void lt_core_start(lt_core* core, int64_t now_ms);
LT_API void lt_core_on_open(lt_core* core, int64_t now_ms);
LT_API void lt_core_on_close(lt_core* core, int64_t now_ms, int32_t close_code);
LT_API void lt_core_on_message(lt_core* core, int64_t now_ms, const char* data, size_t len);
LT_API void lt_core_tick(lt_core* core, int64_t now_ms);
LT_API int64_t lt_core_next_timeout(const lt_core* core); /* absolute local ms, -1 = none */
LT_API void lt_core_set_token(lt_core* core, int64_t now_ms, const char* token);

/* Player reports */
LT_API void lt_core_player_ready(lt_core* core, int64_t now_ms, const char* item_id);
LT_API void lt_core_player_started(lt_core* core, int64_t now_ms, const char* item_id, int64_t pos_ms);
LT_API void lt_core_player_position(lt_core* core, int64_t now_ms, const char* item_id, int64_t pos_ms, int32_t playing);
LT_API void lt_core_player_ended(lt_core* core, int64_t now_ms, const char* item_id);
LT_API void lt_core_player_buffering(lt_core* core, int64_t now_ms, const char* item_id, int32_t buffering);

/* Commands (local-first). Return LT_OK or LT_ERR_*. */
LT_API int32_t lt_core_play(lt_core* core, int64_t now_ms);
LT_API int32_t lt_core_pause(lt_core* core, int64_t now_ms);
LT_API int32_t lt_core_seek(lt_core* core, int64_t now_ms, int64_t pos_ms);
LT_API int32_t lt_core_next(lt_core* core, int64_t now_ms);
LT_API int32_t lt_core_previous(lt_core* core, int64_t now_ms);
LT_API int32_t lt_core_jump(lt_core* core, int64_t now_ms, const char* item_id);
/* after_item_id NULL = insert at the head. More than 100 entries are split
 * into several ops. If out_ids is non-NULL, it receives the item ids as a JSON
 * array string valid until the next call. */
LT_API int32_t lt_core_insert(lt_core* core, int64_t now_ms, const char* after_item_id, const lt_entry* entries,
                       size_t count, const char** out_ids_json);
LT_API int32_t lt_core_remove(lt_core* core, int64_t now_ms, const char* const* item_ids, size_t count);
LT_API int32_t lt_core_move(lt_core* core, int64_t now_ms, const char* item_id, const char* after_item_id);
LT_API int32_t lt_core_set_repeat(lt_core* core, int64_t now_ms, int32_t mode /* lt_repeat */);
LT_API int32_t lt_core_set_shuffle(lt_core* core, int64_t now_ms, int32_t on);
LT_API int32_t lt_core_kick(lt_core* core, int64_t now_ms, const char* user_id);
LT_API int32_t lt_core_end_session(lt_core* core, int64_t now_ms);
LT_API int32_t lt_core_leave(lt_core* core, int64_t now_ms);
/* Phone call etc.: pause locally without pausing others; 0 = catch up. */
LT_API void lt_core_set_interrupted(lt_core* core, int64_t now_ms, int32_t interrupted);

/* Output */
LT_API int32_t lt_core_poll(lt_core* core, lt_event* out); /* 1 = event written, 0 = queue empty */

/* State */
LT_API const char* lt_core_state_json(lt_core* core, int64_t now_ms);
LT_API lt_playback lt_core_playback(lt_core* core, int64_t now_ms);
LT_API int64_t lt_core_server_time(const lt_core* core, int64_t now_ms);
LT_API int64_t lt_core_clock_offset(const lt_core* core);
LT_API int64_t lt_core_rev(const lt_core* core);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TUNEGRAM_LT_CORE_H */
