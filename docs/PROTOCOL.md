# Listen Together — wire protocol (v1)

_Status: implemented by `core/` (C++ client core + reference sequencer used in tests) and `backend/` (Cloudflare Worker + Durable Object). Companion to [PLAN.md](PLAN.md) §3–§4._

## 1. Model in one paragraph

The server is a **sequencer plus log**. It authenticates clients, validates message shape and size, stamps every accepted op with a monotonically increasing `rev` and the server time `ts`, appends it to the session's op log and broadcasts it. It never interprets queue or playback semantics. **All semantics live in one deterministic reducer** in the C++ core (§9): every client folds the same ordered log into the same state, and so can the server later (the core compiled to wasm, §15). Clients apply their own ops immediately (local-first) and rebase onto the server order as events arrive (§7).

## 2. Transport and framing

- One WebSocket per device per session, over TLS in production: `wss://<host>/v1/sessions/<session_id>/ws`.
- Every frame is a **text frame holding one JSON object** (UTF-8). Binary frames are rejected with `bad_request`.
- Every message has a string field `type`. Unknown fields are ignored by both sides (forward compatibility). Unknown `type`s from the server are ignored by clients; unknown `type`s from clients get `error{code:"bad_request"}` (non-fatal).
- All times are integers in **milliseconds**. Server times are Unix epoch ms (`Date.now()` on the server). Client "local" times are whatever monotonic clock the platform supplies; they never go on the wire except as ping `t0` (echoed back untouched).
- All Telegram identifiers that can exceed 2^53 travel as **decimal strings**: `user_id`, `chat_id`, `doc_id`. `msg_id` (32-bit) is a number.
- Limits (server-enforced, see §13): inbound frame ≤ 64 KiB; ≤ 100 entries per `insert`; string limits in §8.4.

## 3. HTTP endpoints

All bodies are JSON. Errors are `{"error":{"code":"<code>","message":"<text>"}}` with the HTTP status listed.

| Method + path | Auth | Body | Success |
|---|---|---|---|
| `POST /v1/auth` | — | `{"init_data":"<raw Telegram initData query string>"}` | `200 {"token","expires_at","user":{"id","name","username"}}` |
| `POST /v1/sessions` | Bearer | `{"host_chat_id":"-1001234","title":"optional"}` | `201 {"session_id","ws_path"}` |
| `POST /v1/sessions/:id/invites` | Bearer, participant or creator | `{"ttl_s":3600}` (optional, 60…86400, default 21600) | `201 {"invite_token","link","expires_at"}` |
| `POST /v1/invites/redeem` | Bearer | `{"invite_token":"…"}` | `200 {"session_id","host_chat_id","title","ws_path"}` |
| `GET /v1/sessions/:id/ws` | (in `hello`) | WebSocket upgrade | `101` |
| `GET /health` | — | — | `200 ok` |

Bearer = `Authorization: Bearer <token>` from `/v1/auth`.

### 3.1 `POST /v1/auth` — Telegram Mini App `initData`

The fork silently opens our bot's Mini App URL and receives Telegram-signed `initData` (PLAN §4.4). The server accepts it if **either** check passes:

1. **Ed25519 (third-party validation, preferred — needs no bot token on the server).** Requires a `signature` field.
   - data-check-string = `"<bot_id>:WebAppData\n"` + all fields except `hash` and `signature`, as `key=value` (value URL-decoded), sorted by key, joined with `\n`.
   - `signature` is base64url (no padding) of a 64-byte Ed25519 signature over the data-check-string.
   - Public keys: production `e7bf03a2fa4602af4580703d88dda5bb59f32ed8b02a56c187fe7d34caed242d`, test environment `40055058a4ee38156a06562e52eece92a771bcd8346a8c4615cb7376eddf72ec`. Selected by `TELEGRAM_ENV=prod|test`; tests override the key with a generated one.
2. **HMAC-SHA256 (bot-token path).** Requires `hash` and the `BOT_TOKEN` secret.
   - secret_key = HMAC-SHA256(key=`"WebAppData"`, msg=bot_token).
   - data-check-string = all fields except `hash` (so `signature`, when present, **is** included), `key=value`, sorted, joined with `\n`.
   - valid iff lowercase hex HMAC-SHA256(secret_key, data-check-string) == `hash` (constant-time compare).

Then: `auth_date` must satisfy `now - 3600 s ≤ auth_date ≤ now + 60 s` (`INIT_DATA_MAX_AGE_S`, default 3600), and `user` must be JSON with a numeric `id`. Failure → `401 bad_init_data` / `401 init_data_expired`.

Success issues a **session token** (default TTL 3600 s, `TOKEN_TTL_S`):

```
token = "v1." + b64url(payload) + "." + b64url(HMAC-SHA256(TOKEN_SECRET, "v1." + b64url(payload)))
payload = {"sub":"<telegram user id>","name":"<first_name>","iat":<s>,"exp":<s>}
```

Tokens are only checked when a request or `hello` arrives; an open socket is not cut when its token expires. Reconnecting with an expired token yields `error{code:"unauthorized"}` and the platform fetches fresh `initData`.

### 3.2 Sessions and invites

- `session_id`: 16 random bytes, base64url (22 chars). One Durable Object per session id (single writer).
- Creating a session appends rev 1, `init{host_chat_id,title}` with `by` = creator, and makes the creator a **member**.
- **Members** may connect. **Participants** are members currently in the session (joined and not left/kicked). Participant cap: `MAX_PARTICIPANTS` (default 10). Several devices of one user count once.
- `invite_token` = b64url(16-byte session id ‖ 16-byte secret) (43 chars, valid `startapp` charset). The DO stores SHA-256(secret) with `expires_at`; **single use**: the first successful redeem marks it used.
- `link` = `https://t.me/<BOT_USERNAME>/<MINI_APP_SHORT_NAME>?startapp=<invite_token>`. Forks intercept it natively; official clients open the Mini App landing page.
- Redeem errors: `404 invite_invalid` (malformed/unknown), `410 invite_used`, `410 invite_expired`, `409 session_full`, `410 session_ended`, `403 kicked` (a kicked user cannot rejoin).
- Redeeming as an existing member is allowed and still consumes the token.

## 4. Connection lifecycle

```
client                                   server (session DO)
  | --- WebSocket upgrade --------------->|
  | --- hello{v,token,device_id,since_rev,t0} ->|  verify version, token, membership, cap
  | <-- welcome{…,head_rev,t0,t1,t2} -----|  (+ join op appended if user was not a participant)
  | <-- log{ops since since_rev}… --------|  one or more chunks, `more` on all but the last
  | <-- presence{…} (others) -------------|
  | --- ping{t0} ------------------------>|
  | <-- pong{t0,t1,t2,head_rev} ----------|
  | --- op{op_id,kind,at,…} ------------->|  stamp rev/ts/by, append, broadcast
  | <-- event{op:{rev,ts,by,op_id,…}} ----|  to every connected device, sender included
```

### 4.1 Client → server

| `type` | Fields | Notes |
|---|---|---|
| `hello` | `v` (int), `token`, `device_id` (≤ 64 chars), `since_rev` (int ≥ 0), `t0` (local ms, optional) | Must be the first message. Anything else before it → fatal `unauthorized`. A socket that hasn't sent `hello` within 10 s is closed. |
| `ping` | `t0` | Clock sample (§6). |
| `op` | `op_id`, `kind`, `base_rev`, `at` (playback kinds), kind args (§8) | |
| `sync` | `since_rev` | Ask for log records with `rev > since_rev` (gap repair). |
| `status` | `state`: `"active"` or `"interrupted"` | Rebroadcast as `presence` (e.g. phone call). Not logged. |

### 4.2 Server → client

| `type` | Fields |
|---|---|
| `welcome` | `v`, `session_id`, `user_id`, `creator`, `head_rev`, `server_time`, `t0` (echo), `t1`, `t2`, `max_participants`, `online` (`[{user_id,device_id,state}]`) |
| `log` | `since_rev`, `head_rev`, `server_time`, `ops` (records, ascending `rev`), `more` (bool) |
| `event` | `server_time`, `op` (one record) |
| `pong` | `t0` (echo), `t1` (server receive), `t2` (server send), `head_rev` |
| `ack` | `op_id`, `rev`, `dup:true` — only for a **duplicate** op_id (§7.3) |
| `reject` | `op_id`, `code`, `message` — op not accepted (§12) |
| `presence` | `user_id`, `device_id`, `online` (bool), `state`, `server_time` |
| `error` | `code`, `message`, `fatal` (bool), plus `min_v`/`max_v` for `version_unsupported`. Fatal errors are followed by a close (§12). |

`snapshot{rev,state}` is reserved for compaction (§15) and not sent in v1: the log *is* the snapshot.

## 5. Records (the log)

A client op becomes a **record** when the server accepts it. The server copies the op's fields and adds:

| Field | Meaning |
|---|---|
| `rev` | 1, 2, 3, … per session, no gaps, never reused |
| `ts` | server time when accepted |
| `by` | `user_id` from the token (clients cannot choose it); `""` for server-originated ops |
| `seed` | only on `set_shuffle` with `on:true`: random uint32 chosen by the server (§9.8) |

The server drops `type` and any `by`/`rev`/`ts`/`seed` the client sent. Server-originated ops (`init`, `join`, idle `end`) use `op_id = "s:<rev>"`.

Example record:

```json
{"rev":42,"ts":1764190000123,"by":"5550001","op_id":"d7f3:1c:9a2b","base_rev":41,
 "kind":"seek","at":1764190000071,"item_id":"d7f3.12.k9x","pos":61000}
```

## 6. Clock sync

Same socket, ping/pong. The client sends `ping{t0}` (local ms); the server replies `pong{t0,t1,t2}` (t1 = receive, t2 = send, server ms; in Workers t1 = t2). On receipt at local `t3`:

```
rtt    = (t3 - t0) - (t2 - t1)
offset = ((t1 - t0) + (t2 - t3)) / 2          // server_time ≈ local + offset
```

- `hello.t0` / `welcome.t1,t2` form the first sample, so a joiner has an estimate before the log arrives.
- Schedule: a burst of 5 pings 150 ms apart after `welcome`, then one every 15 s.
- Estimator: keep the 12 most recent samples, drop samples with `rtt > 3000`, use the **offset of the lowest-RTT sample**. Error is bounded by half the path asymmetry plus the RTT of that sample's spread; typically a few ms.
- Server time is in every `welcome`, `log`, `event` (`server_time` and record `ts`) and `pong`.

## 7. Ordering, rev and idempotency

### 7.1 Server rules

1. Records get `rev = head_rev + 1` in the order the DO processes messages (single writer). `head_rev` only grows.
2. A record is persisted before it is broadcast.
3. Every connected, authenticated device receives every new record exactly once per connection as `event`, **including the sender**. The sender's own event is its confirmation.
4. On `hello{since_rev:n}` or `sync{since_rev:n}` the server sends all records with `rev > n` in ascending order (chunked ≤ 500 records per `log`). `n = 0` means from the start. `n > head_rev` is treated as `0` (the client's state is from another incarnation and must be rebuilt).

### 7.2 Client rules

- Keep `confirmed` = reducer state after records `1..confirmed_rev`, and `pending` = own ops not yet seen in the log, in creation order.
- The visible state (`view`) = `confirmed` + replay of `pending` through the same reducer (local-first; dragging feels instant).
- Record with `rev == confirmed_rev + 1`: apply to `confirmed`, drop the pending op with that `op_id` if any, rebuild `view`. `rev ≤ confirmed_rev`: ignore (duplicate delivery). `rev > confirmed_rev + 1`: buffer; if the gap isn't filled within 1 s, send `sync{since_rev:confirmed_rev}`.
- A `pong.head_rev` or `ack.rev` above `confirmed_rev` that stays unfilled for 1 s also triggers `sync`.
- Pending ops are (re)sent after every `welcome`, and resent if unconfirmed 5 s after sending. Same `op_id` every time.

### 7.3 Idempotency

- `op_id` is client-generated, unique per session: `<device tag>:<counter base36>:<random base36>` (≤ 64 chars).
- The server keeps every `op_id` in the log. A repeated `op_id` is **not** appended; the server replies `ack{op_id,rev,dup:true}` with the original `rev`. The client drops the pending op once `confirmed_rev ≥ rev`.
- Clients ignore records they already applied (`rev ≤ confirmed_rev`).
- The reducer itself is idempotent where it matters: inserting an existing `item_id` is skipped, removing a removed item is a no-op, and `next`/`previous`/`advanced` name the item they act on, so a concurrent duplicate finds a different current item and becomes a no-op.

## 8. Ops

### 8.1 Envelope (client → server)

```json
{"type":"op","op_id":"d7f3:1d:x1","base_rev":42,"kind":"pause","at":1764190003000,"item_id":"d7f3.12.k9x","pos":64000}
```

- `base_rev`: the sender's `confirmed_rev` when it created the op. Informational (debugging, metrics); the reducer ignores it.
- `at`: **required for playback kinds** (`play pause seek next previous jump advanced`): the sender's estimate of server time when the user acted (local time + clock offset). Ignored for other kinds.

### 8.2 Kinds

| kind | args | sent by | effect (details §9) |
|---|---|---|---|
| `init` | `host_chat_id`, `title` | server (rev 1) | session metadata; `by` = creator |
| `join` | `name` | server | add participant `by` |
| `leave` | — | any participant | remove participant `by`; server closes that user's sockets |
| `kick` | `user_id` | creator only | remove participant; server closes their sockets with `kicked` |
| `end` | — | creator, or server on idle | session over; server closes all sockets |
| `insert` | `after` (item_id or `null` = head), `entries[]` (1…100) | any | add entries |
| `remove` | `item_ids[]` (1…100) | any | remove entries |
| `move` | `item_id`, `after` (item_id or `null`) | any | reorder |
| `play` | `item_id` (optional), `pos` | any | play from `pos` |
| `pause` | `item_id` (optional), `pos` | any | pause at `pos` |
| `seek` | `item_id`, `pos` | any | seek, keep play/pause |
| `next` | `from_item` | any | go to next if `from_item` is current |
| `previous` | `from_item`, `pos` | any | restart or go back |
| `jump` | `item_id` | any | play this entry from 0 |
| `advanced` | `from_item`, `to_item` (advisory, may be `null`) | any (automatic) | re-anchor at a real end of track |
| `set_repeat` | `mode`: `off`/`all`/`one` | any | |
| `set_shuffle` | `on` (bool) | any | server adds `seed` when `on` |

### 8.3 Queue entry

```json
{"item_id":"d7f3.12.k9x","chat_id":"-1001234567890","msg_id":5521,"doc_id":"5368712345678901234",
 "title":"Song","performer":"Artist","duration_ms":215000}
```

`item_id` is client-generated and unique for the session lifetime (re-adding a track makes a new id). `added_by` is **not** sent: the reducer sets it from the record's `by`.

### 8.4 Field limits (server validation → `reject{code:"bad_request"}`)

`op_id`, `item_id`, `from_item`, `to_item`, `after`, `user_id`, `device_id`: 1–64 chars. `chat_id`, `doc_id`: 1–32 chars. `title`, `performer`, `name`: ≤ 256 chars. `host_chat_id`: ≤ 32 chars. Integers (`msg_id`, `duration_ms`, `pos`, `at`, `base_rev`): finite, `0 ≤ x ≤ 2^53`. `mode` ∈ {off, all, one}. `on` boolean.

## 9. Reducer semantics (normative)

The reducer is `apply(state, record) → state`. It is pure and total: invalid references become no-ops, never errors. Only integers, no floats, no wall-clock reads.

### 9.1 State

- `entries`: ordered list (the **base order**) of entries, each `alive` or a **tombstone** (removed; kept so anchors to it still resolve). Cap 3000 entries including tombstones; entries beyond the cap are skipped.
- `shuffle` (bool), `seed`, `shuffle_order` (alive item_ids). The **play order** is `shuffle_order` when shuffle is on, else the alive entries in base order.
- `repeat` ∈ {off, all, one}.
- Playback anchor: `cur` (item_id or empty), `playing`, `anchor_pos`, `anchor_time` (server ms), plus `epoch` (bumped on every explicit re-anchor).
- `history`: item_ids played before `cur`, most recent last, capped at 200.
- `participants` (`user_id`, `name`), `creator`, `host_chat_id`, `title`, `ended`.

### 9.2 Effective time and staleness

- Playback kinds: `eff = min(at, ts)`. If `ts - at > 5000` the op is **stale** and is a no-op (a command typed during a long disconnect must not rewind everyone). Missing `at` → treated as `ts` (the server rejects it anyway).
- All other kinds: `eff = ts`.
- Clients applying a *pending* op optimistically use `ts = at`.

### 9.3 Timeline derivation (automatic advance)

`timeline(T)` gives `(item, pos, playing)` at server time `T` without changing state:

```
if cur empty or not playing: return (cur, anchor_pos, false)
pos = anchor_pos + max(0, T - anchor_time); item = cur
loop:
  d = duration(item)
  if d <= 0 or pos < d: return (item, pos, true)         // unknown duration never auto-advances
  nxt = succ_auto(item); pos -= d
  if nxt is none: return end_of_queue()                   // see below
  item = nxt
```

- `succ_auto(x)`: `x` if repeat = one; else `succ_cmd(x)`.
- `succ_cmd(x)`: the item after `x` in play order; after the last item: the first if repeat ∈ {all, one}, else none.
- `end_of_queue()`: `(first item of play order, 0, paused)`, or empty if no alive item.
- Loops are bounded (repeat-one and repeat-all cycles are reduced modulo the cycle length).

**Materialization.** Before applying any record, the reducer re-anchors to `timeline(eff)` if `eff > anchor_time`, pushing every item it walked past onto `history` (and applying end-of-queue). This makes the effect of a later queue edit independent of when clients happen to look. It does not bump `epoch`.

### 9.4 Queue ops

- **insert(after, entries)**: entries whose `item_id` already exists (alive or tombstone) or is empty are skipped. Base order: placed after `after` if it exists (alive **or tombstone**), at the head if `after` is null, at the end if `after` is unknown. With shuffle on they are also placed in `shuffle_order` after `after` if it is there, else at the end. If `cur` is empty afterwards and the queue isn't, `cur` = first of play order, paused at 0.
- **remove(item_ids)**: each alive id becomes a tombstone and leaves `shuffle_order`/`history`. Unknown or already removed ids: no-op. If `cur` is removed: `s` = the first item after `cur` in the **pre-removal** play order that is still alive (wrapping past the end only when repeat ≠ off); anchor `(s, 0)` keeping `playing`; none → `end_of_queue()`.
- **move(item_id, after)**: no-op if `item_id` isn't alive or `after == item_id`. Acts on the **play order**: without shuffle, base order (after `after` if alive or tombstone, head if null, end if unknown); with shuffle, `shuffle_order` (after `after` if in it, head if null, end otherwise).

### 9.5 Playback ops

Below, "anchor `(x, p)`" sets `cur = x`, `anchor_pos = clamp(p, 0, duration(x))`, `anchor_time = eff`, bumps `epoch`, and pushes the old `cur` onto history if it differs.

- **play(item_id?, pos)**: target = `item_id` if alive, else `cur` at its derived position; no target → no-op. If `pos ≥ duration > 0`, pos = 0. Anchor, `playing = true`.
- **pause(item_id?, pos)**: target as for play. Anchor, `playing = false`.
- **seek(item_id, pos)**: `item_id` must be alive. Anchor, `playing` unchanged.
- **jump(item_id)**: must be alive. Anchor `(item_id, 0)`, `playing = true`.
- **next(from_item)**: no-op unless `cur == from_item` (after materialization). `s = succ_cmd(cur)`: anchor `(s, 0)` keeping `playing`; none → `end_of_queue()`.
- **previous(from_item, pos)**: no-op unless `cur == from_item`. If `pos > 3000`: anchor `(cur, 0)` (restart). Otherwise pop `history` until an alive item ≠ `cur` is found and anchor it at 0 **without** pushing `cur`; history exhausted → the play-order predecessor of `cur` (wrapping to the last item only when repeat = all), and with no predecessor restart `cur` at 0. `playing` unchanged. (`pos` is the sender's position, so every device makes the same restart-vs-back decision as the person who pressed it.)
- **advanced(from_item, to_item)**: no-op unless `playing`. Let `(c, p)` = `timeline(eff)` and `prev` = the item played just before `c` during that walk (none if the walk didn't cross a boundary). Accepted when
  - `from_item == c` and `p ≥ duration(c) - 3000` (the real audio ended up to 3 s before the metadata duration), or
  - `from_item == prev` and `p ≤ 3000` (the walk already crossed into `c` less than 3 s ago).
  Then anchor `(succ_auto(from_item), 0)` playing; none → `end_of_queue()`. `to_item` is advisory: the reducer recomputes the successor, so a "play next" insert sequenced earlier wins. The first `advanced` for a boundary re-anchors; any duplicate then finds `c` = the new item near position 0 and is a no-op.

### 9.6 Session ops

- **init**: `creator = by`, `host_chat_id`, `title`.
- **join**: add/update participant `by` with `name`. **leave**: remove `by`. **kick**: remove `user_id`.
- **end**: materialize, `playing = false`, `ended = true`. Later records are still applied (there are none in practice).

### 9.7 Settings

- **set_repeat(mode)**: set `repeat`.
- **set_shuffle(on, seed)**: on → `shuffle_order` = `cur` first (if any), then the other alive items in base order shuffled with Fisher–Yates driven by `splitmix64(seed)`: for `i = n-1 … 1`, `j = next() mod (i+1)`. Turning it on while on reshuffles with the new seed. off → clear `shuffle_order`; play order is the base order again.

### 9.8 Determinism notes

- `splitmix64`: `z = (s += 0x9E3779B97F4A7C15); z = (z ^ z>>30) * 0xBF58476D1CE4E5B9; z = (z ^ z>>27) * 0x94D049BB133111EB; return z ^ z>>31` (uint64 wrap-around).
- The server picks `seed` (uniform uint32), so it chooses the shuffle order while every device computes the identical list from the log.

## 10. Client playback policy (core, not reducer)

These rules turn the replicated state into player commands. They run on each device and don't affect convergence.

- **Local-first.** A user command is applied to `view` immediately and the player is commanded in the same call; the op goes out with `at = local_now + offset`. Remote devices apply the record and land at `pos + (server_now - at)` while playing ("elapsed-time compensation"). Nobody waits for the round trip.
- **Re-anchoring.** Whenever `epoch` changes (play/pause/seek/jump/next/previous/advanced), the device compares the player with `timeline(server_now)` and seeks if they differ by more than 100 ms.
- **Join / track change.** The core first asks the player to **load** (prepare without playing). When the player reports *ready*, the core issues *play* with the live position and a local reference time (`pos` is valid at `ref`; adapters that can should start at `pos + (now - ref)`). When the player reports audio actually *started*, the core recomputes the live position at that instant and seeks if it is off by more than 100 ms.
- **Correction.** About once a second the core asks for the player position. If it differs from `timeline(server_now)` by more than **400 ms on 2 consecutive checks**, the core seeks to the live position. No playback-rate nudging and no output-latency compensation.
- **End of track.** When the player reports the real end of the current item, the core sends `advanced(from, to)` and moves on. If the derived timeline crosses the boundary while the player is still finishing the previous item (metadata duration shorter than the audio), the core holds up to 2 s for the real end instead of cutting the tail.
- **Preload.** When an item starts, the core hints the platform to prefetch `succ_auto(cur)`.
- **Interrupted.** During a phone call the platform marks the device interrupted: the core pauses locally, stops corrections, sends `status{interrupted}`, and does not broadcast a pause. "Catch up" clears it and jumps to the live position.
- **Reconnect.** Playback continues on the last known timeline while offline. After `welcome` the device catches up from `since_rev`, resends pending ops (stale playback ops become no-ops, §9.2), and the re-anchoring rule moves the player to the live position.

## 11. Versioning

- `hello.v` is the client's protocol version (this document: **1**). The server accepts `MIN_V ≤ v ≤ MAX_V` (v1: both 1). Otherwise it sends `error{code:"version_unsupported",fatal:true,min_v,max_v,message:"Please update the app"}` and closes with 4009. The core then stops reconnecting and reports `update_required`.
- Adding optional fields is backwards compatible (unknown fields are ignored). A new op kind or a changed reducer rule is a new version, and `MIN_V` is raised to it, because every device must reduce the log identically. The reducer treats unknown kinds as no-ops only as a safety net.

## 12. Errors

| code | Where | Fatal / close code | Meaning |
|---|---|---|---|
| `bad_request` | error / reject / HTTP 400 | no | malformed JSON, wrong field types, limits exceeded |
| `too_large` | error | no | frame > 64 KiB (frame discarded) |
| `unauthorized` | error / HTTP 401 | yes / 4001 | missing, bad or expired token; message before `hello` |
| `forbidden` | reject / HTTP 403 | no | e.g. `kick`/`end` by a non-creator, unknown/server-only kind |
| `not_member` | error | yes / 4003 | token is fine but the user never redeemed an invite |
| `not_found` | HTTP 404 / error | yes / 4004 | no such session |
| `version_unsupported` | error | yes / 4009 | §11 |
| `session_full` | error / HTTP 409 | yes / 4010 | participant cap reached |
| `session_ended` | error / HTTP 410 | yes / 4011 | session over |
| `kicked` | error / HTTP 403 | yes / 4012 | removed by the creator |
| `left` | — | 4013 | the user left (sent to the user's other devices) |
| `rate_limited` | reject | no | > 30 ops burst / 10 per second sustained per socket |
| `log_full` | reject | no | log reached `MAX_LOG_OPS` (§15) |
| `internal` | error / HTTP 500 | no | unexpected server error |

Non-fatal errors never close the socket. Clients treat close codes 4001 (after refreshing the token), 1000/1001/1006/1011/1012 and transport failures as "reconnect with backoff" (250 ms doubling to 8 s, ±25 % jitter). 4003, 4004, 4009–4013 are terminal for that session.

## 13. Limits and timers

| Item | Value |
|---|---|
| Inbound frame | 64 KiB |
| Entries per `insert` / ids per `remove` | 100 |
| Queue entries incl. tombstones (reducer) | 3000 |
| `MAX_PARTICIPANTS` | 10 (server config) |
| `MAX_LOG_OPS` | 20000 |
| `log` chunk | ≤ 500 records |
| Rate limit | token bucket, 30 burst, 10/s per socket |
| `hello` deadline | 10 s after upgrade |
| Pending ops per client | 256 |
| Stale playback op | `ts - at > 5000` |
| Previous: restart threshold | 3000 ms |
| Correction | > 400 ms on 2 consecutive ~1 s checks |
| Idle end | 6 h with no connected device |
| Data retention | all session data deleted 24 h after the session ends |

## 14. Session lifecycle

1. `POST /v1/sessions` → DO created, rev 1 `init`.
2. Invites are created and redeemed (members).
3. Members connect; the first `hello` of a non-participant appends `join` (cap checked first).
4. A session **ends** when the creator sends `end`, when the last participant leaves, or after 6 h with no connected device (the server appends `end` with `by:""`). On end the server closes every socket with 4011.
5. 24 h after the end, a Durable Object alarm deletes all of the session's storage (log, members, invites). Requests for the session then get `not_found`.

## 15. Log cap and compaction plan

v1 keeps the full log (sessions are small: a 4-hour session is typically a few hundred records). At `MAX_LOG_OPS` new ops get `reject{code:"log_full"}`.

Compaction (later): compile the C++ reducer to wasm (no I/O, no exceptions, so it builds with clang's wasm32 target and `wasi-libc`-free) and run it in the DO. Every N records the DO folds the log into a state snapshot, stores `snapshot{rev,state}` (state in the same JSON the core exposes) and drops records ≤ `rev` except their `op_id`s (kept for idempotency, or replaced by a per-device high-water mark). `hello` with `since_rev` older than the snapshot gets `snapshot` followed by `log`. Because the server runs the same reducer, the snapshot equals what any client would have computed.

## 16. Full example

```text
→ {"type":"hello","v":1,"token":"v1.eyJ…","device_id":"ios-7f3a","since_rev":0,"t0":81234}
← {"type":"welcome","v":1,"session_id":"q3V…","user_id":"5550001","creator":"5550001","head_rev":1,
   "server_time":1764190000000,"t0":81234,"t1":1764190000000,"t2":1764190000000,"max_participants":10,"online":[]}
← {"type":"log","since_rev":0,"head_rev":2,"server_time":1764190000000,"more":false,"ops":[
   {"rev":1,"ts":1764189990000,"by":"5550001","op_id":"s:1","kind":"init","host_chat_id":"-1001234","title":""},
   {"rev":2,"ts":1764190000000,"by":"5550001","op_id":"s:2","kind":"join","name":"Ada"}]}
→ {"type":"ping","t0":81300}
← {"type":"pong","t0":81300,"t1":1764190000060,"t2":1764190000060,"head_rev":2}
→ {"type":"op","op_id":"7f3a:1:k2","base_rev":2,"kind":"insert","after":null,"entries":[
   {"item_id":"7f3a.1.a","chat_id":"-1001234","msg_id":10,"doc_id":"5368712345678901234","title":"One","performer":"A","duration_ms":200000}]}
← {"type":"event","server_time":1764190000200,"op":{"rev":3,"ts":1764190000200,"by":"5550001","op_id":"7f3a:1:k2",
   "base_rev":2,"kind":"insert","after":null,"entries":[{…}]}}
→ {"type":"op","op_id":"7f3a:2:p0","base_rev":3,"kind":"play","at":1764190000500,"item_id":"7f3a.1.a","pos":0}
← {"type":"event","server_time":1764190000560,"op":{"rev":4,"ts":1764190000560,"by":"5550001","op_id":"7f3a:2:p0",
   "base_rev":3,"kind":"play","at":1764190000500,"item_id":"7f3a.1.a","pos":0}}
   …a second device that receives rev 4 at server time 1764190000700 starts at pos 200 ms.
→ {"type":"op","op_id":"7f3a:2:p0", …same op again after a reconnect…}
← {"type":"ack","op_id":"7f3a:2:p0","rev":4,"dup":true}
→ {"type":"hello","v":0,…}
← {"type":"error","code":"version_unsupported","fatal":true,"min_v":1,"max_v":1,"message":"Please update the app"}  (close 4009)
```
