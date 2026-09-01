#!/usr/bin/env bash
# Prism - opt-in GPU policy for a KDE Plasma Wayland session.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Goal, and the whole of it:
#
#     KWin, Plasma and every ordinary application  ->  the AMD GPU
#     Prism, and only Prism                        ->  whichever GPU you launch it with
#
# So this script writes exactly one thing: a KWin render-device preference that
# pins the compositor to the AMD card by its stable /dev/dri/by-path name. It
# does not make NVIDIA the default for anything, does not touch the NVIDIA
# driver, does not edit Xorg or modprobe configuration, and does not remove
# amdgpu. Prism opts into the second GPU per-launch, through run-prism.sh.
#
# Nothing is written without showing you the exact file first and asking.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=lib-gpu.sh
. "$ROOT/scripts/lib-gpu.sh"

ENV_DIR="${HOME}/.config/plasma-workspace/env"
ENV_FILE="${ENV_DIR}/10-prism-gpu-policy.sh"
BACKUP_DIR="${HOME}/.local/share/prism/backups"

MODE="status"
ASSUME_YES=0

usage() {
    cat <<'USAGE'
usage: configure-gpu-policy.sh [--status | --apply | --restore] [--yes]

  --status    (default) show the GPUs, the current policy and what would change
  --apply     pin KWin to the AMD GPU, after showing the file and confirming
  --restore   put back the most recent backup, or remove the file if there was
              none before
  --yes       skip the interactive confirmation (for scripted use)

The only file this touches is:
  ~/.config/plasma-workspace/env/10-prism-gpu-policy.sh

Plasma sources that directory when the session starts, so a change takes effect
at your next login and is undone by removing the file. Nothing outside your home
directory is modified and no root privileges are needed.
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --status) MODE="status"; shift ;;
        --apply) MODE="apply"; shift ;;
        --restore) MODE="restore"; shift ;;
        --yes|-y) ASSUME_YES=1; shift ;;
        --help|-h) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage; exit 2 ;;
    esac
done

say() { printf '%s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

confirm() {
    [ "$ASSUME_YES" = "1" ] && return 0
    printf '\n%s [y/N] ' "$1"
    read -r answer
    case "$answer" in [yY]|[yY][eE][sS]) return 0 ;; *) return 1 ;; esac
}

# ---------------------------------------------------------------- detection --

amd_line=$(prism_find_gpu amd || true)
nvidia_line=$(prism_find_gpu nvidia || true)

if [ -z "$amd_line" ]; then
    die "no AMD GPU found. This policy exists to keep KWin on the AMD card; with no AMD card there is nothing to pin."
fi

IFS=$'\t' read -r amd_pci _ _ amd_driver amd_card amd_render amd_by_path _ _ _ _ _ amd_name <<<"$amd_line"
if [ -n "$nvidia_line" ]; then
    IFS=$'\t' read -r nv_pci _ _ nv_driver nv_card nv_render nv_by_path _ _ _ _ _ nv_name <<<"$nvidia_line"
fi

say "GPUs detected"
say "  AMD     ${amd_name}"
say "          PCI ${amd_pci}  driver ${amd_driver}  ${amd_card}/${amd_render}"
say "          ${amd_by_path}"
if [ -n "$nvidia_line" ]; then
    say "  NVIDIA  ${nv_name}"
    say "          PCI ${nv_pci}  driver ${nv_driver}  ${nv_card}/${nv_render}"
    say "          ${nv_by_path}"
else
    say "  NVIDIA  not present"
fi

if [ "$amd_by_path" = "-" ]; then
    die "the AMD card has no /dev/dri/by-path entry, so there is no stable name to pin KWin to. Report the output of scripts/check-gpus.sh."
fi

say ""
say "Current policy"
if [ -f "$ENV_FILE" ]; then
    say "  ${ENV_FILE} exists:"
    sed 's/^/    /' "$ENV_FILE"
else
    say "  ${ENV_FILE} does not exist (KWin picks its own render device)"
fi

if pidof kwin_wayland >/dev/null 2>&1; then
    running=$(tr '\0' '\n' < "/proc/$(pidof kwin_wayland | awk '{print $1}')/environ" 2>/dev/null |
              grep -E '^KWIN_DRM_DEVICES=' || true)
    say "  Running KWin: ${running:-KWIN_DRM_DEVICES not set in the live session}"
fi

# ------------------------------------------------------------------ actions --

proposed=$(cat <<EOF
# Written by Prism's scripts/configure-gpu-policy.sh
#
# Pins KWin to the AMD GPU by its stable PCI path, so the compositor, Plasma and
# every ordinary application keep rendering on it even once a second GPU is
# installed. Prism opts into the other GPU per-launch; nothing here makes any
# other application use it.
#
# Remove this file (or run configure-gpu-policy.sh --restore) and log out to
# return to KWin's own choice.
export KWIN_DRM_DEVICES=${amd_by_path}
EOF
)

case "$MODE" in
status)
    say ""
    say "Proposed contents of ${ENV_FILE}:"
    printf '%s\n' "$proposed" | sed 's/^/    /'
    say ""
    say "Nothing has been changed. Run with --apply to write it."
    ;;

apply)
    if [ -z "$nvidia_line" ]; then
        say ""
        say "Note: only one GPU is present, so KWin has nothing to choose between."
        say "Applying the pin now is still harmless and makes the policy explicit"
        say "before the second card arrives."
    fi

    say ""
    say "About to write ${ENV_FILE}:"
    printf '%s\n' "$proposed" | sed 's/^/    /'
    say ""
    say "This changes nothing else. It takes effect at your next login."

    confirm "Write this file?" || { say "Cancelled; nothing was changed."; exit 0; }

    mkdir -p "$ENV_DIR" "$BACKUP_DIR"
    if [ -f "$ENV_FILE" ]; then
        backup="${BACKUP_DIR}/10-prism-gpu-policy.sh.$(date +%Y%m%d-%H%M%S)"
        cp -a "$ENV_FILE" "$backup"
        say "Backed up the previous file to ${backup}"
    else
        # Record that there was nothing here, so --restore can remove the file.
        : > "${BACKUP_DIR}/10-prism-gpu-policy.sh.absent"
    fi

    printf '%s\n' "$proposed" > "$ENV_FILE"
    chmod 0644 "$ENV_FILE"
    say "Wrote ${ENV_FILE}"
    say ""
    say "Log out and back in, then confirm with:"
    say "  ./scripts/check-gpus.sh   (the 'KWin render devices' line)"
    ;;

restore)
    latest=$(ls -1t "${BACKUP_DIR}"/10-prism-gpu-policy.sh.* 2>/dev/null | grep -v '\.absent$' | head -1 || true)

    if [ -n "$latest" ]; then
        say ""
        say "Restoring ${ENV_FILE} from ${latest}:"
        sed 's/^/    /' "$latest"
        confirm "Restore it?" || { say "Cancelled; nothing was changed."; exit 0; }
        mkdir -p "$ENV_DIR"
        cp -a "$latest" "$ENV_FILE"
        say "Restored."
    elif [ -f "$ENV_FILE" ]; then
        say ""
        say "There was no file here before Prism wrote one, so restoring means"
        say "removing ${ENV_FILE}."
        confirm "Remove it?" || { say "Cancelled; nothing was changed."; exit 0; }
        rm -f "$ENV_FILE"
        rm -f "${BACKUP_DIR}/10-prism-gpu-policy.sh.absent"
        say "Removed."
    else
        say ""
        say "Nothing to restore: ${ENV_FILE} does not exist."
    fi
    say ""
    say "Log out and back in for the change to take effect."
    ;;
esac
