#!/usr/bin/env bash
# Prism - launch under Wine or Proton with the overrides ReShade needs
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST="${PRISM_DIST:-$ROOT/build/dist}"

PROTON=""
ARGS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --proton) PROTON="${2:-}"; shift 2 ;;
        --help|-h)
            cat <<'USAGE'
usage: run-prism.sh [--proton /path/to/proton] [-- prism arguments]

  --proton PATH   launch through the given Proton script instead of wine
  --test-pattern  (passed to Prism) render a synthetic pattern, no capture
  --diagnostics   (passed to Prism) open the diagnostics window at startup
USAGE
            exit 0 ;;
        --) shift; ARGS+=("$@"); break ;;
        *) ARGS+=("$1"); shift ;;
    esac
done

[ -f "$DIST/Prism.exe" ] || { echo "Prism.exe not found in $DIST - run scripts/build.sh first" >&2; exit 1; }
[ -f "$DIST/PrismCapture.dll" ] || echo "warning: PrismCapture.dll missing from $DIST; capture will be unavailable" >&2

# ReShade's proxy dxgi.dll and the shader compiler both have to win over Wine's
# builtins, or ReShade never loads and Prism falls back to an unscaled blit.
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-d3dcompiler_47,dxgi=n,b}"

cd "$DIST"

if [ -n "$PROTON" ]; then
    [ -x "$PROTON" ] || { echo "not executable: $PROTON" >&2; exit 1; }
    export STEAM_COMPAT_CLIENT_INSTALL_PATH="${STEAM_COMPAT_CLIENT_INSTALL_PATH:-$HOME/.steam/steam}"
    export STEAM_COMPAT_DATA_PATH="${STEAM_COMPAT_DATA_PATH:-$HOME/.local/share/prism/proton}"
    mkdir -p "$STEAM_COMPAT_DATA_PATH"
    exec "$PROTON" run "$DIST/Prism.exe" "${ARGS[@]+"${ARGS[@]}"}"
fi

export WINEPREFIX="${WINEPREFIX:-$HOME/.local/share/prism/pfx}"
mkdir -p "$WINEPREFIX"
exec wine Prism.exe "${ARGS[@]+"${ARGS[@]}"}"
