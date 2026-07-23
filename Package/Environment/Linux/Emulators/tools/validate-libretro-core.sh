#!/usr/bin/env bash
set -euo pipefail

[[ $# -eq 1 ]] || {
    echo "Usage: $0 <core-package-directory>" >&2
    exit 2
}

CORE_ROOT="$1"

fail() { echo "[FAIL] $*" >&2; exit 1; }
pass() { echo "[PASS] $*"; }

[[ -d "$CORE_ROOT" ]] || fail "core directory missing: $CORE_ROOT"
[[ -f "$CORE_ROOT/core.cfg" ]] || fail "core.cfg missing"
[[ -f "$CORE_ROOT/core-options.cfg" ]] || fail "core-options.cfg missing"

for dir in system saves states remaps; do
    [[ -d "$CORE_ROOT/$dir" ]] || fail "$dir/ missing"
    pass "$dir/ exists"
done

if grep -Eq \
  '^[[:space:]]*(system_directory|savefile_directory|savestate_directory|core_options_path|input_remapping_directory)[[:space:]]*=' \
  "$CORE_ROOT/core.cfg"
then
    fail "core.cfg contains launcher-managed path keys"
fi
pass "core.cfg contains no launcher-managed path keys"

mapfile -t libs < <(
    find "$CORE_ROOT" -maxdepth 1 -type f -name '*_libretro.so' -print | sort
)
[[ ${#libs[@]} -eq 1 ]] || \
    fail "expected exactly one *_libretro.so; found ${#libs[@]}"
CORE="${libs[0]}"
pass "exactly one libretro core: $(basename "$CORE")"

SYMBOLS_TMP="$(mktemp)"
trap 'rm -f "$SYMBOLS_TMP"' EXIT
nm -D "$CORE" | awk '{print $3}' > "$SYMBOLS_TMP"

for symbol in \
    retro_api_version \
    retro_init \
    retro_deinit \
    retro_load_game \
    retro_unload_game \
    retro_run
do
    grep -Fxq "$symbol" "$SYMBOLS_TMP" || fail "missing export: $symbol"
    pass "export $symbol"
done

DEP_REPORT="$(ldd "$CORE" 2>&1 || true)"
if grep -q 'not found' <<<"$DEP_REPORT"; then
    printf '%s\n' "$DEP_REPORT" >&2
    fail "unresolved shared-library dependency"
fi
pass "shared-library dependencies resolve"
