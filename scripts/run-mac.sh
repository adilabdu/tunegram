#!/usr/bin/env bash
#
# Download the latest successful Tunegram macOS CI build, decrypt it, and run it.
#
# Usage:
#   scripts/run-mac.sh            # latest successful run of tunegram-build.yml on branch tunegram
#   scripts/run-mac.sh <run-id>   # a specific run
#
# Needs: gh (logged in), .secrets/artifact.key (first line is the key).
# The app lands in .artifacts/macos/Tunegram.app; the download is deleted afterwards.
#
set -euo pipefail

REPO="adilabdu/tunegram-macos"
WORKFLOW="tunegram-build.yml"
BRANCH="tunegram"
ARTIFACT="tunegram-macos-app"
APP_NAME="Tunegram.app"
BUNDLE_ID="app.tunegram.mac"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KEY_FILE="$ROOT/.secrets/artifact.key"
OUT_DIR="$ROOT/.artifacts/macos"
APP="$OUT_DIR/$APP_NAME"

log() { printf '==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

command -v gh >/dev/null || die "gh is not installed"
[ -s "$KEY_FILE" ] || die "missing $KEY_FILE"

RUN_ID="${1:-}"
if [ -z "$RUN_ID" ]; then
  RUN_ID="$(gh run list -R "$REPO" --workflow "$WORKFLOW" --branch "$BRANCH" --status success \
    --limit 1 --json databaseId --jq '.[0].databaseId // empty')"
  [ -n "$RUN_ID" ] || die "no successful $WORKFLOW run on $BRANCH in $REPO"
fi
log "Run $RUN_ID: https://github.com/$REPO/actions/runs/$RUN_ID"

mkdir -p "$OUT_DIR"
FREE_KB="$(df -k "$OUT_DIR" | awk 'NR==2 {print $4}')"
[ "$FREE_KB" -gt $((3 * 1024 * 1024)) ] || die "less than 3 GB free on the disk holding $OUT_DIR"

WORK="$(mktemp -d "$OUT_DIR/.download.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

log "Downloading artifact $ARTIFACT"
gh run download "$RUN_ID" -R "$REPO" -n "$ARTIFACT" -D "$WORK"
[ -f "$WORK/app.zip.enc" ] || die "artifact did not contain app.zip.enc"

log "Decrypting"
/usr/bin/openssl enc -d -aes-256-cbc -pbkdf2 -iter 200000 -md sha256 \
  -pass "file:$KEY_FILE" -in "$WORK/app.zip.enc" -out "$WORK/app.zip" \
  || die "decryption failed (wrong key?)"
rm -f "$WORK/app.zip.enc"

if pgrep -f "$APP/Contents/MacOS/" >/dev/null 2>&1; then
  log "Quitting the running $APP_NAME"
  osascript -e "tell application id \"$BUNDLE_ID\" to quit" >/dev/null 2>&1 || true
  for _ in $(seq 1 20); do
    pgrep -f "$APP/Contents/MacOS/" >/dev/null 2>&1 || break
    sleep 0.5
  done
  pkill -f "$APP/Contents/MacOS/" 2>/dev/null || true
fi

log "Unpacking into $OUT_DIR"
rm -rf "$APP" "$WORK/unpacked"
mkdir -p "$WORK/unpacked"
ditto -x -k "$WORK/app.zip" "$WORK/unpacked"
rm -f "$WORK/app.zip"
[ -d "$WORK/unpacked/$APP_NAME" ] || die "zip did not contain $APP_NAME"
mv "$WORK/unpacked/$APP_NAME" "$APP"
printf '%s\n' "$RUN_ID" > "$OUT_DIR/run-id"

xattr -dr com.apple.quarantine "$APP" 2>/dev/null || true
if codesign --verify --deep --strict "$APP" 2>/dev/null; then
  log "Signature OK"
else
  log "Re-signing ad hoc"
  codesign --force --deep --preserve-metadata=entitlements,flags -s - "$APP"
  codesign --verify --deep --strict "$APP"
fi

log "Opening $APP"
open "$APP"
