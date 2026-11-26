# Listen Together — Architecture & Implementation Plan (v2)

_Revised 2026-11-26. Replaces the plan from the original ChatGPT/Codex research chat._

## 1. Goal and ground rules

- **Feature:** two or more Telegram users listen to the same music at the same time. Everyone can play, pause, skip, go back, seek, and add, remove or reorder the shared queue.
- **Where (v1):** forks of the official Telegram iOS, Android and native macOS apps. Windows (Telegram Desktop) comes later.
- **Feel:** identical to Telegram. Same player, same sheets, same gestures. The only additions are a "Listen Together" action, a "Listening with…" indicator and the shared-queue controls.
- **Zero setup:** after installing the app and logging in to Telegram, there are no extra sign-ups, logins or settings. The invitee taps **Join** and hears music.
- **Use case:** people in different places, each on their own device (remote listening). Two phones playing out loud in the same room is out of scope for v1.
- **Nothing breaks:** everything that works in official Telegram keeps working in the fork, except the short list in §8 that no fork can do.
- **Distribution for now:** direct-installed builds. There is no paid Apple Developer account: iOS uses free Apple ID signing (§5.1), macOS uses local signing, and Android uses a signed APK. The App Store and Play Store are not goals yet.
- **Working name:** Tunegram (pending owner OK; the display name can change at any time). IDs: `app.tunegram.ios`, `app.tunegram.mac`, `app.tunegram.android`.
- **Technical choices** are made for end-user efficiency (latency, battery, reliability, app size) and aren't escalated.

## 2. Product model

### 2.1 A session lives in a chat
Every session is anchored to one **host chat**: a private chat, a group or a channel.

- **What can be queued:**
  - any audio file in the host chat;
  - any audio file from a **public** channel (anyone can play those).
- **Why this works:** Telegram's own membership rules already let every member of a chat play every track in it. Each device fetches the file itself, with its own access, from Telegram's servers and cache. Nothing is copied or relayed. It also works in "protected" chats (forwarding disabled), because members can still play there.
- **Who can join:** members of the host chat, plus anyone for a public channel. A non-member who receives an invite gets "Join [chat] to listen" when an invite link is available, or "Ask [name] to add you".
- **Adding a track from outside the host chat** (e.g. Saved Messages, another chat): one tap on **"Share to [chat] & queue"** posts it into the host chat as a normal message, then queues it. Where the user can't post (e.g. a channel without admin rights), that option is disabled and the reason is shown.
- **Starting from the player:** "Listen Together" uses the chat the current track is playing from as the host chat. The shared queue starts at the current track and is a fixed list: the next 200 tracks of that chat's audio, extended automatically when the session gets within 20 tracks of the end. This replaces Telegram's open-ended, page-by-page playlist, which can be thousands of tracks long.

### 2.2 Controls and roles
- **Equal control for all participants:** play, pause, next, previous, seek, shuffle, repeat, queue add/remove/reorder, and "add whole playlist" (appends the tracks in order).
- **Session creator:** can end the session and remove participants.
- **Personal settings:** volume, output device, leaving. Leaving restores the person's previous personal queue.
- **Previous** behaves like Telegram's player: past the first 3 seconds it restarts the track; otherwise it goes to the previous track in the session history.
- **Lock screen, headset and car controls** act on the session. A persistent "Listening with [name]" indicator in the player and mini-player makes that obvious.
- v1 limit: 10 participants per session, configurable on the server.

### 2.3 Flow (zero setup)
1. In the player, tap **Listen Together**. Pick people (contacts, or members of the host chat), or post the session in the host chat.
2. The other person sees a native invite card in the chat. In groups, fork users also see a "Listening now · Join" bar at the top of the chat, similar to the voice-chat bar.
3. They tap **Join**. Their app prepares the current track and starts at the live position.
4. People on official Telegram see a normal `t.me` link. It opens a small Mini App page: "Get [AppName] to join", with install links.

There is no sign-up and no separate login (see §4.4). No new permission prompts appear beyond the app's normal notification permission.

### 2.4 Interruptions

| Situation | Behaviour |
|---|---|
| Anyone presses pause | Pauses for everyone |
| Phone call on one device | That person is marked "interrupted"; others continue; "Catch up" when the call ends |
| One device loses connection | "Reconnecting…" shown; on return it jumps to the live position |
| Next track not ready on one device | Others wait up to 3 s, then continue; the late device joins at the live position |
| Someone leaves | Everyone else continues; if the creator leaves, the session continues for the remaining listeners |
| iPhone paused in the background | App kept awake while the session is paused (up to 60 min), so remote play just works. After that, the iPhone leaves the session and shows a local "Session paused for a while — tap to rejoin" notification. Without a paid Apple account there are no remote pushes, so the longer keep-awake window replaces them |
| Android paused in the background | The music service stays in the foreground for the whole session, so remote play always works |
| Mac | Mac apps aren't suspended, so remote play always works while the app is open |

The iPhone keep-awake plays inaudible audio, which costs a little battery. It is acceptable while App Review isn't a constraint; it sits behind a flag so it can be replaced if the App Store becomes a goal.

## 3. Sync model (remote listening)

- **Shared timeline:** the server holds the current track, the position at a given server time, and whether playback is running.
- **Clock alignment:** each device measures the offset between its own clock and the server's using quick timestamped pings on the open connection. It keeps the best (lowest-delay) samples and refreshes them periodically.
- **Local-first commands:** the person who presses a button hears the effect immediately. The command goes to the server with its position and timestamp. The server orders it and sends it on, and the other devices apply it, adding the time that has passed since it was pressed. Nobody's button is delayed.
- **Automatic advance:** every device follows the same queue and timeline, so each advances at the end of a track locally. The server confirms the next entry. Devices preload the next track so transitions stay as smooth as upstream playback.
- **Correction:** once a second, each device compares its position with the timeline. If it is off by more than 400 ms on two checks in a row, it seeks. Playback-speed nudging and output-latency compensation are left out: phones drift apart by only tens of milliseconds over a 4-minute track, and real mismatches come from starts, seeks and buffering, which the re-alignment handles.
- **Join:** the joining device prepares the track first, then seeks to the live position at the moment audio actually starts, not when it tapped Join.

**Targets (healthy connections; measured by in-app telemetry plus recorded audio tests):**

| Metric | Target (p95) |
|---|---|
| Command reaches other devices | < 500 ms |
| Tap Join → hearing music | < 3 s |
| Position difference between devices | < 400 ms |
| Gap between tracks | No worse than upstream solo playback |
| Queue identical on all devices after the server confirms | 100% |

## 4. Architecture

```
┌──────── iOS / Android / macOS fork (each) ─────────────┐
│  Listen Together UI   (native, reuses upstream parts)  │
│  Player adapter       (thin hooks into upstream player)│
│  Media resolver       (queue entry → playable file)    │
│  Session core         (shared C++ library, no I/O)     │
│  Transport            (platform WebSocket)             │
│  Push handling        (Telegram pushes + session pushes)│
└───────────────┬───────────────────────────┬────────────┘
                │ WebSocket (TLS)           │ APNs / FCM
┌───────────────▼──────────────┐  ┌─────────▼──────────────────┐
│ Cloudflare Workers           │  │ Push relay (later, §4.5)    │
│  • Session Durable Object    │  │  • Telegram web-push intake │
│    (one per session)         │  │  • APNs (incl. VoIP) + FCM  │
│  • Auth (initData verify)    │  │  • session pushes           │
│  • Invites + Mini App page   │  └─────────────────────────────┘
└──────────────────────────────┘
Audio always flows Telegram → device directly. Our servers never see audio or chat content.
```

### 4.1 Client components
- **Session core (shared C++20 library, one implementation for every platform).** It contains:
  - protocol encode/decode;
  - the session state machine;
  - the queue model and reconciliation of concurrent edits;
  - clock-offset estimation, timeline maths and the correction policy;
  - reconnect and snapshot handling.

  It does no I/O itself: the platform supplies time, timers and a socket, and receives "send this", "player do that" and "UI state changed". That makes it deterministic and fully testable in simulation. C++ builds in every Telegram toolchain today (Bazel on iOS, the NDK on Android, CMake in Telegram Desktop).
- **Player adapter (per platform, thin).** Supplies: set queue, play, pause, seek, report position, and report buffering and end-of-track. It also routes every intentional command (screen, lock screen, headset) through the session, and marks remote-applied actions so they aren't re-broadcast. Expected hooks, to be confirmed in Phase 1:
  - **iOS:** `submodules/TelegramUI/Sources/SharedMediaPlayer.swift`, `MediaManager.swift`, `OverlayAudioPlayerControllerNode.swift`.
  - **Android:** `org.telegram.messenger.MediaController`, `MusicPlayerService`, `ui/Components/AudioPlayerAlert`, `FragmentContextView`.
  - **macOS:** the audio player controller and player header in `TelegramMessenger/telegram-macos`, to be located in Phase 1. That repo pulls in Telegram-iOS as a submodule for TelegramCore/Postbox/SwiftSignalKit.

  The Apple adapter logic is written once against TelegramCore/SwiftSignalKit, with no UIKit/AppKit in the logic, and shared by iOS and macOS. Only the UI layer differs.
- **Media resolver.** A queue entry is identified by host chat, message ID and Telegram document ID. Each device fetches the message with its own account to get a playable file. Expired file references are refreshed by re-fetching the message. Playback uses Telegram's existing download, streaming and cache, so tracks already downloaded play instantly.
- **Transport:** `URLSessionWebSocketTask` on iOS and macOS, and OkHttp WebSocket on Android. Both handle Wi-Fi ↔ cellular changes, and both honour the user's SOCKS5 proxy setting.

### 4.2 Backend
- **One Cloudflare Durable Object per session.** It is the single writer, so edits get one global order. It is created near the session creator, holds state in memory with snapshots in its storage, and uses hibernating WebSockets so idle sessions cost nothing. Session data is deleted 24 h after a session ends.
- **Auth endpoint:** verifies Telegram-signed Mini App `initData` (§4.4) and issues a short-lived session token.
- **Invite endpoint:** creates single-use, expiring invite tokens and builds `https://t.me/<bot>/<app>?startapp=<token>` links. The fork intercepts these links natively; official clients open the Mini App landing page.
- **Push relay (Cloud Run, Go, in the same Google project as Firebase).** It receives Telegram's pushes for fork devices and forwards them over APNs (including VoIP call pushes) and FCM. It also sends our own session pushes (invites, "tap to rejoin"). It runs on Cloud Run rather than Workers because APNs requires HTTP/2 connections.
- **Data stored:** Telegram user IDs, session membership, queue entries (chat ID, message ID, document ID, plus title and duration for display), and timing. No audio, no chat content.

### 4.3 Protocol (v1 sketch)
- **Connection messages:** `hello{token, protocol_version, device_id}` → `snapshot{rev, state}`; `ping/pong{t0,t1,t2}` for clock alignment.
- **Edits:** the client sends `op{op_id, base_rev, kind, args}`; the server replies `ack{op_id, rev}` and broadcasts `event{rev, op}`. Every op is idempotent by `op_id`. Clients apply edits immediately and rebase on server events, so dragging to reorder feels instant.
- **Op kinds:**
  - `insert(after_item_id, entries[])`, `remove(item_id)`, `move(item_id, after_item_id)`;
  - `play(pos)`, `pause(pos)`, `seek(pos)`, `next`, `previous`, `jump(item_id)`;
  - `set_repeat(mode)`, `set_shuffle(on)`. The server picks the shuffle order and sends it, so every device shuffles identically.
- **Versioning:** each client sends its protocol version; the server rejects incompatible versions with an "update the app" message.

### 4.4 Sign-in to our service: zero taps
The fork already has a logged-in Telegram session. When the feature is first used, it silently requests our bot's Mini App URL. That URL contains `initData` signed by Telegram. The server verifies it with Telegram's published Ed25519 public key (or the bot token's HMAC) and issues our own token. The user sees nothing.

Phase 2 confirms that this creates no visible bot chat and no consent sheet. If a one-time consent is required, it becomes part of the first "Listen Together" tap.

### 4.5 Push notifications in the forks
Telegram can't push to an app that uses another developer's APNs/FCM credentials, so a fork gets no notifications by default.

**Status: deferred.** Without a paid Apple account, iPhones can't receive remote pushes at all (free Apple IDs don't include Push Notifications). Until then, every platform still gets notifications:
- **iPhone:** official Telegram stays installed alongside the fork and delivers message and call notifications. Invites arrive as normal messages. The `t.me` link opens the landing page, which offers "Open in Tunegram" through the app's URL scheme. Inside the fork, `t.me` links are intercepted natively.
- **Android:** my.telegram.org has a **Push credentials** section that accepts an FCM service account (confirmed 2026-11-26). Once our Firebase service account is uploaded there, Telegram pushes directly to the fork through FCM, with no relay needed. The owner uploads the key file personally. Until then, Telegram's built-in background connection ("keep-alive service") delivers notifications.
- **macOS:** the app stays connected while it runs and shows notifications itself.

The relay below gets built once a paid Apple account exists, or earlier if the Android background connection proves unreliable.

- **Relay plan:** register each device with Telegram as **web push** (`token_type 10`), with our relay as the endpoint and a P-256 key pair generated on the device.
- The relay forwards the still-encrypted payload: to iOS as an APNs `mutable-content` push, where the Notification Service Extension decrypts it and renders it exactly as upstream does; to Android as a high-priority FCM data message, which goes through the same path. The relay can't read message content.
- **Fallback:** if my.telegram.org lets us attach our APNs/FCM credentials to our `api_id`, use direct pushes instead.
- **Risk to settle in Phase 1:** whether incoming **call** notifications arrive over web push. These need iOS VoIP pushes to ring while the app is closed.

### 4.6 Fork upkeep
- Upstream remotes are tracked as-is. Our changes are limited to small hook call sites plus our own modules, listed in `docs/PATCHES.md`.
- CI builds both forks and runs regression plus Listen Together tests, with the feature both enabled and disabled.
- A watcher triggers a merge-and-build when upstream publishes a new release drop. Upstream publishes in large periodic drops, not small commits.

## 5. Build and development setup

### 5.1 Where things build and run
- **Builds run in the cloud (GitHub Actions)**, because this Mac has about 10 GB free and a local Telegram build needs about 100 GB:
  - iOS Simulator and macOS builds on macOS runners, with upstream's pinned Xcode;
  - Android on Linux runners.
  - A remote build cache keeps rebuilds fast.
- **Repos:** all public (decided 2026-11-26), under the personal `adilabdu` GitHub account (unrelated to any company). Commits use the GitHub noreply address, and no secrets are committed: they stay in `.secrets/` locally and in GitHub Actions encrypted secrets.
  - `adilabdu/tunegram` holds the plan, shared core, backend and CI.
  - `tunegram-ios`, `tunegram-android` and `tunegram-macos` each hold a mirror of upstream plus our changes. The macOS fork's Telegram-iOS submodule points at `tunegram-ios`.
  - Local working copies are sparse partial clones, so only the files being edited take disk space.
- **CI cost:** free. Public repos get unlimited minutes on GitHub's standard runners, macOS included.
  - The standard macOS runners are small (3-core M1, 7 GB RAM). The first Telegram iOS build in Phase 0 confirms the build fits.
  - Fallback: a one-time external SSD, so Apple builds run locally instead.
- **This Mac only runs the results:**
  - the iOS Simulator (iOS 18 runtime already installed);
  - the Android Emulator (Pixel AVD, API 34, already installed);
  - the macOS app.
  
  Downloads are a few hundred MB per build. Local Xcode 27.0 is fine for this; Xcode 27.1 only matters for builds, which happen in the cloud.
- **Development order:** iOS Simulator + Android Emulator + macOS app first, all on this Mac. Two simulators and the emulator together allow three-person sessions locally. Physical iPhones come next.
- **Physical iPhones with a free Apple ID:**
  - Xcode installs the app on the two connected iPhones.
  - Signing expires after 7 days, so reinstalling is automated whenever a phone is connected or on the same Wi-Fi.
  - Limits: at most 3 free-signed apps per phone, at most 10 app IDs per week (extensions trimmed to Share + Widget), and no push notifications (§4.5).
  - App Groups, Keychain sharing and background audio all work with a free Apple ID.
  - Friends' iPhones: AltStore/SideStore with their own free Apple ID, or a paid account later.
- **macOS:** signed locally, with no expiry and no Apple account needed. Friends' Macs need a one-time "Open Anyway" in System Settings. Upstream's Sparkle auto-update is pointed at our builds or disabled.

### 5.2 Build configuration
- **iOS:**
  - Telegram's Bazel build (`build-system/Make/Make.py`) with our own configuration: `bundle_id`, `api_id`, `api_hash`, `team_id` (the free Apple ID's personal team), `app_specific_url_scheme`, `premium_iap_product_id: ""`.
  - Siri, iCloud, the Watch app, the push extensions and Associated Domains are off in v1.
  - Upstream currently pins **Xcode 27.1** and Bazel 9.2.0.
- **macOS:** `TelegramMessenger/telegram-macos` (GPL-2.0) with our bundle ID, `api_id`/`api_hash`, local signing and our own update feed.
- **Android:**
  - Our `APP_PACKAGE` and `APP_ID`/`APP_HASH`, our own `google-services.json`, and our own signing keystore.
  - `CHECK_UPDATES` is disabled so the app never prompts to install official Telegram.
  - Official-app-only integrations are switched off: passkeys, Google sign-in client, SafetyNet/Play Integrity key, Play billing.
- **Telegram test environment** (test data centres and throwaway test numbers) for automated two-account tests, with the owner's approval.

## 6. Phases (ordered by risk)

| Phase | Work | Done when |
|---|---|---|
| 0. Setup | Accounts and keys (§9); cloud build pipeline; local simulator, emulator and Mac run scripts | A CI build of upstream Telegram runs in the simulator, the emulator and on the Mac |
| 1. Working forks | All three forks build with our identity in CI and run in the iOS Simulator, the Android Emulator and on the Mac; log in with test-environment and real accounts; regression checklist of core Telegram features; then free-signed installs on the two iPhones | Messaging, media, calls and solo music playback work on all three; the iPhone install and weekly re-sign are automated |
| 2. Risk spikes | (a) Two accounts play the same track from a shared DM, group, protected group and public channel, each with its own access. (b) Programmatic queue/play/seek/position/end-of-track control in both players; measure start latency and seek accuracy. (c) Background: iPhone locked screen while playing, shared-pause keep-awake, Android foreground service, connection survival. (d) Silent `initData` auth with no visible bot chat | Each spike has a go/no-go result recorded in `docs/spikes/` |
| 3. Core + backend | C++ session core with deterministic simulation tests (delays, reordering, concurrent edits, reconnects, duplicate ops); Durable Object; auth; invites; protocol v1 | Simulation suite passes; two simulated clients always converge |
| 4. Feature UI | Native invite card and group "Listening now" bar; Join; shared controls; queue edit; indicator; leave; "Share to chat & queue"; Mini App landing page | Two people complete the full flow without leaving the app |
| 5. Real-world validation | Device matrix (iPhone + Android; speaker, wired, Bluetooth); Wi-Fi ↔ cellular, weak signal; calls; long sessions; telemetry against the §3 targets | Targets met on the matrix |
| 6. Distribution | Signed Android APK and locally signed macOS app, both with in-app update checks pointing to our builds; iPhone via free signing (owner) or AltStore/SideStore (friends); upstream-merge automation | Friends can install and update with a short guide |
| Later | Telegram Desktop for Windows (C++ core directly); push relay and ad-hoc iPhone installs once a paid Apple account exists | — |

## 7. Testing
- **Session core:** deterministic network/clock simulation and property tests ("all clients converge to the same queue").
- **Backend:** Durable Object tests in the local Workers runtime; relay tests with recorded Telegram push payloads.
- **Devices:** a two-device rig with automated scripts. Sync accuracy is measured from telemetry (positions plus clock offsets) and spot-checked by recording both outputs.
- **Upstream regression:** messaging, media, calls, notifications, stories, login, proxy, multiple accounts — with the feature both on and off.

## 8. What a fork can't do (known, unavoidable)
- Buying Premium, Stars or gifts through Apple/Google in-app purchase. Those products belong to Telegram's store accounts. Premium already on the account works normally; purchases can be made in official Telegram.
- Sign-in methods tied to the official app's identity (passkeys, Google sign-in, Firebase phone verification). Phone number + code login works.
- Brand-new Telegram accounts created through third-party apps are more likely to be restricted, so testers should use existing accounts.
- Users who can reach Telegram only through an MTProto proxy may not reach our sync server; SOCKS5 proxies are supported.
- iPhones on free signing (no paid Apple account): the app must be reinstalled every 7 days (automated when the phone is connected or on the same Wi-Fi), at most 3 such apps per phone, and no remote push notifications.

## 9. Setup checklist (owner)
Only the account owner can create accounts, accept terms and handle payment. Secrets go in `.secrets/` in this project (git-ignored) and are never pasted into chat.

Items 3–7 are done in the owner's Chrome via the Claude browser extension. The owner types only login codes and passwords.

1. ~~Free about 100 GB~~ → replaced by cloud builds (§5.1), in public repos (decided 2026-11-26).
2. ~~Paid Apple Developer Program~~ → replaced by free Apple ID signing (§5.1). Needs: the owner's Apple ID signed in once in Xcode on this Mac (Settings → Accounts).
3. ~~**Telegram API credentials**~~ → done 2026-11-26. Telegram allows one `api_id` per account, and the owner's account already had one, so Tunegram reuses that `api_id`/`api_hash` (saved in `.secrets/telegram.json`). The existing app's settings weren't changed.
4. ~~**Telegram bot**~~ → done 2026-11-26: @TunegramSyncBot, token in `.secrets/bot.json` (verified with `getMe`). The Mini App is attached when the landing page is deployed.
5. ~~**Firebase project**~~ → done 2026-11-26: project "Tunegram" (`tum-58a04`), Spark plan, Analytics and Gemini off; Android app `app.tunegram.android` registered; `google-services.json` is in `.secrets/`.
6. **Cloudflare account** (free). Needed only when the backend is deployed (§6, Phase 3–4); local development uses the local Workers runtime.
7. **App name and IDs:** working name Tunegram (§1).
8. **Devices:** two iPhones already connected; later they need Developer Mode on. Simulators and the emulator come first.
9. ~~Approval for Telegram test-environment accounts~~ → approved 2026-11-26.
