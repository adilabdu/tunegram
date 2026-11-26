# Listen Together backend (`backend/`)

Cloudflare Worker + one Durable Object per session, in TypeScript. It implements the server side of [docs/PROTOCOL.md](../docs/PROTOCOL.md): Telegram Mini App `initData` auth, short-lived session tokens, single-use invites, and the per-session **sequencer plus log** over hibernating WebSockets. It never interprets queue or playback semantics; those live in the C++ reducer in [`core/`](../core/README.md).

Everything runs locally (wrangler + miniflare/workerd). No Cloudflare account is needed or used.

## One-command test

```sh
cd backend && npm install && npm test
```

`npm test` runs vitest inside the local Workers runtime via `@cloudflare/vitest-pool-workers`: auth (Ed25519 and HMAC, freshness, tokens), sessions and invites, the WebSocket sequencer (ordering, idempotency, validation, rate limit, log cap, participant cap, kick/leave/end, eviction/hibernation recovery), and the lifecycle alarms (idle end, deletion 24 h after the end). Keys are generated per run; nothing real is used.

Other commands:

| Command | What |
|---|---|
| `npm run typecheck` | `tsc --noEmit` over sources and tests |
| `npm run dev` | `wrangler dev --local` on http://127.0.0.1:8787 (needs `backend/.dev.vars`, see below) |
| `npm run e2e` | End-to-end: real C++ core clients ↔ this Worker under `wrangler dev` (see below) |

## End-to-end test (one command)

```sh
cd backend && npm install && npm run e2e
```

`e2e/run.mjs` builds `core/tools/lt_stdio` (CMake, into `core/build-e2e/`), starts this Worker with `wrangler dev --local` on a free port with a generated Ed25519 key and token secret, signs Telegram-style `initData` for three users, creates a session and redeems invites, then runs three real C++ core clients (one `lt_stdio` process each, with a built-in fake player) bridged over WebSockets. It checks: joining, queue convergence (including `chat_username`), play/seek/pause sync across devices, concurrent edits from all three, a 6 s network drop with catch-up at the live position, automatic advance at the end of a track, an "other audio" interruption with automatic rejoin, a protocol-version mismatch, and ending the session. It prints PASS/FAIL per check plus timing metrics and exits non-zero on failure (about 30 s). `LT_E2E_VERBOSE=1` shows wrangler and build output.

## Layout

| Path | What |
|---|---|
| `src/index.ts` | Worker: routes `/v1/auth`, `/v1/sessions`, `/v1/sessions/:id/invites`, `/v1/invites/redeem`, `/v1/sessions/:id/ws` |
| `src/session.ts` | `SessionDO`: single writer per session; log, members/participants, invites, presence, alarms |
| `src/auth.ts` | initData verification (Ed25519 third-party + bot-token HMAC) and HMAC-signed session tokens |
| `src/protocol.ts` | Op shape/size validation; whitelists fields copied into log records |
| `test/` | vitest suites (`auth.test.ts`, `session.test.ts`) and helpers that generate signed initData |
| `e2e/run.mjs` | End-to-end runner |

## Configuration

Vars (in `wrangler.jsonc`): `TELEGRAM_ENV` (`prod`/`test`, picks Telegram's Ed25519 key), `BOT_ID` (numeric bot id, required for Ed25519 validation), `BOT_USERNAME`, `MINI_APP_SHORT_NAME` (invite links `https://t.me/<bot>/<app>?startapp=<token>`), `MAX_PARTICIPANTS` (10), `MIN_PROTOCOL_VERSION`/`MAX_PROTOCOL_VERSION` (1/1), `TOKEN_TTL_S` (3600), `INIT_DATA_MAX_AGE_S` (3600). Optional: `MAX_LOG_OPS` (20000), `IDLE_END_S` (21600), `RETENTION_S` (86400).

Secrets (never committed): `TOKEN_SECRET` (required, ≥ 16 chars) and `BOT_TOKEN` (optional; enables the HMAC path). For local dev put them in `backend/.dev.vars` (git-ignored):

```
TOKEN_SECRET=<random hex>
```

`TELEGRAM_PUBLIC_KEY` (hex) overrides Telegram's key and exists only for tests and the e2e run.

## Storage and lifecycle

Per session DO (SQLite-backed storage, KV API): `meta`, `members`, `participants`, `op:<rev>` (one record per key), `inv:<sha256(secret)>`. The full log is kept in memory and reloaded after eviction; op ids are rebuilt from it for idempotency. Writes are covered by the DO output gate, so a record is durable before any client sees it.

One alarm drives three deadlines: closing sockets that never sent `hello` (10 s), ending a session idle for 6 h with no connected device, and `deleteAll()` 24 h after a session ends. The log is capped at `MAX_LOG_OPS`; compaction (running the C++ reducer as wasm inside the DO and storing snapshots) is described in PROTOCOL §15.

## Notes

- `@cloudflare/vitest-pool-workers` (0.23) prints a notice that it was renamed to `@cloudflare/vitest-plugin`; it still works, and the migration is a codemod (`npx @cloudflare/codemods vitest:pool-workers-to-vitest-plugin`).
- `compatibility_date` is pinned to a date the bundled workerd supports (2026-08-01).
