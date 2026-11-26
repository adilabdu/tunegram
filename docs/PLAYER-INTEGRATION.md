# Player integration: where Listen Together plugs into each app

_Written 2026-11-26 from a read-only study of upstream source. Nothing was built or run, so every behaviour below is read from code; the items in §6 are what to confirm once the forks build._

This answers PLAN §4.1 "Player adapter … expected hooks, to be confirmed in Phase 1". It lists, per platform, where playback, remote commands, deep links, Mini App auth and the UI additions hook in, and the smallest upstream patch for each.

**Pinned commits** (all links below point at these):

| App | Repo @ commit | Notes |
|---|---|---|
| iOS | [Telegram-iOS @ 24638bc](https://github.com/TelegramMessenger/Telegram-iOS/tree/24638bc69bd083981b178dd1e7f921eebbf7fc46) (13.2) | Same as our fork base. `release-13.0.0` = f1dd7a2; differences in §1.10 |
| Android | [Telegram-Android @ 21f7d10](https://github.com/TelegramMessenger/Telegram-Android/tree/21f7d109da6ab3506c9e9577f98c6a38771ab335) (13.2.2) | Same as our fork base |
| macOS | [telegram-macos @ 38309ec](https://github.com/TelegramMessenger/telegram-macos/tree/38309ec95e59be3958d1e6fe9be331d72527a38d) | Its `submodules/telegram-ios` is Telegram-iOS 24638bc, so TelegramCore/Postbox answers in §1 apply to macOS too |

---

## Key findings

1. **Every app already has an explicit-list playlist.**
   - iOS: `PeerMessagesPlaylistLocation.custom(messages:…)`, used by saved/profile music.
   - Android: `MediaController.setPlaylist(ArrayList<MessageObject>, …)`.
   - macOS: `APChatMusicController(…, messages: [Message])`.

   None fits as-is: iOS picks neighbours by timestamp, Android re-sorts by message id, and macOS keeps a static list that it reverses internally. Each needs a thin session-owned playlist. On iOS that playlist needs no upstream patch at all.
2. **Each app has one control choke point, in one file.**
   - iOS: `MediaManagerImpl.playlistControl` / `setPlaylist`. Lock screen, headset and car commands enter in the same file.
   - Android: the public methods of `MediaController`. A Chromecast sync with an echo guard (`ignorePlayerUpdate`) already sits at exactly these points and is the pattern to copy.
   - macOS: the public methods of `APController`, plus `MediaPlaybackControlCenter` in the same file.

   On all three, **auto-advance at end of track is internal and bypasses the choke point**, so the adapter must suppress it and report `ended` instead.
3. **No app can "prepare paused at position X".** Every new track autoplays. Each needs a small patch, 5–15 lines:
   - iOS: an `autoplay` flag in `setPlaylist`.
   - Android: `startPaused` at the `audioPlayer.play()` call.
   - macOS: call `MediaPlayer.seek(timestamp:play:false)` instead of `play()`.

   Seeking before the track is ready works on iOS and Android. macOS's `APController.set(seek:)` drops it.
4. **Position:**
   - iOS and macOS publish a timestamp plus rate on events, from the audio timebase. Precision is about 10–25 ms.
   - Android polls ExoPlayer every 17 ms.
   - **None compensates for output latency.** Bluetooth adds about 150–250 ms. That is consistent with PLAN §3, which leaves latency compensation out.
5. **Deep links: one 3–12 line intercept per app**, after the URL is parsed and before any network call or UI. `t.me/<bot>/<app>?startapp=` and `tg://resolve?domain=&appname=&startapp=` both arrive there, from chat links, link-preview buttons, QR codes and external opens. The intercept points:
   - iOS: `openResolvedUrlImpl` `.peer(_, .withBotApp)`.
   - Android: the top of `LaunchActivity.runLinkRequest`.
   - macOS: `execute(inapp:)` `.followResolvedName`.
6. **Silent auth needs no UI and adds nothing to the chat list on the client side.** `getBotApp` + `requestAppWebView(allowWrite: false)` (or `requestMainWebView`) returns a URL whose fragment carries `tgWebAppData`.
   - All consent and "open app" sheets are client-side, so calling the method directly skips them.
   - Unknown: whether the server records the bot under "recent apps", and whether it accepts a not-yet-activated app without consent. Phase 2 spike (d) must test both.
   - **iOS master blocks raw TL calls from outside TelegramCore** (`Network.request` became `internal`; it was `public` at 13.0.0). The calls we need all have public engine wrappers, so this doesn't block us.
7. **The invite card is nearly free.** The server already gives `t.me/<bot>/<app>` previews the type `telegram_botapp`, and all three apps draw them as a card with an "OPEN APP" button. Relabelling it "Join" for our link takes 8–20 lines per app, and the tap already flows into the deep-link intercept. A truly custom bubble is a 300+ line job on every platform; skip it for v1.
8. **Background (plan-relevant):**
   - **iOS** drops the audio session 0.8 s after a pause in the background, so the app suspends. Keep-awake needs a 2–3 line patch plus a silent loop.
   - **Android 12+** keeps the music service in the foreground while paused. **Android 11 and lower drop it.** If the service ever stops mid-session (queue end, notification swiped), a remote "play" can't restart it from the background. Keep the service for the whole session.
   - **macOS: likely blocker for screen-off listening.** `SharedWakeupManager` pauses TelegramCore download connections when the display sleeps (`backgroundAudio` is hard-coded `false`), so streamed music should stall once the buffer runs out. The fix is about 8 lines. A paused, hidden Mac app can also be App-Napped; hold `ProcessInfo.beginActivity` during a session.
9. **Plan/protocol change: queue entries for public channels must carry the channel username.** A device that isn't a member has no access hash from a numeric id alone. iOS `getMessagesLoadIfNecessary` silently drops ids whose chat isn't known locally. Add an optional `chat_username` to `lt_entry` / the protocol entry, or resolve it via the invite.
10. **Estimated upstream patch size:**
    - iOS: about 230–340 lines in about 12 files.
    - Android: about 300–480 lines in 10 files.
    - macOS: about 450–700 lines in about 12 files, more than half of it append-only or new views.

    Easiest everywhere: deep-link intercept, context-menu items, invite-card relabel, silent auth. Hardest everywhere: exact join timing, and telling user commands apart from system/automatic ones. On top of that: Android needs a new header-bar style; macOS needs the header bar and live queue UI, and its single global player slot means any voice note kills the session player.

---

## 0. The player adapter

The C++ core's C API ([`core/include/tunegram/lt_core.h`](../core/include/tunegram/lt_core.h), committed in 728bd56) already fixes the player contract. The core never lets the upstream player run the session on its own. It drives one item at a time, and the queue lives in the core.

**Core → player** (drained with `lt_core_poll`):

| Event | Adapter must |
|---|---|
| `LT_EV_PLAYER_LOAD(item, chat_id, msg_id, doc_id)` | Resolve the message (§1.5, §2.5, §3.5), prepare the track **paused**, then report `lt_core_player_ready` |
| `LT_EV_PLAYER_PLAY(item, pos_ms, ref_ms)` | Start at `pos + (now − ref)`. Report `lt_core_player_started(pos)` when audio actually runs |
| `LT_EV_PLAYER_PAUSE(pos_ms)` | Pause immediately (no fade) and park at `pos` |
| `LT_EV_PLAYER_SEEK(pos_ms, ref_ms)` | Seek, keeping the current play/pause state |
| `LT_EV_PLAYER_STOP` | Release the session item (leave or end) |
| `LT_EV_PLAYER_PRELOAD(entry)` | Prefetch the next track |
| `LT_EV_PLAYER_QUERY` | Report `lt_core_player_position(item, pos, playing)` |
| `LT_EV_STATE_CHANGED` | Re-read `lt_core_state_json` and mirror the queue and participants into the upstream UI |

**Player → core:** `ready`, `started(pos)`, `ended` (the real end of audio), `buffering(on/off)` and `position`. User commands go to `lt_core_play/pause/seek/next/previous/jump/insert/remove/move/set_repeat/set_shuffle`. Phone calls and similar go to `lt_core_set_interrupted`.

**Proposed native shape.** The logic is written once in Swift against TelegramCore/SwiftSignalKit for iOS and macOS, with only the playback backend and the UI differing. Android gets one Java class.

```swift
// Apple: our module (no UIKit/AppKit in this file). Android: ListenTogetherPlayerAdapter.java, same members.
protocol LTPlayerBackend: AnyObject {           // one per platform, wraps the upstream player
    func load(_ e: LTEntry, message: Message)   // prepare paused; call core.ready when status != buffering && duration > 0
    func play(at posMs: Int64)                  // already corrected for (now - ref)
    func pause(at posMs: Int64)
    func seek(to posMs: Int64)
    func stop()
    func preload(_ message: Message)
    var position: (itemId: String, posMs: Int64, playing: Bool)? { get }
    func mirror(queue: [LTQueueRow], current: String?, participants: [PeerId]) // upstream list / mini-player UI
}
final class LTHooks {                           // the only symbol upstream patches call; no-ops when the feature flag is off
    static var shared: LTHooks?
    var isApplying = false                      // true while the adapter applies core commands → upstream doesn't re-report them
    func intercept(_ cmd: LTCommand, source: LTSource) -> Bool   // true = consumed (session active)
    func interceptInviteLink(bot: String, app: String, startParam: String) -> Bool
    func inviteCardButtonTitle(url: String) -> String?
    // + menu items for a message, header-bar state for a chat, mini-player participants
}
enum LTSource { case ui, remote /*lock screen, headset, car, media keys*/, notification, system /*call end, recorder, focus*/, auto /*end of track*/ }
```

**Rules every platform follows:**
1. Upstream patches are one-line calls into `LTHooks`. When the hook returns `false`, or the feature is off, upstream behaviour is unchanged.
2. During a session, `.ui`, `.remote` and `.notification` commands are consumed and sent to `lt_core_*`. The core then emits player events, and the adapter applies them with `isApplying = true`. **Local and remote actions take the same path**, so nothing is re-broadcast. This is also how the core's local-first rule works.
3. `.system` pauses (phone call, audio-focus loss, headphones unplugged, another video starting) become `lt_core_set_interrupted(1)` and are never broadcast. `.auto` end of track becomes `lt_core_player_ended`, with upstream auto-advance suppressed.
4. The upstream "previous" thresholds (iOS restarts the track after 5 s, Android after 10 s, macOS never restarts) are bypassed, because the core decides using PROTOCOL's 3 s rule.
5. Shuffle and repeat come from the core. Upstream's global settings (`MusicPlaybackSettings`, `SharedConfig.shuffleMusic/repeatMode`, mac `orderState/repeatState`) are ignored in session mode and must not be persisted from session UI. Playback rate is pinned to 1.0.

**How each platform implements the backend** (details in §1–3):

| | iOS | Android | macOS |
|---|---|---|---|
| Playlist object | Our `ListenTogetherPlaylist: SharedMediaPlaylist` (our module) | New `MediaController.setCustomPlaylist` (no sort, next = +1) | New `APListenTogetherController: APChatMusicController` (appended in the same file) |
| load (paused) | `setPlaylist(…, autoplay:false)` patch; then `.seek(t)` once the item exists | `playMessage` + `startPaused` patch + `seekToProgressMs` (EXACT seek) | `player.seek(timestamp:t, play:false)` in the subclass |
| play / pause | Bypass `musicMediaPlayer.control(.playback(.play/.pause))` (explicit, so no fade) | `playMessage(cur)` / `pauseMessage(cur, false)` (no 300 ms fade) | `play()` / `pause()` inside `performApplyingRemote` |
| seek | `.seek(sec)`; queued if not ready | `seekToProgressMs` (works before ready) | New `seek(to:play:)` that skips the `duration > 0` guard |
| position | `status.timestamp + (CACurrentMediaTime() − generationTimestamp)·baseRate` | `audioPlayer.player.getCurrentPosition()` + `SystemClock.elapsedRealtime()` (needs a getter) | `CMTimebaseGetTime(controller.timebase)` |
| started | 2nd `.playing` status (the first comes before the renderer runs) | Media3 `onIsPlayingChanged(true)` | First `.playing` status with `generationTimestamp != 0` |
| buffering | `status.status == .buffering` | Media3 `onPlaybackStateChanged(STATE_BUFFERING)` | New `playbackStatus` signal (upstream maps buffering to "paused") |
| ended | Our playlist sees `control(.next)` not sent by the bypass, i.e. `actionAtEnd` | Patched `STATE_ENDED` branch | Overridden `audioPlayerDidFinishPlaying()` |
| Command source tag | `MPRemoteCommand` handlers → `.remote` | `MusicPlayerReceiver` / two `MediaSession` callbacks → `.notification` / `.remote` | `MediaPlaybackControlCenter` closures → `.remote` |
| Keep alive while paused | Skip `dropAll()` + silent loop | Keep the foreground service (also on API ≤ 30) | `beginActivity` + keep worker connections |

---

## 1. iOS (Telegram-iOS @ 24638bc)

### 1.1 Playlist building
- **Tap on an audio bubble:** `OpenChatMessage` `.audio` → `mediaManager.setPlaylist((context, PeerMessagesMediaPlaylist(location: .messages(chatLocation, tagMask: .music, at: id))), type: .music, control: .playback(.play))` ([OpenChatMessage.swift L289–324](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/OpenChatMessage.swift#L289-L324)).
- **`.messages` is lazily paged.** `PeerMessagesMediaPlaylist` holds only `(current, around: prev/next)` and re-queries Postbox with `aroundMessageHistoryViewForLocation(count: 10)` on every navigation ([PeerMessagesMediaPlaylist.swift L578–612](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Components/MediaManager/PeerMessagesMediaPlaylist/Sources/PeerMessagesMediaPlaylist.swift#L578-L612)).
- **The explicit list already exists:** `PeerMessagesPlaylistLocation.custom(messages: Signal<([Message], Int32, Bool)>, canReorder:, at:, loadMore:, hidePanel:)` ([AccountContext MediaManager.swift L83](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/AccountContext/Sources/MediaManager.swift#L83)).
  - Saved/profile music (`.savedMusic`) is rewritten into `.custom` ([L107–145](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/AccountContext/Sources/MediaManager.swift#L107-L145)).
  - The signal is re-read on every next/previous ([L792–907](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Components/MediaManager/PeerMessagesMediaPlaylist/Sources/PeerMessagesMediaPlaylist.swift#L792-L907)), so a `Promise`-backed list is live-editable. Array order is queue order.
- **Don't use `.custom` as-is:**
  - Neighbours (`previousItem`/`nextItem`) are chosen by `MessageIndex`, which sorts by timestamp ([L529–537](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Components/MediaManager/PeerMessagesMediaPlaylist/Sources/PeerMessagesMediaPlaylist.swift#L529-L537)). Prefetch and the mini-player swipe carousel would pick the wrong track in a reordered or mixed-chat queue.
  - The playlist id is `.custom`, so bubbles in the host chat don't show "playing" ([FileMediaResourceStatus.swift L8–21](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/FileMediaResourceStatus/Sources/FileMediaResourceStatus.swift#L8-L21)).
  - The full player treats every `.custom` playlist as saved music ([OverlayAudioPlayerControllerNode.swift L110–116](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/OverlayAudioPlayerControllerNode.swift#L110-L116)).
  - The global order/looping settings are applied ([MediaManager.swift L561](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/MediaManager.swift#L561)).
- **Recommendation (no upstream patch):** add `ListenTogetherPlaylist: SharedMediaPlaylist` to our module ([protocol, SharedMediaPlayer.swift L212–224](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/AccountContext/Sources/SharedMediaPlayer.swift#L212-L224)), about 150–200 lines.
  - Items: the public `MessageMediaPlaylistItem` ([L52–156](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Components/MediaManager/PeerMessagesMediaPlaylist/Sources/PeerMessagesMediaPlaylist.swift#L52-L156)). It carries a `FileMediaReference.message`, so file references refresh automatically.
  - `location` = `.custom(queueSignal, canReorder: true, …)`, so tapping the mini-player opens the full player on our queue.
  - `id.isEqual(to:)` returns true for `.peer(x)` for every chat `x` in the queue, so bubbles light up.
  - `nextItem`/`previousItem` follow queue order, and `setOrder`/`setLooping` are ignored.
- **Host-chat "next 200 tracks" (PLAN §2.1):** read `postbox.aroundMessageHistoryViewForLocation(…, tag: .music)` the way `loadItem` does, then hand the messages to the core as entries.

### 1.2 Programmatic control
- **Surface:** `SharedMediaPlayerControlAction = .next | .previous | .playback(.play/.pause/.togglePlayPause) | .seek(Double) | .setOrder | .setLooping | .setBaseRate` ([SharedMediaPlayer.swift L226–245](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/AccountContext/Sources/SharedMediaPlayer.swift#L226-L245)). Entry: `MediaManagerImpl.setPlaylist` ([L485–586](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/MediaManager.swift#L485-L586)) and `playlistControl` ([L588–612](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/MediaManager.swift#L588-L612)).
- **There's no "jump to item".** Upstream calls `setPlaylist` again with a new `at` ([OverlayAudioPlayerControllerNode.swift L502–523](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/OverlayAudioPlayerControllerNode.swift#L502-L523)). Our playlist can simply change `currentItem`; `SharedMediaPlayer` swaps the `MediaPlayer` when the item changes.
- **`setPlaylist` always plays.** It ends with `control(control)` then `control(.playback(.play))` ([L554–582](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/MediaManager.swift#L554-L582)). Without a `.seek`, a stored resume position for tracks of 10 minutes or more overrides the start.
- **`SharedMediaPlayer`** ([file](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/SharedMediaPlayer.swift)):
  - `control()` is at [L429–478](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/SharedMediaPlayer.swift#L429-L478). `.previous` restarts the track after 5 s.
  - A seek before the item exists is stored as "scheduled play at t" ([L455–461](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/SharedMediaPlayer.swift#L455-L461)) and applied as `seek(t); play()` ([L274–302](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/SharedMediaPlayer.swift#L274-L302)). A scheduled `.pause` ignores the scheduled time.
  - So **load paused at t** = `setPlaylist(…, control: .playback(.pause))` with the new `autoplay:false`, then `.seek(t)` once the item exists (a paused seek stays paused). Alternative: a 5-line patch that honours the scheduled time on `.pause`.
- **End of track:** `actionAtEnd` → `control(.next)` with scheduled play ([L253–266](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/SharedMediaPlayer.swift#L253-L266)). This is internal and never passes through `MediaManager`. At the end of the queue, `playedToEnd` pauses ([L306–323](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/SharedMediaPlayer.swift#L306-L323)).
- **Prefetch:** once the current file is complete, the neighbour is fetched ([L491–538](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/SharedMediaPlayer.swift#L491-L538)). This is correct with our playlist.
- **State:** `mediaManager.musicMediaPlayerState` ([L114–146](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/MediaManager.swift#L114-L146)) → `SharedMediaPlayerItemPlaybackState(item, status, …)`.
- **Position: timestamp plus rate, event-driven.** `MediaPlayerStatus{generationTimestamp, timestamp, baseRate, status: .playing/.paused/.buffering, seekId, duration}` ([MediaPlayer.swift L1089–1109](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/MediaPlayer/Sources/MediaPlayer.swift#L1089-L1109)).
  - It is emitted only on changes: state, buffer edge, rate. While buffering it re-ticks every 0.3 s ([tick L815–1057](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/MediaPlayer/Sources/MediaPlayer.swift#L815-L1057)).
  - Consumers extrapolate it, e.g. `MediaPlayerTimeTextNode` with a 0.2 s timer ([L159–180](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/MediaPlayer/Sources/MediaPlayerTimeTextNode.swift#L159-L180)). The adapter answers `PLAYER_QUERY` the same way.
  - The clock is the audio timebase, anchored at the first render callback and re-synced when drift exceeds 10 ms ([MediaPlayerAudioRenderer.swift L77–118](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/MediaPlayer/Sources/MediaPlayerAudioRenderer.swift#L77-L118)). Output latency is not included; `AVAudioSession.outputLatency` is available if ever needed.
- **Seek:** `MediaPlayer.seek(timestamp:play:)` ([L1232–1242](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/MediaPlayer/Sources/MediaPlayer.swift#L1232-L1242)).
  - Each seek rebuilds the FFmpeg frame source ([L276–375](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/MediaPlayer/Sources/MediaPlayer.swift#L276-L375)), so it costs a range fetch when streaming.
  - Calling `play()` during a seek switches the pending action ([L485–560](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/MediaPlayer/Sources/MediaPlayer.swift#L485-L560)).
  - `togglePlayPause` fades for 250 ms; explicit play and pause don't.
- **Rate:** `.setBaseRate` exists, and music starts at 1.0. The full player's long-press fast-forward uses x4–x16, which must be blocked or turned into seeks in a session.

### 1.3 Remote and system commands
- **All system media commands** are registered in one place: `MPRemoteCommandCenter` play, pause, togglePlayPause, next, previous and changePlaybackPosition, registered only while a music item is active ([MediaManager.swift L297–326](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/MediaManager.swift#L297-L326)).
  - Handlers: [L658–692](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/MediaManager.swift#L658-L692). Each calls `self.playlistControl(…, type: nil)`.
  - These cover the lock screen, Control Center, AirPods, wired headset, Bluetooth AVRCP and the CarPlay Now Playing screen.
  - Now Playing info is set at [L247–295](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/MediaManager.swift#L247-L295).
- **There's no CarPlay audio app**; only `carplay-messaging` is entitled ([Telegram/BUILD L503–507](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/Telegram/BUILD#L503-L507)).
- **In-app UI** also ends in `playlistControl`/`setPlaylist`:
  - Mini-player ([MediaPlaybackHeaderPanelComponent.swift L150–258](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Components/MediaPlaybackHeaderPanelComponent/Sources/MediaPlaybackHeaderPanelComponent.swift#L150-L258)). Beware: `playPrevious` sends `.next`, because the names follow the swipe direction.
  - Full player ([OverlayAudioPlayerControllerNode.swift L465–469](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/OverlayAudioPlayerControllerNode.swift#L465-L469)).
  - Bubbles, queue-row taps and the mini-player close button (`setPlaylist(nil)`).
- **Patch:** add an interceptor plus an `LTSource` to `playlistControl` and `setPlaylist` in that one file. The objc handlers pass `.remote`. Add a bypass `applyControl` for the adapter.
- **Tag as `.system`:**
  - Call end → `playlistControl(.play)` ([SharedAccountContext.swift L899–904](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/SharedAccountContext.swift#L899-L904)).
  - The voice recorder's pause ([ChatControllerMediaRecording.swift L484](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/Chat/ChatControllerMediaRecording.swift#L484)).
- **Seen only as a status change:**
  - An audio-session interruption pauses the player directly and doesn't auto-resume ([ManagedAudioSession.swift L554–571](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramAudio/Sources/ManagedAudioSession.swift#L554-L571)).
  - Headphone removal pauses it too ([MediaPlayerAudioRenderer.swift L433–475](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/MediaPlayer/Sources/MediaPlayerAudioRenderer.swift#L433-L475)).
  - The adapter maps an unexpected `.paused` while the session says playing to `set_interrupted`.

### 1.4 Background
- **`UIBackgroundModes`** includes `audio` ([Telegram/BUILD L1807–1815](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/Telegram/BUILD#L1807-L1815)). Music uses the `.playback` category via `ManagedAudioSession` ([L68–90](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramAudio/Sources/ManagedAudioSession.swift#L68-L90)).
- **Released on pause:** in the background while not playing, `shouldKeepAudioSession` triggers `audioSession.dropAll()` after 0.8 s ([MediaManager.swift L378–405](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/MediaManager.swift#L378-L405), [L441–448](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/MediaManager.swift#L441-L448)). iOS then suspends the app, so nothing keeps it alive while paused.
- **Keep-awake (PLAN §2.4):** skip `dropAll()` while a session is active (2–3 lines). Our module plays a silent loop on the already-active `.playback` session while paused in the background, for up to 60 minutes, then leaves.

### 1.5 Media resolution (TelegramCore; also macOS)
- **Streaming:** `MediaPlayer` streams from `postbox.mediaBox` with `streamable: .conservative`, stalling and resuming at 1, 2 and 3 s of buffer ([MediaPlayer.swift L67–97](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/MediaPlayer/Sources/MediaPlayer.swift#L67-L97)). Range fetches go through `fetchedMediaResource` ([FetchedMediaResource.swift L28](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramCore/Sources/Network/FetchedMediaResource.swift#L28)). The media box is the cache, so a downloaded track plays instantly.
  - Prefetch: `engine.resources.fetch(reference:…)`.
- **File references refresh automatically.** On `FILE_REFERENCE_*`, `revalidateMediaResourceReference` ([L866–929](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramCore/Sources/Network/FetchedMediaResource.swift#L866-L929)) re-fetches the message via `fetchRemoteMessage` ([SearchMessages.swift L774](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramCore/Sources/TelegramEngine/Messages/SearchMessages.swift#L774)). It uses the channel access hash stored in the `MessageReference`, so it works for non-joined public channels too. PLAN §4.1's "refresh by re-fetching the message" is already built in.
- **Fetch by (peer, id):** `engine.messages.getMessagesLoadIfNecessary(ids, strategy: .cloud(skipLocal: false))` ([TelegramEngineMessages.swift L356–358](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramCore/Sources/TelegramEngine/Messages/TelegramEngineMessages.swift#L356-L358); impl [LoadMessagesIfNecessary.swift L21–135](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramCore/Sources/TelegramEngine/Messages/LoadMessagesIfNecessary.swift#L21-L135)). It calls `channels.getMessages` or `messages.getMessages` and stores the results.
  - **It silently skips ids whose peer isn't in Postbox** ([L61](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramCore/Sources/TelegramEngine/Messages/LoadMessagesIfNecessary.swift#L61)).
  - Its `minAvailableMessageId` filter looks inverted ([L25–29](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramCore/Sources/TelegramEngine/Messages/LoadMessagesIfNecessary.swift#L25-L29)). Test supergroups with hidden history.
  - `downloadMessage` ([L222–226](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramCore/Sources/TelegramEngine/Messages/TelegramEngineMessages.swift#L222-L226)) waits forever if the peer is unknown. Avoid it.
- **Public channel not joined:**
  1. `engine.peers.resolvePeerByName(name:referrer:)` ([ResolvePeerByName.swift L16–72](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramCore/Sources/TelegramEngine/Peers/ResolvePeerByName.swift#L16-L72)) stores the channel and its access hash.
  2. Then call `getMessagesLoadIfNecessary`.

  This is why queue entries need the username (Key finding 9).

### 1.6 UI hook points
- **Full player:** `OverlayAudioPlayerControlsNode.updateLayout` ([L831–972](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/OverlayAudioPlayerControlsNode.swift#L831-L972)).
  - The info row has the share button at the top right ([L852](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/OverlayAudioPlayerControlsNode.swift#L852)). **A small "Listen Together" icon button goes just left of share**, about 15 lines.
  - Alternative: the existing full-width 52 pt glass "Add to Profile" button row ([L900–966](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/OverlayAudioPlayerControlsNode.swift#L900-L966)).
  - In a session, `orderPressed`/`loopingPressed` ([L1003–1037](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/OverlayAudioPlayerControlsNode.swift#L1003-L1037)) route to the core and must not persist the global setting.
  - A "Listening with …" title can reuse the saved-music header path ([OverlayAudioPlayerControllerNode.swift L1000–1080](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/OverlayAudioPlayerControllerNode.swift#L1000-L1080)).
- **Mini-player:** `MediaNavigationAccessoryHeaderNode` (40 pt).
  - The subtitle "Listening with A, B" goes in `MediaHeaderItemNode.updateLayout` ([L45–126](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Components/MediaPlaybackHeaderPanelComponent/Sources/MediaNavigationAccessoryHeaderNode.swift#L45-L126)).
  - A 20–22 pt `AnimatedAvatarSetNode` goes in the rate-button slot ([updateLayout L431–483](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Components/MediaPlaybackHeaderPanelComponent/Sources/MediaNavigationAccessoryHeaderNode.swift#L431-L483)).
  - Patch this copy. `submodules/TelegramBaseController/…/MediaNavigationAccessory*` is an older duplicate.
- **Queue:** the full player's list is a `ChatHistoryListNode` of `ListMessageFileItemNode` rows.
  - **Drag-reorder exists but is only wired for your own saved music** (`setupReordering`, [L678–728](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/OverlayAudioPlayerControllerNode.swift#L678-L728)). Enabling it for our location is about 15 lines.
  - **There's no swipe-to-remove.** Add "Remove from queue" to the long-press menu next to the existing remove item ([L1642–1650](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/OverlayAudioPlayerControllerNode.swift#L1642-L1650)).
  - "Added by" avatars need about 40 lines in `ListMessageFileItemNode`.
- **Audio message context menu:** `contextMenuForChatPresentationInterfaceState` ([ChatInterfaceStateContextMenus.swift L493](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/ChatInterfaceStateContextMenus.swift#L493)). The music "Save to Files" item is at [L1559–1571](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/ChatInterfaceStateContextMenus.swift#L1559-L1571).
  - Add "Add to Listen Together queue" and "Share to [chat] & queue" **outside** the `!isCopyProtected` block, because protected chats can still play.
- **"Listening now · Join" bar:** chat header panels are a list of `HeaderPanelContainerComponent.Panel` items (topics 0, media 1, live location 2, group call 3), built in `ChatControllerNode.containerLayoutUpdated` ([L1552–1660](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/ChatControllerNode.swift#L1552-L1660)).
  - Append `key: "listenTogether", orderIndex: 4`, about 20–30 lines including a state subscription.
  - Copy the layout of `GroupCallNavigationAccessoryPanel` (title and subtitle, centred avatar stack, gradient "Join" pill; [L64–199](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Components/GroupCallHeaderPanelComponent/Sources/GroupCallNavigationAccessoryPanel.swift#L64-L199)) into our component. It's `final` and bound to `GroupCall` data.
  - Reuse the public `AnimatedAvatarSetNode` ([L219–300](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/AnimatedAvatarSetNode/Sources/AnimatedAvatarSetNode.swift#L219-L300)).
- **Invite card:** the bubble picks `ChatMessageWebpageBubbleContentNode` for previews ([ChatMessageBubbleItemNode.swift L408–426](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Components/Chat/ChatMessageBubbleItemNode/Sources/ChatMessageBubbleItemNode.swift#L408-L426)). `"telegram_botapp"` gets the "OPEN APP" button ([ChatMessageWebpageBubbleContentNode.swift L476–478](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Components/Chat/ChatMessageWebpageBubbleContentNode/Sources/ChatMessageWebpageBubbleContentNode.swift#L476-L478)).
  - Relabel it when `content.url` is our app, 5–10 lines. The tap flows `openUrl` → our §1.7 intercept.

### 1.7 Deep links
- **Parse:** `parseInternalUrl` turns `t.me/<bot>/<app>?startapp=` into `.peer(.name(bot), .appStart(app, startapp, mode))` ([UrlHandling.swift L822–845](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/UrlHandling/Sources/UrlHandling.swift#L822-L845)).
- **Resolve:** `resolveInternalUrl` calls `resolvePeerByName` + `getBotApp` and returns `.peer(peer, .withBotApp(ChatControllerInitialBotAppStart(botApp, payload: token, …)))` ([L1086–1106](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/UrlHandling/Sources/UrlHandling.swift#L1086-L1106)).
- **`tg://` and our scheme:** `tg://resolve?…&appname=&startapp=` and the app-specific scheme (`tunegram://`, which the landing page uses for "Open in Tunegram") are treated like `tg://` ([OpenUrl.swift L445–453](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/OpenUrl.swift#L445-L453)) and converge in the same place.
- **Intercept:** `openResolvedUrlImpl` `case let .peer(peer, navigation)` ([OpenResolvedUrl.swift L138](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/OpenResolvedUrl.swift#L138-L143)). Add `if case .withBotApp(let s) = navigation, LTHooks…interceptInviteLink(…) { return }`, about 5 lines.
  - It covers chat links, card taps, QR codes, universal links and `tg://`.
  - Optional: recognise our bot and app names in `.appStart` before `getBotApp` to save a round trip (about 6 lines).

### 1.8 Silent Mini App auth (TelegramCore; also macOS)
- **TL calls** in [BotWebView.swift](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramCore/Sources/TelegramEngine/Messages/BotWebView.swift):
  - `messages.requestSimpleWebView` ([L19–72](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramCore/Sources/TelegramEngine/Messages/BotWebView.swift#L19-L72)).
  - `messages.requestMainWebView` ([L74–126](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramCore/Sources/TelegramEngine/Messages/BotWebView.swift#L74-L126)).
  - `messages.requestWebView` ([L254](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramCore/Sources/TelegramEngine/Messages/BotWebView.swift#L254)).
  - `messages.requestAppWebView(app, start_param, write_allowed)` ([L363–426](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramCore/Sources/TelegramEngine/Messages/BotWebView.swift#L363-L426)).
- **initData** comes back in `RequestWebViewResult.url` ([L166–194](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramCore/Sources/TelegramEngine/Messages/BotWebView.swift#L166-L194)), in the fragment `#tgWebAppData=…`. The client never parses it.
- **Public wrappers** ([TelegramEngineMessages.swift L942–1008](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramCore/Sources/TelegramEngine/Messages/TelegramEngineMessages.swift#L942-L1008)): `getBotApp(botId:shortName:)`, `requestAppWebView(peerId:appReference:payload:themeParams:compact:fullscreen:allowWrite:)` and `requestMainWebView(peerId:botId:source:themeParams:)`.
  - **Silent sign-in** = `resolvePeerByName(bot)` → `getBotApp` → `requestAppWebView(peerId: bot, appReference: .id(…), payload: nil, allowWrite: false)` → parse the fragment → POST it to `/v1/auth`.
- **Consent is client-side only.** `presentBotApp` ([ChatControllerOpenWebApp.swift L739–941](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/Chat/ChatControllerOpenWebApp.swift#L739-L941)) shows `webAppLaunchConfirmationController`, with a write-access checkbox when the app is `.notActivated` and `.requiresWriteAccess`.
  - It is skipped when the per-bot notice is set or the bot is in `whitelisted_bots`.
  - Attach-menu bots also get a terms sheet and `addBotToAttachMenu` ([L887–893](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramUI/Sources/Chat/ChatControllerOpenWebApp.swift#L887-L893)).
  - The wrapper call skips all of this. Nothing is installed client-side, no chat is created, and write access is granted only with `allowWrite: true`.
- **Raw TL isn't possible from our module on master.** `Network.request` is `internal` ([Network.swift L1421](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramCore/Sources/Network/Network.swift#L1421)); it was `public` at 13.0.0. Any future TL call that lacks a wrapper needs a TelegramCore patch.

### 1.9 Sending the invite (TelegramCore; also macOS)
- **Text with a link:** `enqueueMessages(account:peerId:messages: [.message(text:attributes:…)])` ([EnqueueMessage.swift L761](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramCore/Sources/PendingMessages/EnqueueMessage.swift#L761); case at [L172–174](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramCore/Sources/PendingMessages/EnqueueMessage.swift#L172-L174)). It goes through the normal pending queue, with offline retry.
  - No preview: `OutgoingContentInfoMessageAttribute(flags: [.disableLinkPreviews])`.
  - Forced or positioned preview: `WebpagePreviewMessageAttribute(…)` ([TelegramMediaWebpage.swift L104–112](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramCore/Sources/ApiUtils/TelegramMediaWebpage.swift#L104-L112)).
  - With neither, the server builds the preview, which the invite card needs.
- **Inline bot:** `engine.messages.requestChatContextResults(botId:peerId:query:offset:)` ([L573](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramCore/Sources/TelegramEngine/Messages/TelegramEngineMessages.swift#L573)) → `enqueueOutgoingMessageWithChatContextResult(to:threadId:botId:result:…)` ([L517–523](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramCore/Sources/TelegramEngine/Messages/TelegramEngineMessages.swift#L517-L523)), sent as `messages.sendInlineBotResult`.

### 1.10 Differences at `release-13.0.0` (f1dd7a2)
- No player, playlist, mini-player, WebUI, UrlHandling or header-panel file was added, moved or removed.
- **Byte-identical:**
  - `PeerMessagesMediaPlaylist.swift`, so `.custom` and saved music exist in 13.0.0.
  - Both `MediaManager.swift` files, both `SharedMediaPlayer.swift` files, `MediaPlayer.swift` and `MediaPlayerAudioRenderer.swift`.
  - `ChatControllerOpenWebApp.swift`, `MediaPlaybackHeaderPanelComponent.swift` and `GroupCallNavigationAccessoryPanel.swift`.
- **Same hooks at shifted lines:** e.g. `OpenResolvedUrl` `.peer` is at L99 (master L138), the context-menu music item at L1553, chat header panels at L1449, and `telegram_botapp` at L462.
- **The one material difference:** `Network.request` is `public` at 13.0.0 ([Network.swift L1306 @ f1dd7a2](https://github.com/TelegramMessenger/Telegram-iOS/blob/f1dd7a2dbd02cbbf513e75d5695d8d36d1cf5838/submodules/TelegramCore/Sources/Network/Network.swift#L1306)), so raw TL calls work there. The patch plan is otherwise the same.

---

## 2. Android (Telegram-Android @ 21f7d10)

### 2.1 Playlist building
- **Tap on an audio bubble:** `ChatActivity` calls `setPlaylist(chatAdapter.getMessages(), msg, mergeDialogId, !isFiltered, null, topicId)` ([ChatActivity.java L40454](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/ui/ChatActivity.java#L40454)).
- **`setPlaylist(ArrayList<MessageObject>, current, mergeDialogId, loadMusic, params, topicId)`** ([MediaController.java L2947–2997](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/MediaController.java#L2947-L2997)):
  - It keeps only `isMusic()` messages and **always calls `sortPlaylist()`**, which sorts by message id ([L2999–3017](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/MediaController.java#L2999-L3017)).
  - With `loadMusic`, it reads up to 1000 neighbours from the local DB.
- **Lazy paging:** `loadMoreMusic()` ([L2831–2933](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/MediaController.java#L2831-L2933)) pages chat history (`loadMedia`, 50 at a time), in-chat or global search (`PlaylistGlobalSearchParams`, 20 at a time) and saved/profile music (`currentSavedMusicList.load()`).
  - With `loadMusic=false` there is no paging, but the list loops at the end.
  - Profile music bypasses `setPlaylist` and edits `getPlaylist()` directly ([ProfileActivity.java L3851–3866](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/ui/ProfileActivity.java#L3851-L3866)).
- **Order:** chat lists are stored oldest-first, and "next" = index − 1 ([L3134–3137](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/MediaController.java#L3134-L3137)). Shuffle, repeat and reverse are global `SharedConfig` settings.
- **Substitute:** add `setCustomPlaylist(ArrayList<MessageObject> ordered, int start)` with no sort and a `customOrder` flag (next = +1, cleared in `clearPlaylist()`). `AudioPlayerAlert` should read the same order predicate ([L2529–2535](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/ui/Components/AudioPlayerAlert.java#L2529-L2535)). About 25 lines.
  - Beware: `playlistMap` is keyed by message id only, so ids can collide across chats.

### 2.2 Programmatic control
- **Public methods on `MediaController` (UI thread):**
  - `playMessage` ([L3721–4227](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/MediaController.java#L3721-L4227)): resumes if it's the same message, otherwise creates a new `VideoPlayer` (ExoPlayer) per track.
  - `pauseMessage(msg, smoothFade)` ([L4404–4459](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/MediaController.java#L4404-L4459)): fades for 300 ms when the activity is resumed; remote pauses use `false`.
  - `seekToProgressMs(msg, ms)` ([L2742–2774](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/MediaController.java#L2742-L2774)): EXACT seek, works before ready, no upstream callers.
  - `playNextMessage`/`playPreviousMessage` ([L3023–3132](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/MediaController.java#L3023-L3132)): previous restarts the track after 10 s.
  - `findMessageInPlaylistAndPlay` ([L3027–3035](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/MediaController.java#L3027-L3035)): the jump used by queue taps.
  - `cleanupPlayer(notify, stopService)` ([L2552–2695](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/MediaController.java#L2552-L2695)).
- **Position is polled.** A 17 ms timer reads ExoPlayer and posts `messagePlayingProgressDidChanged` with a **fraction** ([L1682–1746](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/MediaController.java#L1682-L1746)). The MediaSession only gets whole seconds.
  - The adapter reads `audioPlayer.player.getCurrentPosition()` (`VideoPlayer.player` is public, [VideoPlayer.java L154](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/ui/Components/VideoPlayer.java#L154)) with `SystemClock.elapsedRealtime()`. It needs a getter for the private `audioPlayer`.
- **End of track:** the `onStateChanged` `STATE_ENDED` branch auto-advances via `playNextMessageWithoutOrder(true)` ([L3976–3988](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/MediaController.java#L3976-L3988)). Patch it to emit `ended` in a session.
- **Errors and buffering:** `onError` is empty, so player errors are swallowed. Add a Media3 `Player.Listener` next to the existing `MusicListenReporter` listener ([L4107–4109](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/MediaController.java#L4107-L4109)) to get buffering, ready, started and errors.
- **No prepare-paused:** `audioPlayer.play()` is always called ([L4111](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/MediaController.java#L4111)). Upstream's own workaround, `restoreMusicPlaylistState` (play then pause), risks an audible blip ([L7482–7518](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/MediaController.java#L7482-L7518)).
  - Patch: `startPaused` / `startPositionMs` consumed at L4111.
- **Rate:** `setPlaybackSpeed` ([L3373–3411](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/MediaController.java#L3373-L3411)) rounds to 0.1, persists globally, and applies to music only for tracks of 10 minutes or more.
- **Buffering thresholds:** `bufferForPlaybackMs = 100` ([VideoPlayer.java L233–248](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/ui/Components/VideoPlayer.java#L233-L248)), so the start moment is "when READY". The core corrects after `started`.

### 2.3 Remote and system commands
- **Precedent:** `CastSync` mirrors play, pause, seek and speed to Chromecast. Each forward call is guarded by `if (!ignorePlayerUpdate)`, and remote state is applied in `syncCastedPlayer()` with the flag set ([MediaController.java L4229–4251](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/MediaController.java#L4229-L4251), [CastSync.java](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/ui/CastSync.java)). `LT.isApplying` copies this.
- **Entry points, all ending in `MediaController`:**
  1. Notification buttons → `MusicPlayerReceiver` ([L55–83](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/MusicPlayerReceiver.java#L55-L83)). Swiping the notification away sends `STOP_PLAYER` → `cleanupPlayer(true,true)`.
  2. `MusicPlayerService` MediaSession callback ([L115–231](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/MusicPlayerService.java#L115-L231)) covers the lock screen, Android 13+ quick-settings player, Bluetooth, wired headset, keyboard media keys and Wear.
  3. Legacy `MEDIA_BUTTON` in `MusicPlayerReceiver` ([L20–54](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/MusicPlayerReceiver.java#L20-L54)), only on pre-Lollipop and MIUI.
  4. Android Auto, media browser and Car App → **a second session**, `TelegramMediaSession.SessionCallback` ([L612–786](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/TelegramMediaSession.java#L612-L786)). It includes `onSkipToQueueItem` and `onPlayFromMediaId` → `setPlaylist`, and its `PlaybackState` publishing is dead code at this commit.
- **Tag the source** at those three classes (1–2 lines each). The default is `.ui`. The in-app UI makes about 25 call sites per method, so patch the choke point, not the callers.
- **Internal callers to tag:**
  - Auto-advance.
  - `onAudioFocusChange` ([L1629–1656](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/MediaController.java#L1629-L1656)).
  - `playerDidStartPlaying`, where another video pauses music ([L2050–2058](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/MediaController.java#L2050-L2058)).
  - `restoreMusicPlaylistState` after voice messages, `fileLoaded` re-entry and `resumeByRewind`.
  - `AUDIO_BECOMING_NOISY` ([MusicPlayerService.java L80–87](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/MusicPlayerService.java#L80-L87)).
  - VoIP pause.

### 2.4 Background
- **Manifest:** `FOREGROUND_SERVICE_MEDIA_PLAYBACK`, and `MusicPlayerService` declared with `foregroundServiceType="mediaPlayback"` ([AndroidManifest.xml L534–548](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/AndroidManifest.xml#L534-L548)).
- **Start:** `playMessage` uses plain `startService` ([L4198–4212](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/MediaController.java#L4198-L4212)). `onStartCommand` → `startForeground`.
- **While paused** ([MusicPlayerService.java L548–564](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/MusicPlayerService.java#L548-L564)): **API 31+ stays foreground.** API 30 and lower call `stopForeground(false)`, so the process becomes killable.
- **Teardown:** `cleanupPlayer(…, stopService=true)` and a play-state change with no message both call `stopSelf()` ([L834–841](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/MusicPlayerService.java#L834-L841)).
  - From the background on Android 12+, a foreground service can't be restarted, and `startService` throws on Android 8+.
  - **In a session, never stop the service.** Keep the foreground notification (on API ≤ 30 too) until the session ends. This also keeps the core's socket alive while paused (PLAN §2.4).

### 2.5 Media resolution
- **Streaming:** `playMessage` builds `tg://…?rid=<FileLoader.getFileReference(msg)>` for ExoPlayer ([L4032–4045](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/MediaController.java#L4032-L4045)). `FileStreamLoadOperation` reads ranges through `FileLoader` and fills the normal cache ([L87–110](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/FileStreamLoadOperation.java#L87-L110)).
  - The next track is prefetched in playlist order, which is correct once our order is right.
- **File references:** `FileLoadOperation` → `FileRefController.requestReference` ([FileLoadOperation.java L2271–2288](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/FileLoadOperation.java#L2271-L2288)), which re-fetches the message with `channels.getMessages` or `messages.getMessages` ([FileRefController.java L554–583](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/FileRefController.java#L554-L583)). It needs the channel access hash cached in `MessagesController`.
- **Fetch by id:** there's no public helper. Copy the inline `TL_channels_getMessages` → `new MessageObject(account, msg, false, true)` from [AudioPlayerAlert.java L2771–2786](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/ui/Components/AudioPlayerAlert.java#L2771-L2786). Try the local DB first with `MessagesStorage.getMessage(dialogId, msgId)`, off the UI thread.
- **Public channel not joined:** `getUserNameResolver().resolve(username, …)` → `contacts.resolveUsername`, which stores the chat and access hash in memory and the DB ([UserNameResolver.java L36–114](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/UserNameResolver.java#L36-L114)). Then call `channels.getMessages`.

### 2.6 UI hook points
- **Full player (`AudioPlayerAlert`):**
  - The bottom row is hard-coded to 5 buttons ([L764–775](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/ui/Components/AudioPlayerAlert.java#L764-L775)).
  - **Put "Listen Together" in the options menu**, `showMenuOptions` ([L2834–2878](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/ui/Components/AudioPlayerAlert.java#L2834-L2878)), where Chromecast already sits ([L1101–1120](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/ui/Components/AudioPlayerAlert.java#L1101-L1120)).
  - The `saveToProfileButton` slot ([L1271–1286](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/ui/Components/AudioPlayerAlert.java#L1271-L1286)) suits a "Listening with …" row.
- **Mini-player (`FragmentContextView` `STYLE_AUDIO_PLAYER`, 36 dp):**
  - Content is set in `checkPlayer` ([L1845–2037](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/ui/Components/FragmentContextView.java#L1845-L2037)).
  - **It already owns an `AvatarsImageView`** ([L557–561](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/ui/Components/FragmentContextView.java#L557-L561)), hidden in the audio style ([L1279](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/ui/Components/FragmentContextView.java#L1279)). Show it and change the title text.
- **Queue:**
  - `ItemTouchHelper` drag-reorder exists only when `isMyList()`, and swipe is off ([L1391–1452](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/ui/Components/AudioPlayerAlert.java#L1391-L1452)).
  - `AudioPlayerCell` already supports a reorder handle ([L176–191](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/ui/Cells/AudioPlayerCell.java#L176-L191)).
  - Enabling both plus `onSwiped` → `lt_core_remove` is about 30 lines. An "added by" avatar in `AudioPlayerCell.onDraw` is about 30–40.
- **Context menu:** `ChatActivity.fillMessageMenu` music branches ([L46768–46771](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/ui/ChatActivity.java#L46768-L46771), [L46795–46801](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/ui/ChatActivity.java#L46795-L46801)) plus `processSelectedOption` ([L34290](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/ui/ChatActivity.java#L34290)). Use new option ids above 118 ([L1299](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/ui/ChatActivity.java#L1299)).
- **"Listening now · Join" bar:** clone `STYLE_INACTIVE_GROUP_CALL` (48 dp: avatars, title, subtitle, "Join" pill; [L1309–1350](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/ui/Components/FragmentContextView.java#L1309-L1350)) as a new `STYLE_LISTEN_TOGETHER`.
  - Hook it into `checkCall`/`checkPlayer`/`onClick` ([L750–835](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/ui/Components/FragmentContextView.java#L750-L835)).
  - The styles are mutually exclusive, so this is about 100–150 lines. Priority: group call, then Listen Together, then player.
- **Invite card:** `ChatMessageCell` maps `"telegram_botapp"` to card type 15 ([L7045–7047](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/ui/Cells/ChatMessageCell.java#L7045-L7047)), with the "OPEN APP" button text at [L12066–12067](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/ui/Cells/ChatMessageCell.java#L12066-L12067).
  - The click goes `didPressInstantButton` → `Browser.openUrl` ([ChatActivity.java L42885–42930](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/ui/ChatActivity.java#L42885-L42930)), which reaches our §2.7 intercept.
  - Relabelling is about 10 lines. A custom bubble in the 24k-line cell isn't worth it.

### 2.7 Deep links
- **Routing:** `Browser.openUrl` → `openAsInternalIntent` → `LaunchActivity` ([Browser.java L531–551](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/browser/Browser.java#L531-L551)). External intents go to `handleIntent` ([L1553](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/ui/LaunchActivity.java#L1553)).
- **Parse:** `t.me/<bot>/<app>?startapp=` gives `botAppMaybe` and `startApp` ([L2220–2258](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/ui/LaunchActivity.java#L2220-L2258)). `tg://resolve?domain=&appname=&startapp=` is parsed at [L2338–2355](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/ui/LaunchActivity.java#L2338-L2355). Both call `runLinkRequest`.
- **Intercept: the first statement of `runLinkRequest`** ([L4139](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/ui/LaunchActivity.java#L4139)): `if (state == 0 && botAppMaybe != null && startApp != null && LT.tryHandleLink(…)) return;` (about 3 lines).
  - That's before `processWebAppBot` / `getBotApp` and every dialog ([L5630](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/ui/LaunchActivity.java#L5630)).
  - Note: `t.me/<user>/<number>` also sets `botAppMaybe`. That's harmless, because we compare against our app name.
- **Landing page:** the fork registers `tg://` and `t.me`, both shared with official Telegram ([manifest L259–276](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/AndroidManifest.xml#L259-L276)). "Open in Tunegram" on Android should use an `intent://…;package=app.tunegram.android` URL.

### 2.8 Silent Mini App auth
- **Requests** are built in `BotWebViewSheet.requestWebView`:
  - `TL_messages_requestAppWebView` ([L1661–1695](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/ui/bots/BotWebViewSheet.java#L1661-L1695)).
  - `TL_messages_requestMainWebView` ([L1696–1726](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/ui/bots/BotWebViewSheet.java#L1696-L1726)).
  - Simple and menu variants above those.
- **initData** comes back in `TL_webViewResultUrl.url` ([TLRPC.java L62751–62759](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/tgnet/TLRPC.java#L62751-L62759)). The client never parses the fragment.
- **Consent, only on the link path** (`processWebAppBot`, [L5705–5757](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/ui/LaunchActivity.java#L5705-L5757)):
  - `botApp.inactive && attachable` → disclaimer.
  - `request_write_access` → `createBotLaunchAlert`.
  - Nothing is added to the chat list or attach menu by the client.
- **Silent:** `ConnectionsManager.sendRequest(TL_messages_getBotApp)` → `TL_messages_requestAppWebView{app, platform="android", write_allowed=false}`, or `TL_messages_requestMainWebView`. Raw TL works from any class on Android.

### 2.9 Sending the invite
- **Text:** `SendMessagesHelper.getInstance(acc).sendMessage(SendMessageParams.of(text, dialogId))` ([L4441](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/SendMessagesHelper.java#L4441); params [L12333–12371](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/SendMessagesHelper.java#L12333-L12371)).
  - `searchLinks=false` sends `no_webpage` ([L5487–5488](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/SendMessagesHelper.java#L5487-L5488)).
  - `mediaWebPage` forces a preview ([L5392–5407](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/SendMessagesHelper.java#L5392-L5407)).
  - Respect `ChatObject.canSendEmbed`, and pay stars in paid-message chats.
- **Inline bot:** `TL_messages_getInlineBotResults` → `prepareSendingBotContextResult(…, params{id, query_id, bot, bot_name}, dialogId, …)` ([L10268–10273](https://github.com/TelegramMessenger/Telegram-Android/blob/21f7d109da6ab3506c9e9577f98c6a38771ab335/TMessagesProj/src/main/java/org/telegram/messenger/SendMessagesHelper.java#L10268-L10273)), sent as `sendInlineBotResult`.

---

## 3. macOS (telegram-macos @ 38309ec)

The app was recently split into Swift packages under `packages/`, wired to the app by `Telegram-Mac/packages-integration/hooks/*HooksInstall.swift`. Our hooks follow the same pattern. **The whole audio player is one file:** [`packages/AudioPlayerCore/Sources/AudioPlayerCore/AudioPlayerController.swift`](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/AudioPlayerCore/Sources/AudioPlayerCore/AudioPlayerController.swift). It holds 1922 lines: items, `MediaPlaybackControlCenter`, `APController` and the playlist subclasses. Most of its internals are `fileprivate`.

### 3.1 Playlist building
- **Bubble tap:** `ChatAudioContentView.open()` toggles if the message is already in the current controller's list. Otherwise it creates `APChatMusicController(context:chatLocationInput:mode:index:)`, then `showPlayer` + `start()` ([ChatAudioContentView.swift L194–209](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/ChatUI/Sources/ChatUI/ChatAudioContentView.swift#L194-L209)).
- **`APChatController.start()`** ([L1692–1774](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/AudioPlayerCore/Sources/AudioPlayerCore/AudioPlayerController.swift#L1692-L1774)):
  - With no explicit messages, it opens **one 100-message window** around the anchor (`aroundMessageHistoryViewForLocation`, tag `.music`), never paged further.
  - **With `messages:` it uses that exact list.** Shared-media search results already do this ([PeerMediaListController.swift L516–537](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/PeerMediaUI/Sources/PeerMediaUI/PeerMediaListController.swift#L516-L537)).
  - Init with `messages:` and `baseRate:`: [L1777–1797](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/AudioPlayerCore/Sources/AudioPlayerCore/AudioPlayerController.swift#L1777-L1797).
  - Profile music uses `APProfileSavedController`, paged ([L1862–1922](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/AudioPlayerCore/Sources/AudioPlayerCore/AudioPlayerController.swift#L1862-L1922)).
- **Gotchas:**
  - `proccessEntries` reverses the items ([System.swift L39–84](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/TelegramUtils/Sources/TelegramUtils/System.swift#L39-L84)). `next()` does `current -= 1` and wraps around.
  - The diff helper assumes the list is sorted by `MessageIndex`, so replace the items wholesale.
  - The same message twice collides on `chatStableId` ([L423–439](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/AudioPlayerCore/Sources/AudioPlayerCore/AudioPlayerController.swift#L423-L439)).
  - A non-file message crashes `APSongItem.init` on a force-cast ([L148–149](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/AudioPlayerCore/Sources/AudioPlayerCore/AudioPlayerController.swift#L148-L149)).
- **Substitute:** append `APListenTogetherController: APChatMusicController` to the same file, about 130 lines. It adds `replaceQueue`, `jump(to:position:play:)`, prepare-paused-at-position, an `ended` callback with no auto-advance, and no wrap-around.
  - Subclassing keeps the header's playlist button and the music path working.
  - Alternative: widen four `fileprivate` members to `internal` and put the class in a new file in the same package.

### 3.2 Programmatic control
- **API:** `APController` exposes `play()`, `pause()`, `playOrPause()`, `playOrPause(MessageId)` (which is jump-or-toggle), `next()`, `prev()`, `set(seek:)`, `set(trackProgress:)`, `stop()`, `baseRate` and listeners via `APDelegate` ([L1244–1368](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/AudioPlayerCore/Sources/AudioPlayerCore/AudioPlayerController.swift#L1244-L1368); delegate [L536–543](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/AudioPlayerCore/Sources/AudioPlayerCore/AudioPlayerController.swift#L536-L543)).
  - `prev()` never restarts the current track.
- **Engine:** each track gets a fresh `MediaPlayer` (the mac copy in `packages/TelegramMedia`), with `play()` called immediately ([L1415–1494](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/AudioPlayerCore/Sources/AudioPlayerCore/AudioPlayerController.swift#L1415-L1494)).
- **Position: timestamp plus rate.** It's the same `MediaPlayerStatus` model as iOS ([MediaPlayer.swift L672–886](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/TelegramMedia/Sources/MediaPlayer.swift#L672-L886)).
  - `APController` extrapolates on a 0.2 s timer, so listeners fire at about 5 Hz ([L1622–1640](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/AudioPlayerCore/Sources/AudioPlayerCore/AudioPlayerController.swift#L1622-L1640)).
  - The precise source is `CMTimebaseGetTime(controller.timebase)`.
- **Buffering is hidden:** `.buffering` is mapped to "paused" ([L67–89](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/AudioPlayerCore/Sources/AudioPlayerCore/AudioPlayerController.swift#L67-L89)) while the controller still says playing. Add a `playbackStatus` signal, about 6 lines.
- **End of track:** `audioPlayerDidFinishPlaying` → repeat, next or `complete()` ([L1525–1556](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/AudioPlayerCore/Sources/AudioPlayerCore/AudioPlayerController.swift#L1525-L1556)). `complete()` makes the header tear the player down.
- **Seek before ready is dropped.** `set(seek:)` guards on `duration > 0` ([L1590–1607](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/AudioPlayerCore/Sources/AudioPlayerCore/AudioPlayerController.swift#L1590-L1607)).
  - `MediaPlayer.seek(timestamp:play:)` works in any state ([MediaPlayer.swift L1082–1091](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/TelegramMedia/Sources/MediaPlayer.swift#L1082-L1091)), so the subclass uses it for load-paused and seek.
- **Rate:** 0.2–2.5x, persisted in `FastSettings.playingMusicRate`. `InlineAudioPlayerView.update(with:)` re-applies it on every update, so pin it to 1.0 in session mode.

### 3.3 Remote and system commands
- **System choke point:** `MediaPlaybackControlCenter.setupRemoteTransportControls()` registers `MPRemoteCommandCenter` play, pause, toggle, next, previous and changePlaybackPosition ([L777–821](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/AudioPlayerCore/Sources/AudioPlayerCore/AudioPlayerController.swift#L777-L821)). They're wired to `APController` in `init` ([L1067–1099](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/AudioPlayerCore/Sources/AudioPlayerCore/AudioPlayerController.swift#L1067-L1099)).
  - These cover keyboard media keys, AirPods and headset buttons, Control Center and the menu-bar Now Playing, the Touch Bar's system Now Playing strip, and Bluetooth.
  - There's no app-level `NSTouchBar` audio code and no media-key tap at this commit.
- **In-app UI:**
  - Header buttons ([InlineAudioPlayerView.swift L119–171](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/AudioPlayerUI/Sources/AudioPlayerUI/InlineAudioPlayerView.swift#L119-L171)).
  - The playlist modal ([PlayerListController.swift L669–722](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/AudioPlayerUI/Sources/AudioPlayerUI/PlayerListController.swift#L669-L722)).
  - Bubbles.
  - All end in the same `APController` methods.
- **Patch:** add `commandObserver(cmd, source) -> Bool` plus `performApplyingRemote {}` at the top of the 8 public methods, and tag the `MediaPlaybackControlCenter` closures `.remote`. About 45 lines in one file.
- **Tag as `.system`:**
  - Gallery video open and close ([GalleryUIHooksInstall.swift L140–153](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/Telegram-Mac/packages-integration/hooks/GalleryUIHooksInstall.swift#L140-L153)).
  - Voice recording ([ChatController.swift L12693–12701](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/ChatUI/Sources/ChatUI/ChatController.swift#L12693-L12701)).
  - The passcode lock.
- **One global player slot:** `SharedAccountContext.showInlinePlayer` stops the previous controller ([L782–808](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/Telegram-Mac/account/SharedAccountContext.swift#L782-L808)). Any voice note, video message or other song therefore kills the session player, across accounts too. Hook it (about 10 lines) and decide the product behaviour (§6).

### 3.4 Background
- **No suspension:** closing the window keeps the app running ([AppDelegate.swift L1585–1587](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/Telegram-Mac/app/AppDelegate.swift#L1585-L1587)). There's no AVAudioSession, and output goes straight to the default HAL device.
- **App Nap:** no `beginActivity` exists anywhere. The renderer stops 3 s after a pause ([MediaPlayerAudioRenderer.swift L317–342](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/TelegramMedia/Sources/MediaPlayerAudioRenderer.swift#L317-L342)), so a paused, hidden app can be napped. Hold `ProcessInfo.beginActivity(.userInitiatedAllowingIdleSystemSleep)` for the session. This is new code, not a patch.
- **Display sleep pauses downloads (probable blocker):**
  - `SharedWakeupManager` sets `shouldBeServiceTaskMaster = .never` on `screensDidSleep`, and `backgroundAudio` is hard-coded `false` ([SharedWakeupManager.swift L59–118](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/Telegram-Mac/account/SharedWakeupManager.swift#L59-L118), [L172–176](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/Telegram-Mac/account/SharedWakeupManager.swift#L172-L176)).
  - TelegramCore then pauses the worker download sessions ([Network.swift L1307–1312](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramCore/Sources/Network/Network.swift#L1307-L1312), [Download.swift L63–73](https://github.com/TelegramMessenger/Telegram-iOS/blob/24638bc69bd083981b178dd1e7f921eebbf7fc46/submodules/TelegramCore/Sources/Network/Download.swift#L63-L73)).
  - Streamed music should stall once the buffer runs out with the display asleep. Fix: keep worker connections while a session is active, about 8 lines.

### 3.5 Media resolution
- **Shared with iOS (§1.5):** `APSongItem` uses `FileMediaReference.message(…)` ([L287–296](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/AudioPlayerCore/Sources/AudioPlayerCore/AudioPlayerController.swift#L287-L296)), so file references refresh automatically.
- **Streaming:** music streams from the media box with `fetchAutomatically: false`.
- **No next-track prefetch for streamed music.** Copy `ChatMusicContentView.preloadStreamblePart()`, which fetches the first 500 KB ([L183–191](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/ChatUI/Sources/ChatUI/ChatMusicContentView.swift#L183-L191)).
- **Message fetch already used by the app:** `getMessagesLoadIfNecessary` ([InAppLinks.swift L529–537](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/Telegram-Mac/utils/InAppLinks.swift#L529-L537)) and `resolvePeerByName` ([L1333](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/Telegram-Mac/utils/InAppLinks.swift#L1333)).

### 3.6 UI hook points
- **No full-screen player.** The "full player" is the 300×500 `PlayerListController` modal.
  - Its `updateLayout` has an accent-button slot at y = 140 ("Add to Profile"; [L557–667](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/AudioPlayerUI/Sources/AudioPlayerUI/PlayerListController.swift#L557-L667)). **"Listen Together" fits that slot.**
  - It opens from the header's playlist button ([AudioPlayerUIHooksInstall.swift L58–62](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/Telegram-Mac/packages-integration/hooks/AudioPlayerUIHooksInstall.swift#L58-L62)).
- **Mini-player:** `InlineAudioPlayerView`, a 52 pt navigation header.
  - `layout()` places transport on the left, the title, then volume, speed and repeat on the right ([L779–854](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/AudioPlayerUI/Sources/AudioPlayerUI/InlineAudioPlayerView.swift#L779-L854)). Content is set in `update` ([L477–616](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/AudioPlayerUI/Sources/AudioPlayerUI/InlineAudioPlayerView.swift#L477-L616)).
  - Add `MergedAvatarsView` ([ChatUI MergedAvatarsView.swift L41–80](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/ChatUI/Sources/ChatUI/MergedAvatarsView.swift#L41-L80)) and "Listening with …" as the second line, and hide the speed control.
- **Queue:**
  - Rows are `PeerMediaMusicRowItem`, with a context-menu closure ([PlayerListController.swift L1018–1074](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/AudioPlayerUI/Sources/AudioPlayerUI/PlayerListController.swift#L1018-L1074)).
  - **Drag reorder exists** (`TableResortController`), but only for your own profile music ([L1167–1211](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/AudioPlayerUI/Sources/AudioPlayerUI/PlayerListController.swift#L1167-L1211)).
  - On the Mac, remove items with a context-menu item ("Remove from queue"); swipe-to-reveal is only used in the chat list.
  - Add `Source.listenTogether(signal)`, about 110 lines in total including "added by" avatars.
- **Context menu:** `chatMenuItems` ([ChatMessageMenuItems.swift L440–470](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/ChatUI/Sources/ChatUI/ChatMessageMenuItems.swift#L440-L470)).
  - The music items are gated on the download being complete ([L1455–1530](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/ChatUI/Sources/ChatUI/ChatMessageMenuItems.swift#L1455-L1530)). Add ours next to them, without that gate.
  - The Forward submenu ([L1298–1404](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/ChatUI/Sources/ChatUI/ChatMessageMenuItems.swift#L1298-L1404)) is the template for "Share to [chat] & queue".
  - Expose the items via a `ChatUIHooks` closure.
- **"Listening now · Join" bar:** `ChatHeaderController` renders the voice chat as `ChatGroupCallView` (avatars, title, "Join" button; [ChatHeaderController.swift L2003–2140](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/ChatUI/Sources/ChatUI/ChatHeaderController.swift#L2003-L2140)). Its state is computed in `ChatController.updateHeader` ([L1314–1330](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/ChatUI/Sources/ChatUI/ChatController.swift#L1314-L1330)).
  - Add a `listenTogether` state field and a cloned `ChatListenTogetherView`, about 150 lines.
- **Invite card:** `WPLayout.action_text` already returns "Open App" for `.makeWebview` links ([WPLayout.swift L363–460](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/ChatUI/Sources/ChatUI/WPLayout.swift#L363-L460)). Relabel it for our URL. The click routes to `execute(inapp:)`.
  - A custom layout would be picked in `ChatMessageItem` ([L452–502](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/ChatUI/Sources/ChatUI/ChatMessageItem.swift#L452-L502)).

### 3.7 Deep links
- **Parse:** `parseInAppLink` maps `t.me/<bot>/<app>?startapp=` to `.followResolvedName(username: bot, action: .makeWebview(appname:, command:))` ([LinkParsing.swift L624–677](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/TelegramLinks/Sources/TelegramLinks/LinkParsing.swift#L624-L677)). The `tg://resolve` form is handled at [L742–743](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/TelegramLinks/Sources/TelegramLinks/LinkParsing.swift#L742-L743).
- **Converge:** all sources meet in `execute(inapp:)` ([InAppLinks.swift L1268–1372](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/Telegram-Mac/utils/InAppLinks.swift#L1268-L1372)): message links, preview buttons, markdown and external URLs via `AppDelegate.processURL` ([L1449–1482](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/Telegram-Mac/app/AppDelegate.swift#L1449-L1482)).
- **Intercept:** the first lines of the `.followResolvedName` case. Match our bot and app, call `LTHooks…interceptInviteLink`, `afterComplete(true)` and `return`. That's before the resolve modal and the consent alert, about 10 lines.
- **Scheme clash:** the app still registers `tg://`, the same as official Telegram (BUILD-NOTES).

### 3.8 Silent Mini App auth
- **The app's own path:** `executeWebAppLink` ([InAppLinks.swift L688–779](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/Telegram-Mac/utils/InAppLinks.swift#L688-L779)) calls `getBotApp`. If the app is `.notActivated`, it shows a local consent alert ([L748–775](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/Telegram-Mac/utils/InAppLinks.swift#L748-L775)), then calls `requestAppWebView(…)`.
  - **There's no separate activate call**, so silent auth is the same pair of calls without the alert.
- **Main app:** consent is a purely local flag ([BrowserState.swift L446–500](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/WebAppUI/Sources/WebAppUI/BrowserState.swift#L446-L500)).
- Use the same TelegramCore wrappers as iOS (§1.8), with platform `"macos"`.

### 3.9 Sending the invite
- **Text:** `Sender.enqueue(input:context:peerId:…, disablePreview:, mediaPreview:)` ([SenderController.swift L168–300](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/ChatHistoryModel/Sources/ChatHistoryModel/SenderController.swift#L168-L300)) → `enqueueMessages`.
- **Forward:** `Sender.forwardMessages` ([L555–586](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/ChatHistoryModel/Sources/ChatHistoryModel/SenderController.swift#L555-L586)), for "Share to [chat] & queue".
- **Inline bot:** `requestChatContextResults` → `outgoingMessageWithChatContextResult` + `enqueueMessages` ([ChatController.swift L6292–6312](https://github.com/TelegramMessenger/telegram-macos/blob/38309ec95e59be3958d1e6fe9be331d72527a38d/packages/ChatUI/Sources/ChatUI/ChatController.swift#L6292-L6312)).

---

## 4. Upstream files we'd patch

Line counts are estimates of changed or added lines in existing upstream files. Our own modules (the adapter, playlist, header-bar component, join flow, auth, invite sender and core bridge) are new files and aren't counted.

### iOS (about 230–340 lines, about 12 files)
| File | Change | Lines |
|---|---|---|
| `submodules/TelegramUI/Sources/MediaManager.swift` | Interceptor + source in `playlistControl`/`setPlaylist`; `.remote` in 6 objc handlers; bypass `applyControl`; `autoplay` flag; skip `dropAll()` while a session is active | 40–60 |
| `submodules/AccountContext/Sources/MediaManager.swift` | Protocol additions (`LTSource`, interceptor, bypass) so other modules can reach them | 15–25 |
| `submodules/TelegramUI/Sources/SharedMediaPlayer.swift` | Optional: honour the scheduled start time on `.pause` | 0–5 |
| `submodules/TelegramUI/Sources/SharedAccountContext.swift`, `…/Chat/ChatControllerMediaRecording.swift` | Pass `.system` | 2–4 |
| `submodules/TelegramUI/Sources/OpenResolvedUrl.swift` (+ optional `submodules/UrlHandling/Sources/UrlHandling.swift`) | Invite-link intercept | 5–14 |
| `submodules/TelegramUI/Sources/ChatInterfaceStateContextMenus.swift` | Two menu items | 25–40 |
| `submodules/TelegramUI/Sources/ChatControllerNode.swift`, `…/Chat/ChatControllerLoadDisplayNode.swift` | "Listening now" panel + relayout subscription | 20–30 |
| `submodules/TelegramUI/Components/Chat/ChatMessageWebpageBubbleContentNode/…/ChatMessageWebpageBubbleContentNode.swift` | Invite-card relabel | 8 |
| `submodules/TelegramUI/Sources/OverlayAudioPlayerControlsNode.swift` | Listen Together button; order/looping routing | 25–40 |
| `submodules/TelegramUI/Sources/OverlayAudioPlayerControllerNode.swift` | Reorder for our location; "Remove from queue"; header title | 30–50 |
| `submodules/TelegramUI/Components/MediaPlaybackHeaderPanelComponent/…/MediaNavigationAccessoryHeaderNode.swift` | Avatar stack + "Listening with" | 30–40 |
| `BUILD` files (`submodules/TelegramUI/BUILD` …) | Dependency on our module | 5 |

### Android (about 300–480 lines, 10 files)
| File | Change | Lines |
|---|---|---|
| `org/telegram/messenger/MediaController.java` | `setCustomPlaylist` + `customOrder`; `LT.intercept` at public methods; `ended` hook; `Player.Listener`; `startPaused`; no `stopService` in a session; position getter; tag internal callers | 80–120 |
| `org/telegram/messenger/MusicPlayerReceiver.java` | Source tag | 4–6 |
| `org/telegram/messenger/MusicPlayerService.java` | Source tag; stay foreground on API ≤ 30 in a session; no `stopSelf` | 10–15 |
| `org/telegram/messenger/TelegramMediaSession.java` | Source tag; route `onPlayFromMediaId`/`onSkipToQueueItem` in a session | 6–10 |
| `org/telegram/ui/LaunchActivity.java` | Link intercept in `runLinkRequest` | 3–5 |
| `org/telegram/ui/Components/AudioPlayerAlert.java` | Options-menu item; order predicate; reorder + swipe for the session queue; "Listening with" row | 40–70 |
| `org/telegram/ui/Cells/AudioPlayerCell.java` | "Added by" mini avatar | 30–40 |
| `org/telegram/ui/Components/FragmentContextView.java` | Avatars in the audio style; new `STYLE_LISTEN_TOGETHER` bar | 100–160 |
| `org/telegram/ui/ChatActivity.java` | Two options in `fillMessageMenu` + `processSelectedOption` | 25–35 |
| `org/telegram/ui/Cells/ChatMessageCell.java` | Invite-card relabel | 8–12 |

### macOS (about 450–700 lines, about 12 files; more than half append-only or new views)
| File | Change | Lines |
|---|---|---|
| `packages/AudioPlayerCore/Sources/AudioPlayerCore/AudioPlayerController.swift` | `commandObserver` + `performApplyingRemote` in 8 methods; `.remote` tags; `seek(to:play:)`; `playbackStatus`; **append** `APListenTogetherController` (about 130) | 150–190 |
| `packages/AudioPlayerUI/Sources/AudioPlayerUI/InlineAudioPlayerView.swift` | Avatar stack, "Listening with", hide speed, button | 50–70 |
| `packages/AudioPlayerUI/Sources/AudioPlayerUI/PlayerListController.swift` | `Source.listenTogether`, reorder, remove item, button slot, "added by" | 80–110 |
| `packages/AudioPlayerUI/Sources/AudioPlayerUI/AudioPlayerUIHooks.swift` + `Telegram-Mac/packages-integration/hooks/AudioPlayerUIHooksInstall.swift` | Hook fields | 20–25 |
| `packages/ChatUI/Sources/ChatUI/ChatMessageMenuItems.swift` + `ChatUIHooks` / `ChatUIHooksInstall.swift` | Two menu items via hook | 40–55 |
| `packages/ChatUI/Sources/ChatUI/ChatHeaderController.swift` + `ChatController.swift` (`updateHeader`) | `ChatListenTogetherView` (can be a new file in ChatUI) + state + slot | 60–150 |
| `Telegram-Mac/utils/InAppLinks.swift` | Link intercept | 10–12 |
| `packages/ChatUI/Sources/ChatUI/WPLayout.swift` | Invite-card relabel | 10–20 |
| `Telegram-Mac/account/SharedAccountContext.swift` | Notify on player replace/end | 10 |
| `Telegram-Mac/account/SharedWakeupManager.swift` | Keep worker connections during a session | 8 |
| `GalleryUIHooksInstall.swift`, `ChatController.swift` (recording), `SharedNotificationManager.swift` | `.system` tags | 3–6 |
| `Telegram-Mac/app/AppDelegate.swift` (or a new `*HooksInstall.swift`) | Install our hooks | 5 |

Upstream drops are large and periodic (PLAN §4.6). The files most likely to conflict on merge are Android's `ChatActivity`/`ChatMessageCell` (48k and 24k lines) and macOS's `AudioPlayerController.swift`. Keep patches in those to one-line hook calls.

---

## 5. Suggested changes to PLAN / PROTOCOL

- **Entry identity:** add an optional `chat_username`, or another resolvable handle, to queue entries for public channels (Key finding 9). Without it, a non-member device can't fetch the message.
- **Join path:** the core's LOAD → ready → PLAY(pos, ref) → started → correct sequence (PROTOCOL "Join / track change") maps cleanly onto all three players once the small prepare-paused patches are in. No plan change.
- **Previous rule:** fine as specified (3 s, decided by the core). Upstream thresholds (5 s, 10 s, none) are bypassed because every "previous" is intercepted.
- **Silent auth:** prefer `getBotApp` + `requestAppWebView(allowWrite: false)` for the same named app as the invite link. This doesn't need a BotFather "Main Mini App". `requestMainWebView` is the fallback.
- **macOS:** add "keep download connections during a session" and the `beginActivity` holder to the Mac rows of PLAN §2.4. Decide what happens when another audio replaces the global player during a session.
- **Android:** PLAN §2.4 ("service stays in the foreground for the whole session") needs the API ≤ 30 patch and a "never `stopService` in a session" rule. It isn't upstream behaviour on its own.

## 6. Risks and unknowns to verify once builds work

**Cross-platform**
- **Server side of silent auth:** does `requestAppWebView`/`requestMainWebView` without UI add the bot to "recent apps" or search "Apps", notify the bot, or fail for a not-yet-activated (`inactive`) app? Is the initData fragment present and valid? (Spike 2d.)
- **Preview type:** does the server label our `t.me/<bot>/<app>?startapp=` previews `telegram_botapp`? The card relabel depends on it, and no card appears if the sender disables previews.
- **Start latency and seek cost:** measure the real delay from `play` to audible on each player, and output latency on Bluetooth. Measure seek cost when streaming (iOS and macOS rebuild the decoder; Android re-buffers) and whether correction seeks cause audible gaps.
- **Interruption classification:** phone call, Siri, focus loss and unplugged headphones appear only as status changes on iOS and macOS. Confirm the "unexpected pause → interrupted" heuristic doesn't misfire on buffering stalls.
- **Message access:** non-member public channels (resolve → getMessages → file-reference refresh after an app restart), protected and `noforwards` chats (playback allowed), deleted messages mid-queue, and restricted content.
- **"Share to [chat] & queue":** a forwarded copy gets a new (chat, msg id). Confirm it plays for every member.

**iOS**
- Does a silent loop on the active `.playback` session keep the app alive for 60 minutes paused in the background, without `ManagedAudioSession` holder churn deactivating it? Does remote play then resume?
- `setPlaylist(autoplay:false)` + `.seek`: is there no audible blip, and does the first `.playing` status come before audio actually starts?
- `getMessagesLoadIfNecessary`: test the unknown-peer skip and the `minAvailableMessageId` quirk in a supergroup with hidden pre-history.
- Fast-forward long-press (x4–x16 rate) and repeated backward seeks need mapping or debouncing in a session.

**Android**
- What does Android Auto show and control, given `TelegramMediaSession` never publishes state? Which session receives headset and Bluetooth keys while both are active?
- Background restart on Android 12, 14, 15 and 16 plus OEM killers (MIUI, Samsung) with the service kept alive. API ≤ 30 with the foreground patch.
- Does the `restoreMusicPlaylistState` voice-message path, or another video, pause or replace the session queue? Decide the policy.

**macOS**
- **Display-sleep stall** (confirm, then apply the 8-line fix) and App Nap with `beginActivity`.
- `MediaPlayer.seek(timestamp:play:false)` from `.empty`: does it leave a primed, paused renderer, and does `play()` then start within a few ms?
- Single global player slot across accounts: does a voice note, video message or profile music stop the session?
- `fileprivate` internals: an append-only class versus widening four members. Pick whichever merges more cleanly with upstream.

---

_How this was studied: shallow sparse clones of the three upstream repos at the pinned commits, read file by file, with key line references spot-checked a second time. No fork or upstream repo was modified._
