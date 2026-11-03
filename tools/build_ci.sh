#!/usr/bin/env bash
# GitHub Actions / Linux build: one installable PS4 PKG.
#
# Runs inside a PacBrew environment where OPENORBIS points at the OpenOrbis
# toolchain (the workflow installs ps4-openorbis + pacbrew-ps4-portlibs).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OPENORBIS="${OPENORBIS:-/opt/pacbrew/ps4/openorbis}"
export OPENORBIS
export OO_PS4_TOOLCHAIN="$OPENORBIS"
if [ -f "$OPENORBIS/ps4vars.sh" ]; then
    # shellcheck disable=SC1091
    source "$OPENORBIS/ps4vars.sh"
fi

# ps4vars.sh exports CFLAGS/CXXFLAGS/CPPFLAGS with an extra
# "-isystem $OPENORBIS/include/c++/v1". CMake puts environment flags before
# the toolchain's own flags, and that order makes OpenOrbis' libc++ pick the
# C <math.h> instead of its C++ wrapper. <cmath> then fails on "using ::abs".
# The CMake toolchain file already sets the correct compiler flags.
unset CFLAGS CXXFLAGS CPPFLAGS

# OpenOrbis' crt1.o leaves libc's malloc replacement table empty; libc then
# builds its own heap by mapping about 2.5 GiB of system flexible memory on the
# first malloc. That fails on retail consoles and the next allocation faults at
# address 0x38. Build our crt1.o instead: same entry point and process
# parameters, but the malloc table points at the ps4_replace_* functions, which
# use a dlmalloc heap mapped by ps4_heap.cpp.
CRT1="$ROOT/.build/crt1.o"
mkdir -p "$ROOT/.build"
"$OPENORBIS/bin/clang" --target=x86_64-pc-freebsd12-elf -fPIC \
    -isysroot "$OPENORBIS" -isystem "$OPENORBIS/include" \
    -c "$ROOT/native/ps4/ps4_crt1.S" -o "$CRT1"

SAMPLE=""
NO_PACKAGE=0
DEV_SERVER=0
while [ "$#" -gt 0 ]; do
    case "$1" in
        --sample|-Sample)
            SAMPLE="${2:-}"
            [ -n "$SAMPLE" ] || { echo "--sample needs a name" >&2; exit 2; }
            shift 2
            ;;
        --sample=*)
            SAMPLE="${1#*=}"
            shift
            ;;
        --no-package)
            NO_PACKAGE=1
            shift
            ;;
        --dev-server)
            DEV_SERVER=1
            shift
            ;;
        -h|--help)
            echo "usage: $0 [--sample NAME] [--dev-server] [--no-package]"
            exit 0
            ;;
        *)
            echo "unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

if [ -n "$SAMPLE" ]; then
    case "$SAMPLE" in
        *[!A-Za-z0-9._-]*)
            echo "invalid sample name: $SAMPLE" >&2
            exit 2
            ;;
    esac
    APP="$ROOT/samples/$SAMPLE"
    OUTPUT_NAME="PS4-React-$SAMPLE.pkg"
else
    APP="$ROOT/app"
    OUTPUT_NAME="PS4-React-Starter.pkg"
fi
if [ ! -f "$APP/app.json" ]; then
    echo "app.json not found: $APP/app.json" >&2
    exit 2
fi

LOCK="$ROOT/dependencies.lock.json"
DEPS="$ROOT/.deps"
ER="$DEPS/embeddedReact"
QUICKJS="$DEPS/quickjs"
HB_SRC="$DEPS/harfbuzz-src"
HB_BUILD="$ROOT/.build/harfbuzz"
GEN="$ROOT/.build/react"
BUILD="$ROOT/build"

read -r ER_URL ER_REV QJS_URL QJS_TAG HB_URL HB_SHA <<EOF
$(python3 - "$LOCK" <<'PY'
import json, sys
lock = json.load(open(sys.argv[1], encoding='utf-8'))
print(lock['embeddedReact']['url'], lock['embeddedReact']['revision'],
      lock['quickjs']['url'], lock['quickjs']['tag'],
      lock['harfbuzz']['url'], lock['harfbuzz']['sha256'])
PY
)
EOF

mkdir -p "$DEPS" "$ROOT/.build"

if [ ! -f "$ER/engine/include/er_scene.h" ]; then
    rm -rf "$ER"
    git clone "$ER_URL" "$ER"
    git -C "$ER" checkout --detach "$ER_REV"
fi

if ! grep -q er_image_unload "$ER/engine/include/er_scene.h"; then
    python3 - "$ROOT" "$ER" <<'PY'
import json, pathlib, subprocess, sys
root = pathlib.Path(sys.argv[1])
er = sys.argv[2]
lock = json.loads((root / 'dependencies.lock.json').read_text(encoding='utf-8'))
for relative in lock['embeddedReact']['patches']:
    subprocess.run(['git', '-C', er, 'apply', '--ignore-whitespace', str(root / relative)],
                   check=True)
PY
fi

if [ ! -d "$ER/bridges/quickjs/js/node_modules" ]; then
    (cd "$ER/bridges/quickjs/js" && npm ci --omit=optional --no-audit --no-fund)
fi

if [ ! -f "$QUICKJS/quickjs.h" ]; then
    rm -rf "$QUICKJS"
    git clone --depth 1 --branch "$QJS_TAG" "$QJS_URL" "$QUICKJS"
fi

if [ ! -f "$HB_BUILD/libharfbuzz.a" ] || [ ! -f "$HB_BUILD/libharfbuzz-raster.a" ]; then
    rm -rf "$HB_SRC" "$HB_BUILD"
    mkdir -p "$HB_SRC" "$HB_BUILD"
    curl -L "$HB_URL" -o "$DEPS/harfbuzz.tar.xz"
    echo "$HB_SHA  $DEPS/harfbuzz.tar.xz" | sha256sum -c -
    tar -xf "$DEPS/harfbuzz.tar.xz" -C "$HB_SRC" --strip-components=1
    cmake -S "$HB_SRC" -B "$HB_BUILD" \
        -DCMAKE_TOOLCHAIN_FILE="$OPENORBIS/cmake/ps4.cmake" \
        -DCMAKE_BUILD_TYPE=Release \
        -DHB_BUILD_UTILS=OFF -DHB_BUILD_TESTS=OFF -DHB_BUILD_SUBSET=OFF \
        -DHB_BUILD_VECTOR=OFF -DHB_BUILD_GPU=OFF \
        -DHB_HAVE_FREETYPE=OFF -DHB_HAVE_GLIB=OFF -DHB_HAVE_GOBJECT=OFF \
        -DHB_HAVE_ICU=OFF -DHB_HAVE_GRAPHITE2=OFF -DHB_HAVE_CAIRO=OFF \
        -DHB_HAVE_CORETEXT=OFF -DHB_HAVE_DIRECTWRITE=OFF \
        -DHB_HAVE_UNISCRIBE=OFF -DHB_HAVE_GDI=OFF
    cmake --build "$HB_BUILD" --parallel "$(nproc)" --target harfbuzz harfbuzz-raster
fi

python3 "$ROOT/tools/check_app_config.py" "$APP/app.json"
APP_META=$(python3 - "$APP/app.json" <<'PY'
import json, sys
app = json.load(open(sys.argv[1], encoding='utf-8'))
print('\x1f'.join((app['titleId'], app['contentId'], app['name'], app['version'],
                  '1' if app.get('privileged', False) else '0')))
PY
)
IFS=$'\x1f' read -r TITLE_ID CONTENT_ID TITLE VERSION PRIVILEGED <<< "$APP_META"

rm -rf "$GEN"
mkdir -p "$GEN/app"
cp -a "$APP/." "$GEN/app/"
if [ -n "$SAMPLE" ] && [ -d "$ROOT/samples/assets" ]; then
    mkdir -p "$GEN/assets"
    cp -a "$ROOT/samples/assets/." "$GEN/assets/"
fi
if [ ! -f "$GEN/app/index.jsx" ]; then
    if [ -f "$GEN/app/index.js" ]; then
        printf "import './index.js';\n" > "$GEN/app/index.jsx"
    elif [ -f "$GEN/app/index.ts" ]; then
        printf "import './index.ts';\n" > "$GEN/app/index.jsx"
    elif [ -f "$GEN/app/index.tsx" ]; then
        printf "import './index.tsx';\n" > "$GEN/app/index.jsx"
    else
        echo "app/ has no index.js, index.jsx, index.ts or index.tsx" >&2
        exit 2
    fi
fi

node "$ROOT/tools/ps4_app_config.mjs" "$GEN/app/app.json" 1920 1080
node "$ROOT/tools/app_header.mjs" "$GEN/app/app.json" "$GEN/app_config.generated.h"
node "$ROOT/tools/bundle.mjs" "$ER" "$GEN/app" "$GEN"
node "$ROOT/tools/ps4_bundle_assets.mjs" "$GEN/app" "$GEN/app.bundle.js"
node "$ROOT/tools/bundle.mjs" "$ER" "$GEN/app" "$GEN"
python3 "$ROOT/tools/embed_bundle.py" "$GEN/app.bundle.js" "$GEN/bundle.c"

if [ "${PS4_REACT_KEEP_BUILD:-0}" != "1" ]; then
    rm -rf "$BUILD"
fi
cmake -S "$ROOT" -B "$BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$OPENORBIS/cmake/ps4.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DOPENORBIS="$OPENORBIS" \
    -DER_ROOT="$ER" \
    -DQUICKJS_ROOT="$QUICKJS" \
    -DHARFBUZZ_SRC="$HB_SRC/src" -DHARFBUZZ_LIB_DIR="$HB_BUILD" \
    -DPS4_REACT_GENERATED_DIR="$GEN" \
    -DPS4_REACT_CRT1="$CRT1" \
    -DPS4_REACT_PRIVILEGED="$([ "$PRIVILEGED" = "1" ] && echo ON || echo OFF)" \
    -DPS4_REACT_DEV_SERVER="$([ "$DEV_SERVER" = "1" ] && echo ON || echo OFF)"
cmake --build "$BUILD" --target ps4_react_starter_stage --parallel "$(nproc)"

if [ "$NO_PACKAGE" = "0" ]; then
    PRIVILEGED_ARG=""
    if [ "$PRIVILEGED" = "1" ]; then
        PRIVILEGED_ARG="--privileged"
    fi
    python3 "$ROOT/tools/package_ps4.py" "$BUILD/native/ps4/react" \
        --app-dir "$BUILD/app-react" \
        --title-id "$TITLE_ID" \
        --content-id "$CONTENT_ID" \
        --title "$TITLE" \
        --version "$VERSION" \
        $PRIVILEGED_ARG \
        --assets "$ROOT/assets" \
        --output "$BUILD/$OUTPUT_NAME"
fi
