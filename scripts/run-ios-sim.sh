#!/usr/bin/env bash
# Download the newest Tunegram iOS Simulator build from GitHub Actions, decrypt it and run it
# in the iOS Simulator on this Mac.
#
# Usage:
#   scripts/run-ios-sim.sh [--local] [variant]
#
#   variant   artifact suffix, e.g. "master" or "13.0.0" (artifact tunegram-ios-sim-<variant>).
#             Default: the newest tunegram-ios-sim-* artifact of any variant.
#   --local   skip the download and reinstall the app already in .artifacts/ios/.
#
# Environment overrides:
#   TUNEGRAM_IOS_REPO  GitHub repo with the build workflow (default adilabdu/tunegram-ios)
#   RUN_ID             take the artifact from this workflow run instead of the newest one
#   SIM_DEVICE         simulator name to boot, created if missing (default "iPhone 16 Pro")
#   SIM_DEVICE_TYPE    device type used when creating it (default iPhone 16 Pro)
#   SIM_RUNTIME        runtime identifier (default com.apple.CoreSimulator.SimRuntime.iOS-18-0)
#
# The artifact is AES-256 encrypted in CI (public-repo artifacts are downloadable by anyone and
# the app embeds our API credentials); the key is the first line of .secrets/artifact.key.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO="${TUNEGRAM_IOS_REPO:-adilabdu/tunegram-ios}"
KEY_FILE="$ROOT/.secrets/artifact.key"
OUT_DIR="$ROOT/.artifacts/ios"
APP="$OUT_DIR/Telegram.app"
DEVICE_NAME="${SIM_DEVICE:-iPhone 16 Pro}"
DEVICE_TYPE="${SIM_DEVICE_TYPE:-com.apple.CoreSimulator.SimDeviceType.iPhone-16-Pro}"
RUNTIME="${SIM_RUNTIME:-com.apple.CoreSimulator.SimRuntime.iOS-18-0}"

LOCAL=0
VARIANT=""
for arg in "$@"; do
  case "$arg" in
    --local) LOCAL=1 ;;
    -h|--help) sed -n '2,21p' "$0"; exit 0 ;;
    -*) echo "Unknown option: $arg" >&2; exit 2 ;;
    *) VARIANT="$arg" ;;
  esac
done

log() { printf '==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

mkdir -p "$OUT_DIR"
WORK="$OUT_DIR/.download"
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

if [ "$LOCAL" -eq 0 ]; then
  [ -s "$KEY_FILE" ] || die "missing $KEY_FILE"
  gh auth status >/dev/null 2>&1 || die "gh is not logged in (run: gh auth login)"

  # Pick the artifact: newest non-expired tunegram-ios-sim-<variant> (any variant by default).
  # Artifacts are looked up directly so that a run where only one matrix variant succeeded counts.
  prefix="tunegram-ios-sim-"
  query="[.artifacts[] | select(.expired | not)"
  if [ -n "$VARIANT" ]; then
    query+=" | select(.name == \"$prefix$VARIANT\")"
  else
    query+=" | select(.name | startswith(\"$prefix\"))"
  fi
  if [ -n "${RUN_ID:-}" ]; then
    query+=" | select(.workflow_run.id == $RUN_ID)"
  fi
  query+="] | sort_by(.created_at) | last | select(. != null) | [.name, (.workflow_run.id | tostring), (.size_in_bytes | tostring), .created_at] | @tsv"
  read -r NAME RUN SIZE CREATED < <(gh api "repos/$REPO/actions/artifacts?per_page=100" --jq "$query") \
    || die "no ${prefix}${VARIANT:-*} artifact found in $REPO (expired after 7 days? re-run the workflow)"
  log "Artifact $NAME from run $RUN ($((SIZE / 1048576)) MB, $CREATED)"
  log "Run: https://github.com/$REPO/actions/runs/$RUN"

  rm -rf "$WORK"; mkdir -p "$WORK"
  gh run download "$RUN" -R "$REPO" -n "$NAME" -D "$WORK"
  [ -f "$WORK/app.zip.enc" ] || die "app.zip.enc not found in the artifact"

  log "Decrypting"
  openssl enc -d -aes-256-cbc -pbkdf2 -iter 200000 -md sha256 \
    -pass "file:$KEY_FILE" -in "$WORK/app.zip.enc" -out "$WORK/app.zip" \
    || die "decryption failed (wrong key in $KEY_FILE?)"
  rm -f "$WORK/app.zip.enc"

  log "Unpacking into $OUT_DIR"
  rm -rf "$APP"
  ditto -x -k "$WORK/app.zip" "$OUT_DIR"
  rm -f "$WORK/app.zip"
fi

[ -d "$APP" ] || die "no app at $APP (run without --local first)"
BUNDLE_ID="$(plutil -extract CFBundleIdentifier raw "$APP/Info.plist")"
MIN_OS="$(plutil -extract MinimumOSVersion raw "$APP/Info.plist")"
VERSION="$(plutil -extract CFBundleShortVersionString raw "$APP/Info.plist") ($(plutil -extract CFBundleVersion raw "$APP/Info.plist"))"
log "App $BUNDLE_ID $VERSION, minimum iOS $MIN_OS"

# Runtime check: the app's minimum iOS must not exceed the installed runtime.
RUNTIME_VERSION="$(xcrun simctl list runtimes -j | python3 -c '
import json, sys
rid = sys.argv[1]
for r in json.load(sys.stdin)["runtimes"]:
    if r["identifier"] == rid and r.get("isAvailable"):
        print(r["version"]); break
' "$RUNTIME")"
[ -n "$RUNTIME_VERSION" ] || die "simulator runtime $RUNTIME is not installed or unavailable"
if [ "$(printf '%s\n%s\n' "$MIN_OS" "$RUNTIME_VERSION" | sort -V | head -1)" != "$MIN_OS" ]; then
  die "the app needs iOS $MIN_OS but the installed runtime is iOS $RUNTIME_VERSION"
fi

# Find (or create) the simulator on that runtime.
UDID="$(xcrun simctl list devices -j | python3 -c '
import json, sys
rid, name = sys.argv[1], sys.argv[2]
for d in json.load(sys.stdin)["devices"].get(rid, []):
    if d["name"] == name and d.get("isAvailable"):
        print(d["udid"]); break
' "$RUNTIME" "$DEVICE_NAME")"
if [ -z "$UDID" ]; then
  log "Creating simulator \"$DEVICE_NAME\""
  UDID="$(xcrun simctl create "$DEVICE_NAME" "$DEVICE_TYPE" "$RUNTIME")"
fi
STATE="$(xcrun simctl list devices -j | python3 -c '
import json, sys
for ds in json.load(sys.stdin)["devices"].values():
    for d in ds:
        if d["udid"] == sys.argv[1]: print(d["state"])
' "$UDID")"
if [ "$STATE" != "Booted" ]; then
  log "Booting \"$DEVICE_NAME\" ($UDID)"
  xcrun simctl boot "$UDID"
fi
xcrun simctl bootstatus "$UDID" -b >/dev/null
SIM_APP="$(mdfind "kMDItemCFBundleIdentifier == 'com.apple.iphonesimulator'" 2>/dev/null | head -1)"
[ -z "$SIM_APP" ] && SIM_APP="$(xcode-select -p)/Applications/Simulator.app"
open "$SIM_APP" --args -CurrentDeviceUDID "$UDID" 2>/dev/null || echo "==> Simulator window app not found; the simulator runs headless (use simctl / the Claude Simulator panel)"

log "Installing and launching"
xcrun simctl install "$UDID" "$APP"
xcrun simctl launch "$UDID" "$BUNDLE_ID"
log "Running on \"$DEVICE_NAME\" ($UDID). Screenshot: xcrun simctl io $UDID screenshot shot.png"
