# Listen Together session core (`core/`)

Platform-independent C++20 core shared by the iOS, Android and macOS forks (and later Telegram Desktop). It implements the client side of [docs/PROTOCOL.md](../docs/PROTOCOL.md): protocol encode/decode, the deterministic reducer, local-first rebase, clock sync, timeline maths, the correction policy and reconnect/catch-up.

**No I/O.** The platform passes in bytes, player reports and "now" (monotonic ms), and drains output events: *send these bytes*, *connect/disconnect*, *player: load/play/pause/seek/stop/preload/query* and *UI state changed*.

## Build and test (one command)

```sh
cmake -S core -B core/build -DCMAKE_BUILD_TYPE=Release && cmake --build core/build -j && ctest --test-dir core/build -j8 --output-on-failure
```

Requirements: CMake ≥ 3.20 and a C++20 compiler (Apple clang 15+, NDK r26+, GCC 11+). No other dependencies.

- Run the test binary directly for details: `core/build/lt_tests` (all) or `core/build/lt_tests convergence clock` (name filter).
- More random seeds: `LT_SEEDS=2000 core/build/lt_tests convergence` (default 200; the 10-client profile uses a fifth).
- Reproduce one seed: `LT_ONLY_SEED=5008 LT_VERBOSE=1 core/build/lt_tests convergence_many`.
- Sanitizers: `cmake -S core -B core/build-asan -DCMAKE_BUILD_TYPE=Debug -DLT_CORE_SANITIZE=ON && cmake --build core/build-asan -j && core/build-asan/lt_tests`.

## Layout

| Path | What |
|---|---|
| `include/tunegram/lt_core.h` | The **C API** (`extern "C"`), the only public header |
| `src/reducer.*` | Deterministic reducer: queue ops, playback anchor, timeline derivation (PROTOCOL §9) |
| `src/session.*` | Client state machine: connection, clock sync, confirmed log + pending ops, player driver |
| `src/protocol.*`, `src/json.*` | Wire format; a small exception-free JSON parser/serializer |
| `src/clock.hpp` | Min-RTT clock-offset estimator |
| `tests/sim.*` | Deterministic simulation: fake clock, fake network, reference sequencer, fake player |
| `tests/test_*.cpp` | Unit, scenario and property tests |
| `tools/lt_stdio.cpp` | A real core client over stdin/stdout (used by `backend/e2e`) |
| `tools/size_probe.c` | Links the whole C API into a tiny program to measure binary cost |

## Integrating

Link the static library `lt_core` and include `tunegram/lt_core.h`.

```c
lt_config cfg = { .device_id = "ios-7f3a", .token = token_from_auth_endpoint, .seed = arc4random() };
lt_core* core = lt_core_create(&cfg);
lt_core_start(core, now_ms());
// after every lt_core_* call:
lt_event ev;
while (lt_core_poll(core, &ev)) {
  switch (ev.type) {
    case LT_EV_SEND:         ws_send_text(ev.data, ev.len); break;
    case LT_EV_CONNECT:      ws_open("wss://…/v1/sessions/<id>/ws"); break;  // then lt_core_on_open / lt_core_on_close
    case LT_EV_DISCONNECT:   ws_close(); break;
    case LT_EV_PLAYER_LOAD:  player_prepare(ev.item_id, ev.chat_id, ev.msg_id, ev.doc_id); break;  // then lt_core_player_ready
    case LT_EV_PLAYER_PLAY:  player_play_from(ev.pos_ms + (now_ms() - ev.ref_ms)); break;     // then lt_core_player_started
    case LT_EV_PLAYER_PAUSE: player_pause_at(ev.pos_ms); break;
    case LT_EV_PLAYER_SEEK:  player_seek(ev.pos_ms + (now_ms() - ev.ref_ms)); break;
    case LT_EV_PLAYER_STOP:  player_stop(); break;
    case LT_EV_PLAYER_PRELOAD: prefetch(ev.item_id, ev.chat_id, ev.msg_id, ev.doc_id); break;
    case LT_EV_PLAYER_QUERY: lt_core_player_position(core, now_ms(), item, position, is_playing); break;
    case LT_EV_STATE_CHANGED: refresh_ui(lt_core_state_json(core, now_ms())); break;
    case LT_EV_ERROR:        show_error(ev.code, ev.fatal); break;
  }
}
schedule_timer_at(lt_core_next_timeout(core));  // -> lt_core_tick(core, now_ms())
```

Rules for adapters:
- One thread (or serialized calls) per `lt_core`. Event pointers are valid until the next call on the same core.
- Route every intentional user action (UI, lock screen, headset, car) through `lt_core_play/pause/seek/next/previous/…`; never drive the player directly while in a session. The core commands the player for local and remote actions alike, so remote actions are never re-broadcast.
- Report `ready` when the item can start, `started` when audio actually starts, `ended` at the real end of the audio, `buffering` stalls, and answer `PLAYER_QUERY` with the current position.
- `now` must be a monotonic clock (`CACurrentMediaTime`, `SystemClock.elapsedRealtime`, `steady_clock`), never wall time.
- After `LT_EV_DISCONNECT`, drop the socket and ignore its callbacks. `LT_EV_ERROR` with `code == "unauthorized"` means: fetch fresh `initData`, get a new token, call `lt_core_set_token`.

## Build flags and size

The library is built with `-fno-exceptions -fno-rtti` (`LT_CORE_NO_EXCEPTIONS=ON`), `-ffunction-sections -fdata-sections` and hidden visibility for internals. Nothing throws; the C API also catches everything when exceptions are enabled. For iOS (Bazel) and Android (NDK/CMake) add the five `src/*.cpp` files with the same flags; there are no generated files.

Measured on arm64 macOS (Apple clang 21):

| Build | `liblt_core.a` | Added to a linked, stripped binary |
|---|---|---|
| Release (`-O3`) | 266 KB | ~150 KB |
| MinSizeRel (`-Os`, recommended for apps) | 264 KB | ~119 KB |

The archive size is mostly symbol tables; the second column (`lt_size_probe` minus an empty program, both stripped) is what an app pays.
