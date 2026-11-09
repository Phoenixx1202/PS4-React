#!/usr/bin/env bash
# Package a built PS4 React app into a PKG inside the PacBrew rootfs.
#
# Usage:
#   tools/package_ps4_wsl.sh [rootfs] [project] [build-dir] [app-dir] [title-id] [content-id] [title] [output] [version]
set -euo pipefail

ROOTFS="${1:-/opt/codex-pacbrew-rootfs/rootfs}"
PROJECT="${2:-/mnt/c/Port/PS4-React}"
BUILD="${3:-$PROJECT/build/native/ps4}"
APP="${4:-$PROJECT/build/app}"
TITLE_ID="${5:-CUSA99059}"
CONTENT_ID="${6:-UP0001-CUSA99059_00-PS4REACTSTARTER0}"
TITLE="${7:-PS4 React Starter}"
OUTPUT="${8:-$PROJECT/build/PS4-React-Starter.pkg}"
VERSION="${9:-01.00}"
OPENORBIS="${OPENORBIS:-/opt/pacbrew/ps4/openorbis}"

for WINDOWS_MOUNT in /mnt/c /mnt/d; do
    if [ -d "$WINDOWS_MOUNT" ]; then
        mkdir -p "$ROOTFS$WINDOWS_MOUNT"
        if ! mountpoint -q "$ROOTFS$WINDOWS_MOUNT"; then
            mount --bind "$WINDOWS_MOUNT" "$ROOTFS$WINDOWS_MOUNT"
        fi
    fi
done
mkdir -p "$ROOTFS/proc" "$ROOTFS/dev" "$ROOTFS/sys"
# The .NET-based PkgTool.Core resolves /proc/self/exe; mount proc before chroot.
mountpoint -q "$ROOTFS/proc" || mount -t proc proc "$ROOTFS/proc"
mountpoint -q "$ROOTFS/dev" || mount --rbind /dev "$ROOTFS/dev"
mountpoint -q "$ROOTFS/sys" || mount --rbind /sys "$ROOTFS/sys"

chroot "$ROOTFS" /usr/bin/env OPENORBIS="$OPENORBIS" /bin/bash -lc "
    set -euo pipefail
    python3 '$PROJECT/tools/package_ps4.py' '$BUILD' \
        --app-dir '$APP' \
        --title-id '$TITLE_ID' \
        --content-id '$CONTENT_ID' \
        --title '$TITLE' \
        --version '$VERSION' \
        --output '$OUTPUT'
"
