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

LOCK="$ROOT/dependencies.lock.json"
DEPS="$ROOT/.deps"
ER="$DEPS/embeddedReact"
QUICKJS="$DEPS/quickjs"
HB_SRC="$DEPS/harfbuzz-src"
HB_BUILD="$ROOT/.build/harfbuzz"
GEN="$ROOT/.build/react"
BUILD="$ROOT/build"
APP="$ROOT/app"

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

rm -rf "$GEN"
mkdir -p "$GEN/app"
cp -a "$APP/." "$GEN/app/"
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
node "$ROOT/tools/bundle.mjs" "$ER" "$GEN/app" "$GEN"
node "$ROOT/tools/ps4_bundle_assets.mjs" "$GEN/app" "$GEN/app.bundle.js"
node "$ROOT/tools/bundle.mjs" "$ER" "$GEN/app" "$GEN"
python3 "$ROOT/tools/embed_bundle.py" "$GEN/app.bundle.js" "$GEN/bundle.c"

rm -rf "$BUILD"
cmake -S "$ROOT" -B "$BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$OPENORBIS/cmake/ps4.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DOPENORBIS="$OPENORBIS" \
    -DER_ROOT="$ER" \
    -DQUICKJS_ROOT="$QUICKJS" \
    -DHARFBUZZ_SRC="$HB_SRC/src" -DHARFBUZZ_LIB_DIR="$HB_BUILD" \
    -DPS4_REACT_GENERATED_DIR="$GEN"
cmake --build "$BUILD" --target ps4_react_starter_stage --parallel "$(nproc)"

python3 "$ROOT/tools/package_ps4.py" "$BUILD/native/ps4/react" \
    --app-dir "$BUILD/app-react" \
    --title-id CUSA99059 \
    --content-id UP0001-CUSA99059_00-PS4REACTSTARTER0 \
    --title "PS4 React Starter" \
    --assets "$ROOT/assets" \
    --output "$BUILD/PS4-React-Starter.pkg"
