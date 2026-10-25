#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
RECON="$ROOT/docs/RECON.md"
SRC="$ROOT/runtime/src"

if [[ ! -f "$RECON" ]]; then
  echo "missing $RECON" >&2
  exit 2
fi

# Expected imports: bullet lists under each ### DLL (N) section in RECON.md.
declare -a EXPECTED=()
current_dll=""
while IFS= read -r line; do
  if [[ "$line" =~ ^###[[:space:]](.+)[[:space:]]\(([0-9]+)\) ]]; then
    case "${BASH_REMATCH[1]}" in
      KERNEL32.dll) current_dll="kernel32.dll" ;;
      USER32.dll) current_dll="user32.dll" ;;
      GDI32.dll) current_dll="gdi32.dll" ;;
      ADVAPI32.dll) current_dll="advapi32.dll" ;;
      comdlg32.dll) current_dll="comdlg32.dll" ;;
      DDRAW.dll) current_dll="ddraw.dll" ;;
      DSOUND.dll) current_dll="dsound.dll" ;;
      WINMM.dll) current_dll="winmm.dll" ;;
      *) current_dll="" ;;
    esac
    continue
  fi
  if [[ -z "$current_dll" ]]; then
    continue
  fi
  if [[ "$line" =~ ^[A-Za-z_@] ]]; then
    IFS=',' read -ra names <<<"$line"
    for raw in "${names[@]}"; do
      name="$(echo "$raw" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//;s/\.$//')"
      [[ -n "$name" ]] && EXPECTED+=("${current_dll}!${name}")
    done
    current_dll=""
  fi
done <"$RECON"

if [[ "${#EXPECTED[@]}" -ne 135 ]]; then
  echo "expected 135 RECON imports, parsed ${#EXPECTED[@]}" >&2
  exit 2
fi

# Collect HLE registrations from runtime/src (skip d3d8, gles, fmod).
mapfile -t REG_LINES < <(
  grep -R -n 'HLE_STDCALL' "$SRC" \
    --include='*.cpp' \
    --exclude-dir=d3d8 --exclude-dir=gles --exclude-dir=fmod \
    2>/dev/null || true
)

declare -A COUNT
declare -A LOCS

for entry in "${REG_LINES[@]}"; do
  file="${entry%%:*}"
  rest="${entry#*:}"
  body="${rest#*:}"
  if [[ "$body" =~ HLE_STDCALL_AS\([[:space:]]*\"([^\"]+)\"[[:space:]]*,[[:space:]]*[^,]+,[[:space:]]*\"([^\"]+)\" ]]; then
    dll="${BASH_REMATCH[1],,}"
    name="${BASH_REMATCH[2]}"
  elif [[ "$body" =~ HLE_STDCALL\([[:space:]]*\"([^\"]+)\"[[:space:]]*,[[:space:]]*([A-Za-z0-9_]+) ]]; then
    dll="${BASH_REMATCH[1],,}"
    name="${BASH_REMATCH[2]}"
  elif [[ "$body" =~ HLE_STDCALL\([[:space:]]*kDll,[[:space:]]*([A-Za-z0-9_]+) ]]; then
    hdr="$(sed -n 's/.*const char\* kDll = "\([^"]*\)".*/\1/p' "$file" | head -1)"
    [[ -z "$hdr" ]] && continue
    dll="${hdr,,}"
    name="${BASH_REMATCH[1]}"
  elif [[ "$body" =~ HLE_STDCALL\([[:space:]]*(k[A-Za-z0-9_]+),[[:space:]]*([A-Za-z0-9_]+) ]]; then
    sym="${BASH_REMATCH[1]}"
    name="${BASH_REMATCH[2]}"
    hdr="$(sed -n "s/.*const char\\* ${sym} = \"\\([^\"]*\\)\".*/\\1/p" "$file" | head -1)"
    [[ -z "$hdr" ]] && continue
    dll="${hdr,,}"
  else
    continue
  fi
  key="${dll}!${name}"
  COUNT[$key]=$(( ${COUNT[$key]:-0} + 1 ))
  LOCS[$key]="${LOCS[$key]:-}${file};"
done

# wsprintfA is cdecl (caller pops args); registered manually in win32_extra.cpp.
manual="$SRC/hle/win32_extra.cpp"
if grep -q 'HleRegistrar.*"wsprintfA"' "$manual" 2>/dev/null; then
  key="user32.dll!wsprintfA"
  COUNT[$key]=$(( ${COUNT[$key]:-0} + 1 ))
  LOCS[$key]="${LOCS[$key]:-}${manual};"
fi

missing=0
dup=0
for key in "${EXPECTED[@]}"; do
  n="${COUNT[$key]:-0}"
  if [[ "$n" -eq 0 ]]; then
    echo "MISSING: $key" >&2
    missing=$((missing + 1))
  elif [[ "$n" -ne 1 ]]; then
    echo "DUPLICATE ($n): $key in ${LOCS[$key]}" >&2
    dup=$((dup + 1))
  fi
done

if [[ "$missing" -ne 0 || "$dup" -ne 0 ]]; then
  echo "registry check failed: $missing missing, $dup duplicate" >&2
  exit 1
fi

echo "OK: all 135 BDETECT.EXE imports registered exactly once"
exit 0
