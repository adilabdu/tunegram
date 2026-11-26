#!/usr/bin/env bash
# Mount (or eject) "SARAH Mac": a general-purpose Mac-format (APFS) disk image stored on the
# external FAT32 drive "SARAH". FAT32 has no symlinks/permissions and a 4 GB file limit, so builds
# (and any large files) live inside this sparse image instead. It grows on demand up to 750 GB.
#
# The volume is named "SARAH Mac" (that's what Finder shows), but it's mounted at a path without
# spaces (~/.sarah-mac) because several build tools (Bazel, autotools, ndk) break on spaces.
#
# Usage: scripts/sarah-mac.sh          # mount
#        scripts/sarah-mac.sh eject    # eject the image (do this before unplugging SARAH)
#        scripts/sarah-mac.sh path     # print the mount path
set -euo pipefail

IMG="/Volumes/SARAH/Claude/SARAH Mac.sparsebundle"
MP="$HOME/.sarah-mac"

is_mounted() { mount | grep -q " on $MP (apfs"; }

case "${1:-mount}" in
  path) echo "$MP" ;;
  eject)
    if is_mounted; then hdiutil detach "$MP" >/dev/null && echo "Ejected SARAH Mac. You can now eject SARAH."; else echo "SARAH Mac is not mounted."; fi ;;
  mount)
    if [[ ! -d /Volumes/SARAH ]]; then echo "The SARAH drive isn't connected. Plug it in and try again." >&2; exit 1; fi
    if is_mounted; then
      echo "SARAH Mac is already mounted at $MP."
    else
      mkdir -p "$MP"
      hdiutil attach -mountpoint "$MP" "$IMG" >/dev/null
      echo "Mounted SARAH Mac at $MP."
    fi
    touch "$MP/.metadata_never_index"   # keep Spotlight from indexing build output
    df -h "$MP" | tail -1 ;;
  *) echo "usage: $0 [mount|eject|path]" >&2; exit 2 ;;
esac
