#!/usr/bin/env bash
# Prism - report what the GPUs, DRM devices and GPU-selection environment look
# like right now. Read-only: this script changes nothing.
# SPDX-License-Identifier: GPL-3.0-or-later
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=lib-gpu.sh
. "$ROOT/scripts/lib-gpu.sh"

rule() { printf '\n== %s ==\n' "$1"; }
have() { command -v "$1" >/dev/null 2>&1; }

rule "Session"
printf '  XDG_SESSION_TYPE     %s\n' "${XDG_SESSION_TYPE:-(unset)}"
printf '  XDG_CURRENT_DESKTOP  %s\n' "${XDG_CURRENT_DESKTOP:-(unset)}"
printf '  Kernel               %s\n' "$(uname -r)"
if have kwin_wayland && pidof kwin_wayland >/dev/null 2>&1; then
    printf '  Compositor           kwin_wayland (running)\n'
    kwin_env=$(tr '\0' '\n' < "/proc/$(pidof kwin_wayland | awk '{print $1}')/environ" 2>/dev/null |
               grep -E '^KWIN_DRM_DEVICES=' || true)
    printf '  KWin render devices  %s\n' "${kwin_env:-(not pinned - KWin chose for itself)}"
fi

rule "GPUs (from /sys/bus/pci, keyed by PCI address)"
found=0
while IFS=$'\t' read -r pci vendor device driver card render by_path cur_s cur_w max_s max_w boot name; do
    found=$((found + 1))
    printf '\n  [%d] %s\n' "$found" "$name"
    printf '      PCI address     %s   (%s:%s)\n' "$pci" "$vendor" "$device"
    printf '      Kernel driver   %s\n' "$driver"
    printf '      DRM nodes       %s / %s\n' "$card" "$render"
    printf '      Stable path     %s\n' "$by_path"
    printf '      Boot VGA        %s\n' "$([ "$boot" = "1" ] && echo yes || echo no)"
    printf '      PCIe link       %s x%s   (max %s x%s)' "$cur_s" "$cur_w" "$max_s" "$max_w"
    if [ "$cur_w" != "?" ] && [ "$max_w" != "?" ] && [ "$cur_w" -lt "$max_w" ] 2>/dev/null; then
        printf '   <-- WIDTH BELOW MAXIMUM\n'
    elif [ "$cur_s" != "$max_s" ] && [ "$cur_s" != "?" ]; then
        printf '   <-- SPEED BELOW MAXIMUM\n'
    else
        printf '\n'
    fi
    printf '      DRI_PRIME tag   %s\n' "$(prism_dri_prime_tag "$pci")"
done < <(prism_gpus)
[ "$found" -eq 0 ] && printf '  (no display-class PCI devices found)\n'

printf '\n  Note: a GPU on an external PCIe dock idles its link down. Re-check under\n'
printf '  load before concluding that it trained badly:\n'
printf '    sudo lspci -vv -s <pci-address> | grep -E "LnkCap|LnkSta"\n'

rule "/dev/dri"
ls -l "$PRISM_SYSFS_ROOT/dev/dri" 2>/dev/null | sed 's/^/  /' || printf '  (missing)\n'
printf '\n'
ls -l "$PRISM_SYSFS_ROOT/dev/dri/by-path" 2>/dev/null | sed 's/^/  /' || printf '  (no by-path directory)\n'

rule "Vulkan"
if have vulkaninfo; then
    vulkaninfo --summary 2>/dev/null |
        grep -E 'GPU[0-9]|deviceName|driverName|apiVersion|deviceType' | sed 's/^/  /' ||
        printf '  vulkaninfo produced no device summary\n'
else
    printf '  vulkaninfo not installed (dnf install vulkan-tools)\n'
fi

rule "NVIDIA"
if have nvidia-smi; then
    nvidia-smi --query-gpu=name,driver_version,pci.bus_id,pcie.link.gen.current,pcie.link.width.current,pcie.link.gen.max,pcie.link.width.max \
        --format=csv 2>/dev/null | sed 's/^/  /' || printf '  nvidia-smi failed\n'
    printf '\n  Kernel modules:\n'
    lsmod 2>/dev/null | grep -E '^nvidia' | sed 's/^/    /' || printf '    (none loaded)\n'
    printf '  DRM/KMS modeset: %s\n' "$(cat /sys/module/nvidia_drm/parameters/modeset 2>/dev/null || echo '(nvidia_drm not loaded)')"
    printf '  Open kernel module: %s\n' \
        "$(modinfo nvidia 2>/dev/null | grep -qi 'license.*MIT' && echo 'yes (open)' || echo 'proprietary or not installed')"
else
    printf '  nvidia-smi not present - no NVIDIA driver installed yet.\n'
    printf '  That is expected while Prism runs single-GPU on the AMD card.\n'
fi

rule "GPU selection environment"
for v in DRI_PRIME MESA_VK_DEVICE_SELECT VK_DEVICE_SELECT_PCI_BUS_ID VK_LOADER_DEVICE_SELECT \
         DXVK_FILTER_DEVICE_NAME __NV_PRIME_RENDER_OFFLOAD __GLX_VENDOR_LIBRARY_NAME \
         __VK_LAYER_NV_optimus KWIN_DRM_DEVICES; do
    printf '  %-28s %s\n' "$v" "$(printenv "$v" 2>/dev/null || echo '(unset)')"
done

rule "What Prism would use"
printf '  Prism picks its adapter through DXGI inside Wine, so the definitive\n'
printf '  answer comes from Prism itself:\n\n'
printf '    ./scripts/run-prism.sh --test-pattern --diagnostics\n\n'
printf '  and read the "Renderer" and "System" sections of the diagnostics window.\n'

rule "Displays (KDE)"
if have kscreen-doctor; then
    kscreen-doctor -o 2>/dev/null | sed 's/^/  /' || printf '  kscreen-doctor failed\n'
else
    printf '  kscreen-doctor not present (it ships with plasma-workspace)\n'
fi

printf '\n'
