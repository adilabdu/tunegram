# Build notes (Phase 0)

What the first build setup found, written 2026-11-26. Cloud builds (GitHub Actions) are set up in all three forks. But the `adilabdu` account currently has a billing lock, so GitHub won't start any job ("account is locked due to a billing issue"). Builds therefore move to this Mac, with the build files on an external drive. Free ways to remove the lock: keep the account on GitHub Free, or ask GitHub Support.

Runs that are stuck in the queue get cancelled automatically after about 24 h.

## Forks (default branch `tunegram`)
| Fork | Base | Our changes so far |
|---|---|---|
| `adilabdu/tunegram-ios` | upstream master 24638bc (13.2) | `.github/workflows/tunegram-build.yml` only |
| `adilabdu/tunegram-android` | upstream master 21f7d10 (13.2.2) | `APP_PACKAGE=app.tunegram.android`; `CHECK_UPDATES`, `SUPPORTS_PASSKEYS` and `SUPPORTS_FPNV` set to false; ABIs limited to arm64-v8a + x86_64 in the app and library modules; workflow |
| `adilabdu/tunegram-macos` | upstream master 38309ec | bundle ids `app.tunegram.mac[.TelegramShare/.WidgetEngine]` in pbxproj, entitlements and `ApiCredentials/Config.swift`; `-D'GITHUB'` in Debug/Release xcconfig (turns off Sparkle updates); `.github/tunegram/inject_api_credentials.py`; workflow |

Secrets are never committed. CI injects `TG_API_ID`/`TG_API_HASH` at build time, and local builds read them from `.secrets/`. Build outputs are encrypted with `.secrets/artifact.key` (`openssl enc -aes-256-cbc -pbkdf2 -iter 200000 -md sha256`).

## iOS (Telegram-iOS, Bazel)
- **Command:** `python3 -u build-system/Make/Make.py [--cacheDir=…] --overrideXcodeVersion build --configurationPath=<json> --xcodeManagedCodesigning --buildNumber=<n> --configuration=debug_sim_arm64`. The `--cacheDir` and `--overrideXcodeVersion` flags go before `build`.
- **Extra Bazel flags go in `~/.bazelrc`.** `--bazelArguments` is ignored. Use `build --//Telegram:disableProvisioningProfiles` and `--jobs=N`.
- **Fake codesigning doesn't fit our bundle id.** Its profiles are for `C67CF9S4VU.ph.telegra.Telegraph`. Use `--xcodeManagedCodesigning` plus the disable-profiles flag instead. For simulator builds `team_id` is only a label (`C67CF9S4VU`); replace it with the free Apple ID's personal team for device builds.
- **Config JSON:**
  - bundle_id `app.tunegram.ios`, app_specific_url_scheme `tunegram`, `premium_iap_product_id ""`, siri and icloud false.
  - Add `"app_center_id": "0"`: release-13.0.0 requires it, and master ignores it.
- **Submodules:** relative URLs (`../tgcalls.git`, `../rlottie.git`) resolve against the fork. Repoint them to `https://github.com/TelegramMessenger/` before `git submodule update --init --recursive --depth 1`.
- **Xcode version:**
  - Master pins Xcode 27.1 (Swift 6.4). This Mac has Xcode 27.0, so it needs `--overrideXcodeVersion`.
  - `release-13.0.0` is the last tag pinned to Xcode 26.6.
- **Minimum iOS** is 15.0, so the local iOS 18.0 simulator runtime works.
- **Output:** `bazel-bin/Telegram/Telegram.ipa` (or `bazel-out/applebin_ios-*-ST-*/bin/Telegram/Telegram.ipa`); the app is `Payload/Telegram.app`.
- Upstream's own `build.yml` runs on `macos-13` (retired), so syncing the fork's `master` will start failing runs. Disable that workflow in the fork.

## Android (Gradle)
- JDK 17 (AGP 8.13.2 / Gradle 8.13), NDK 27.2.12479018, SDK 36, build-tools 36.0.0, cmake 3.22.1.
- **Task:** `:TMessagesProj_App:assembleAfatRelease`. This is the only variant without a package suffix, and Firebase only knows `app.tunegram.android`.
- **Generated at build time:**
  - `TMessagesProj_App/google-services.json`, `TMessagesProj/config/release.keystore`, the `RELEASE_*` values in `gradle.properties`;
  - APP_ID/APP_HASH in `BuildVars.java`.
- Native code compiles in the `TMessagesProj` library module, which is why its ABI filter matters for build time.

## macOS (telegram-macos, Xcode workspace)
- **Requirements:**
  - Xcode 26 is what upstream uses; Xcode 27 needs the `XCODE27.md` / `scripts/xcode27.sh apply` patches.
  - Homebrew packages: cmake ninja meson nasm yasm openssl@1.1 zlib autoconf libtool automake pkg-config.
  - rustup, plus Rosetta 2.
- **Submodules:**
  - Use https rewrites for `git@github.com:` and `git@gitlab.com:`, then `submodule update --init --recursive`.
  - Apply the abseil patch from `scripts/setup_build_env.sh`.
- **Build:** `scripts/configure_frameworks.sh` builds the native libraries once (slow, mostly WebRTC). Then build scheme `Telegram` in `Telegram-Mac.xcworkspace`, ad-hoc signed.
- **API credentials** live in `packages/ApiCredentials/Sources/ApiCredentials/Config.swift`.
- **Still to do:**
  - The app still shows "Telegram" as its name and registers `tg://`, which clashes with the official app.
  - Team ID `6N38VWS5BX` (`Config.swift`, `submodules/BuildConfig/Sources/BuildConfig.m`) may cause keychain or app-group prompts on an ad-hoc build.
  - The Telegram-iOS submodule still points upstream.

## Run scripts (`scripts/`)
- `run-ios-sim.sh [variant] [--local]`: downloads and decrypts the newest `tunegram-ios-sim-*` artifact, or with `--local` reuses `.artifacts/ios/Telegram.app`. Installs into an iOS 18.0 simulator (default "iPhone 16 Pro") and launches it.
- `run-android.sh [run-id]`: downloads and decrypts the APK, starts `Pixel_6_Pro_API_34` if needed, installs and launches it.
- `run-mac.sh [run-id]`: downloads and decrypts `Tunegram.app` into `.artifacts/macos/`, clears quarantine, re-signs ad hoc if needed and opens it.

For local builds, the scripts still need a "use a locally built app" path where it's missing (Android and macOS). None of the three has launched a real build yet.

## Local build volume: "SARAH Mac" (external drive)
- The `SARAH` drive (WD My Passport 2 TB, a spinning disk, on a USB 2.0 link) is formatted FAT32: no symlinks, no Unix permissions, and a 4 GB file limit.
- Builds and large files therefore live in a sparse APFS disk image at `/Volumes/SARAH/Claude/SARAH Mac.sparsebundle`. It uses 64 MB bands and grows on demand up to 750 GB.
- The volume is named **SARAH Mac** and mounts at the standard `/Volumes/SARAH Mac` (changed 2026-11-26). It holds storage only, so the space in the path doesn't matter, and the shorter path keeps the 158-level-deep Downloads archive under macOS's 1023-byte path limit.
- Mount it with `scripts/sarah-mac.sh`. Before unplugging SARAH, run `scripts/sarah-mac.sh eject` (or eject SARAH Mac in Finder), then eject SARAH.
- **Policy (2026-11-26):** all active work happens on the internal disk: source checkouts, Bazel output root, Xcode DerivedData, `GRADLE_USER_HOME` and agent scratch, under `~/Documents/projects/tunegram-work` or the repo. The owner freed space for this (76 GB free). SARAH Mac holds storage and archives only; it's too slow for builds.
- Measured 2026-11-26 over USB 2.0:
  - **Raw FAT32:** about 36 MB/s write, 38 MB/s read, and only about 100 small files/s written.
  - **Inside the image:** about 4,100 small files/s written and about 22 MB/s uncached read.
