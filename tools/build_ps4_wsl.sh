#!/usr/bin/env bash
# WSL/rootfs entry point used by tools/build_ps4.ps1.
#
# It bind-mounts /mnt/c into the PacBrew rootfs, enters it and builds the React
# starter. Arguments:
#   $1 rootfs path (inside the WSL distribution)
#   $2 parallel jobs
#   $3 project path (WSL path, normally /mnt/c/...)
#   $4 ER_ROOT path (WSL path)
#   $5 QUICKJS_ROOT path (WSL path)
#   $6 app directory relative to the project (default: app)
#   $7 enable the TCP hot-reload receiver (1) or not (0)
set -euo pipefail

ROOTFS="${1:-/opt/codex-pacbrew-rootfs/rootfs}"
JOBS="${2:-6}"
PROJECT="${3:-/mnt/c/Port/PS4-React}"
ER_ROOT="${4:-}"
QUICKJS_ROOT="${5:-}"
APP_RELATIVE="${6:-app}"
DEV_SERVER="${7:-0}"
OPENORBIS="${OPENORBIS:-/opt/pacbrew/ps4/openorbis}"

if [ ! -d "$ROOTFS" ]; then
    echo "rootfs not found: $ROOTFS" >&2
    exit 2
fi
if [ ! -d "$PROJECT" ]; then
    echo "project not found: $PROJECT" >&2
    exit 2
fi
if [ -z "$ER_ROOT" ] || [ -z "$QUICKJS_ROOT" ]; then
    echo "ER_ROOT and QUICKJS_ROOT are required" >&2
    exit 2
fi

# The project lives on C:, which the chroot only sees after this bind mount.
mkdir -p "$ROOTFS/mnt/c" "$ROOTFS/proc" "$ROOTFS/dev" "$ROOTFS/sys"
if ! mountpoint -q "$ROOTFS/mnt/c"; then
    mount --bind /mnt/c "$ROOTFS/mnt/c"
fi
# PkgTool.Core (and some build tools) need the usual kernel filesystems.
mountpoint -q "$ROOTFS/proc" || mount -t proc proc "$ROOTFS/proc"
mountpoint -q "$ROOTFS/dev" || mount --rbind /dev "$ROOTFS/dev"
mountpoint -q "$ROOTFS/sys" || mount --rbind /sys "$ROOTFS/sys"

chroot "$ROOTFS" /usr/bin/env OPENORBIS="$OPENORBIS" /bin/bash -lc "
    set -euo pipefail
    cd '$PROJECT'
    python3 '$PROJECT/tools/check_app_config.py' '$PROJECT/$APP_RELATIVE/app.json'
    # Text shaping needs HarfBuzz 14.6 + its raster addon. The PacBrew rootfs
    # carries the pinned source under /workspace; build it once into .build.
    if [ ! -f '$PROJECT/.build/harfbuzz/libharfbuzz.a' ]; then
        cmake -S /workspace/harfbuzz -B '$PROJECT/.build/harfbuzz' \
            -DCMAKE_TOOLCHAIN_FILE='$OPENORBIS/cmake/ps4.cmake' \
            -DCMAKE_BUILD_TYPE=Release \
            -DHB_BUILD_UTILS=OFF -DHB_BUILD_TESTS=OFF -DHB_BUILD_SUBSET=OFF \
            -DHB_BUILD_VECTOR=OFF -DHB_BUILD_GPU=OFF \
            -DHB_HAVE_FREETYPE=OFF -DHB_HAVE_GLIB=OFF -DHB_HAVE_GOBJECT=OFF \
            -DHB_HAVE_ICU=OFF -DHB_HAVE_GRAPHITE2=OFF -DHB_HAVE_CAIRO=OFF \
            -DHB_HAVE_CORETEXT=OFF -DHB_HAVE_DIRECTWRITE=OFF \
            -DHB_HAVE_UNISCRIBE=OFF -DHB_HAVE_GDI=OFF
        cmake --build '$PROJECT/.build/harfbuzz' --parallel '$JOBS' \
            --target harfbuzz harfbuzz-raste
    fi
    rm -rf build
    cmake -S . -B build \
        -DCMAKE_TOOLCHAIN_FILE='$OPENORBIS/cmake/ps4.cmake' \
        -DCMAKE_BUILD_TYPE=Release \
        -DOPENORBIS='$OPENORBIS' \
        -DER_ROOT='$ER_ROOT' \
        -DQUICKJS_ROOT='$QUICKJS_ROOT' \
        -DHARFBUZZ_LIB_DIR='$PROJECT/.build/harfbuzz' \
        -DPS4_REACT_GENERATED_DIR='$PROJECT/.build/react' \
        -DPS4_REACT_DEV_SERVER='$([ "$DEV_SERVER" = "1" ] && echo ON || echo OFF)'
    cmake --build build --target ps4_react_starter_stage --parallel '$JOBS'
    echo
    echo 'Build complete:'
    ls -l build/native/ps4/react/ps4_react_starter build/native/ps4/react/eboot.bin
"
