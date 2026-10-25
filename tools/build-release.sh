#!/bin/bash
# Build the deployable site into web/dist and run the release guards.
#   REPO_URL=https://github.com/<owner>/<repo> tools/build-release.sh
# Optional: BELLA_BUILD_WEB=$PWD/work/build-web-<id> (private Emscripten build dir).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
: "${REPO_URL:?set REPO_URL to the public source repository (shown on the page and the licenses page)}"
WORKER_ID="${WORKER_ID:-x4-release}"
DIST="$ROOT/web/dist"
BELLA_BUILD_WEB="${BELLA_BUILD_WEB:-$ROOT/work/build-web-$WORKER_ID}"
export DETECTIVE_BUILD_WEB="$BELLA_BUILD_WEB"

"$ROOT/tools/build-web.sh"
(cd "$ROOT/web" && npx vite build)
sed -i "s#__REPO_URL__#${REPO_URL}#g" "$DIST/index.html" "$DIST/licenses.html"

echo "== placeholders and development references"
if grep -rl "__REPO_URL__" "$DIST"; then echo "FAIL: unfilled placeholder"; exit 1; fi
if grep -rlE "/dev-engine|/dev-iso|work/(install|build|iso|tools)|research/" "$DIST"; then echo "FAIL: development reference"; exit 1; fi
echo "ok"

echo "== shared bytes with the game"
node "$ROOT/tools/scan-dist.mjs" "$DIST" "$ROOT/work/install" "$ROOT/work/ref" \
  --allow "$ROOT/node_modules/iced-x86/iced_x86_bg.wasm" \
  --allow "$ROOT/tools/scan-allow/mit-license.txt"

UM="${UM:-$HOME/repos/universal-modder/bin/um}"
if [ -x "$UM" ]; then
  echo "== um publish check (site)"
  "$UM" publish check "$DIST" --game "$ROOT/work/install"
  if git -C "$ROOT" rev-parse HEAD >/dev/null 2>&1; then
    echo "== um publish check (repository, committed files only)"
    EXPORT="$(mktemp -d)"
    trap 'rm -rf "$EXPORT"' EXIT
    git -C "$ROOT" archive HEAD | tar -x -C "$EXPORT"
    "$UM" publish check "$EXPORT" --game "$ROOT/work/install"
  else
    echo "== um publish check (repository): skip (not a git checkout)"
  fi
else
  echo "== um publish check: skip (UM not found at $UM)"
fi

echo "release build ready in $DIST"
