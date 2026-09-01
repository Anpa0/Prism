#!/usr/bin/env bash
# Prism - launch under Wine or Proton with the overrides ReShade needs, and
# optionally pin Prism (and only Prism) to a particular GPU.
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIST="${PRISM_DIST:-$ROOT/build/dist}"
# shellcheck source=lib-gpu.sh
. "$ROOT/scripts/lib-gpu.sh"

PROTON=""
GPU="auto"
ARGS=()

usage() {
    cat <<'USAGE'
usage: run-prism.sh [--proton PATH] [--gpu amd|nvidia|auto] [-- prism arguments]

  --proton PATH   launch through the given Proton script instead of wine
  --gpu WHICH     which GPU Prism itself renders on. Affects this process only:
                  no other application, and not the compositor.
                    auto   (default) leave the session default alone
                    amd    pin Prism to the AMD GPU
                    nvidia pin Prism to the NVIDIA GPU

Arguments passed through to Prism:
  --test-pattern  render a synthetic pattern; no portal or game needed
  --gameplay      start straight into fullscreen gameplay output
  --diagnostics   open the diagnostics window at startup
  --monitor=N     put gameplay output on monitor N (see the Output Display menu)
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --proton) PROTON="${2:-}"; shift 2 ;;
        --gpu) GPU="${2:-auto}"; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        --) shift; ARGS+=("$@"); break ;;
        *) ARGS+=("$1"); shift ;;
    esac
done

[ -f "$DIST/Prism.exe" ] || { echo "Prism.exe not found in $DIST - run scripts/build.sh first" >&2; exit 1; }
[ -f "$DIST/PrismCapture.dll" ] || echo "warning: PrismCapture.dll missing from $DIST; capture will be unavailable" >&2

# ReShade's proxy dxgi.dll and the shader compiler both have to win over Wine's
# builtins, or ReShade never loads and Prism falls back to an unscaled blit.
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-d3dcompiler_47,dxgi=n,b}"

# --------------------------------------------------------- GPU selection --
#
# These variables are exported into Prism's process only. Nothing here is
# written to disk, nothing changes the session default, and no other application
# is affected. Prism's own GPU menu is still the final say: DXGI enumerates
# whatever Vulkan exposes, and Prism picks from that list.
if [ "$GPU" != "auto" ]; then
    line=$(prism_find_gpu "$GPU" || true)
    if [ -z "$line" ]; then
        echo "error: no $GPU GPU found. ./scripts/check-gpus.sh lists what is present." >&2
        exit 1
    fi
    IFS=$'\t' read -r pci vendor device _ _ _ _ _ _ _ _ _ name <<<"$line"

    # The Mesa device-select layer is a Vulkan *loader* layer, so it reorders
    # every ICD's devices, NVIDIA's included. The trailing '!' makes it exclusive
    # rather than merely preferred.
    export MESA_VK_DEVICE_SELECT="${vendor}:${device}!"
    export VK_DEVICE_SELECT_PCI_BUS_ID="$pci"
    export DRI_PRIME="$(prism_dri_prime_tag "$pci")"

    if [ "$GPU" = "nvidia" ]; then
        # Harmless on a desktop with two full GPUs; needed on offload setups.
        export __NV_PRIME_RENDER_OFFLOAD=1
        export __GLX_VENDOR_LIBRARY_NAME=nvidia
        export __VK_LAYER_NV_optimus=NVIDIA_only
    fi

    echo "Prism GPU: $name  (PCI $pci, ${vendor}:${device})"
    echo "  MESA_VK_DEVICE_SELECT=$MESA_VK_DEVICE_SELECT  DRI_PRIME=$DRI_PRIME"
    echo "  This process only. Check the diagnostics window to confirm what DXGI picked."
fi

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
