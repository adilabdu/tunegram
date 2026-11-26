#!/usr/bin/env bash
# Download the latest successful Tunegram Android CI build, decrypt it, and run it
# in the Android Emulator on this Mac.
#
# Usage: scripts/run-android.sh [RUN_ID]
#   RUN_ID   optional GitHub Actions run id (default: latest successful run on `tunegram`)
# Env:
#   AVD      emulator to start when none is running (default: Pixel_6_Pro_API_34)
#   ANDROID_HOME  Android SDK (default: ~/Library/Android/sdk)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="adilabdu/tunegram-android"
WORKFLOW="tunegram-build.yml"
BRANCH="tunegram"
ARTIFACT="tunegram-android-apk"
PKG="app.tunegram.android"
AVD="${AVD:-Pixel_6_Pro_API_34}"
SDK="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
ADB="$SDK/platform-tools/adb"
EMULATOR="$SDK/emulator/emulator"
KEY="$ROOT/.secrets/artifact.key"
OUT="$ROOT/.artifacts/android"
APK="$OUT/tunegram.apk"
DL="$OUT/download"

log() { printf '==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

for tool in gh openssl unzip; do
  command -v "$tool" > /dev/null || die "$tool not found"
done
[ -x "$ADB" ] || die "adb not found at $ADB"
[ -x "$EMULATOR" ] || die "emulator not found at $EMULATOR"
[ -s "$KEY" ] || die "artifact key missing: $KEY"

mkdir -p "$OUT"
# The encrypted download and the decrypted zip never outlive this script.
cleanup() { rm -rf "$DL"; }
trap cleanup EXIT

# 1. Find the run.
RUN_ID="${1:-}"
if [ -z "$RUN_ID" ]; then
  RUN_ID="$(gh run list -R "$REPO" -w "$WORKFLOW" -b "$BRANCH" -s success -L 1 \
    --json databaseId -q '.[0].databaseId')"
  [ -n "$RUN_ID" ] || die "no successful $WORKFLOW run on $BRANCH"
fi

# 2. Download + decrypt + unzip (skipped when this run's APK is already here).
if [ -s "$APK" ] && [ "$(cat "$OUT/run-id" 2> /dev/null)" = "$RUN_ID" ]; then
  log "APK from run $RUN_ID already downloaded"
else
  log "Downloading $ARTIFACT from run $RUN_ID"
  rm -rf "$DL"
  mkdir -p "$DL"
  gh run download "$RUN_ID" -R "$REPO" -n "$ARTIFACT" -D "$DL"
  [ -s "$DL/apk.zip.enc" ] || die "apk.zip.enc not in artifact"
  log "Decrypting"
  openssl enc -d -aes-256-cbc -pbkdf2 -iter 200000 -md sha256 \
    -pass "file:$KEY" -in "$DL/apk.zip.enc" -out "$DL/apk.zip"
  rm -f "$APK"
  unzip -q -o "$DL/apk.zip" -d "$OUT"
  [ -s "$APK" ] || die "tunegram.apk not in zip"
  printf '%s\n' "$RUN_ID" > "$OUT/run-id"
  rm -rf "$DL"
fi
log "APK: $APK ($(du -h "$APK" | cut -f1))"

# 3. Emulator: reuse a running one, otherwise start $AVD with a window.
running_emulator() {
  "$ADB" devices | awk '$1 ~ /^emulator-[0-9]+$/ && $2 == "device" { print $1; exit }'
}
SERIAL="$(running_emulator)"
if [ -z "$SERIAL" ]; then
  log "Starting emulator $AVD"
  "$EMULATOR" -list-avds | grep -qx "$AVD" || die "AVD $AVD not found"
  nohup "$EMULATOR" -avd "$AVD" -netdelay none -netspeed full \
    > "$OUT/emulator.log" 2>&1 &
  for _ in $(seq 1 120); do
    SERIAL="$(running_emulator)"
    [ -n "$SERIAL" ] && break
    sleep 2
  done
  [ -n "$SERIAL" ] || die "emulator did not come up (see $OUT/emulator.log)"
fi
log "Device: $SERIAL"
adb_s() { "$ADB" -s "$SERIAL" "$@"; }

log "Waiting for boot to complete"
adb_s wait-for-device
for _ in $(seq 1 150); do
  [ "$(adb_s shell getprop sys.boot_completed 2> /dev/null | tr -d '\r')" = "1" ] && break
  sleep 2
done
[ "$(adb_s shell getprop sys.boot_completed | tr -d '\r')" = "1" ] || die "boot did not complete"

# 4. Install + launch.
log "Installing"
if ! adb_s install -r "$APK"; then
  die "install failed. On INSTALL_FAILED_UPDATE_INCOMPATIBLE (signing key changed): $ADB -s $SERIAL uninstall $PKG (deletes the app's data), then re-run"
fi

log "Launching $PKG"
COMPONENT="$(adb_s shell cmd package resolve-activity --brief \
  -a android.intent.action.MAIN -c android.intent.category.LAUNCHER "$PKG" | tr -d '\r' | tail -n 1)"
if [[ "$COMPONENT" == */* ]]; then
  adb_s shell am start -n "$COMPONENT" > /dev/null
else
  adb_s shell monkey -p "$PKG" -c android.intent.category.LAUNCHER 1 > /dev/null
fi
log "Done (run $RUN_ID)"
