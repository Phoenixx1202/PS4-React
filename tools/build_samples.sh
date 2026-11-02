#!/usr/bin/env bash
# Build every sample into samples-pkg/. The build tree is reused between
# samples, so only the generated bundle and the final link are redone.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUTPUT="$ROOT/samples-pkg"
mkdir -p "$OUTPUT"
rm -f "$OUTPUT"/*.pkg

samples=()
for dir in "$ROOT"/samples/*/; do
    [ -f "$dir/app.json" ] && samples+=("$(basename "$dir")")
done
if [ "${#samples[@]}" -eq 0 ]; then
    echo "no samples found under $ROOT/samples" >&2
    exit 2
fi

if [ "${PS4_REACT_REUSE_BUILD:-0}" != "1" ]; then
    rm -rf "$ROOT/build"
fi
export PS4_REACT_KEEP_BUILD=1

for sample in "${samples[@]}"; do
    echo
    echo "=== $sample ==="
    bash "$ROOT/tools/build_ci.sh" --sample "$sample"
    pkg="$ROOT/build/PS4-React-$sample.pkg"
    [ -f "$pkg" ] || { echo "missing sample PKG: $pkg" >&2; exit 1; }
    cp -f "$pkg" "$OUTPUT/"
done

echo
echo "Sample packages:"
ls -l "$OUTPUT"/*.pkg
