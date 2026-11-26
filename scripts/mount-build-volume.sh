#!/usr/bin/env bash
# Mount (or eject) the TunegramBuild APFS disk image that lives on the external "SARAH" drive.
# SARAH is FAT32 (no symlinks/permissions, 4 GB file limit), so all builds happen inside this
# sparse APFS image instead. It grows on demand up to 600 GB.
#
# Usage: scripts/mount-build-volume.sh          # mount
#        scripts/mount-build-volume.sh eject    # eject the image (do this before unplugging SARAH)
set -euo pipefail

IMG="/Volumes/SARAH/Claude/TunegramBuild.sparsebundle"
VOL="/Volumes/TunegramBuild"

if [[ "${1:-}" == "eject" ]]; then
  if [[ -d "$VOL" ]]; then hdiutil detach "$VOL" && echo "Ejected $VOL. You can now eject SARAH."; else echo "$VOL is not mounted."; fi
  exit 0
fi

if [[ ! -d /Volumes/SARAH ]]; then
  echo "The SARAH drive isn't connected. Plug it in and try again." >&2
  exit 1
fi
if [[ -d "$VOL" ]]; then
  echo "$VOL is already mounted."
else
  hdiutil attach "$IMG" >/dev/null
  echo "Mounted $VOL."
fi
touch "$VOL/.metadata_never_index"   # keep Spotlight from indexing build output
df -h "$VOL" | tail -1
