#!/usr/bin/env bash
# Prism - install the desktop entry the portal needs to identify Prism.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# xdg-desktop-portal 1.20 added org.freedesktop.host.portal.Registry, and 1.21
# made GlobalShortcuts refuse a session whose connection has no application id.
# Prism registers as net.prism.Prism, and the portal expects that id to match
# the basename of an installed .desktop file. Without this, global hotkeys fall
# back to focus-bound RegisterHotKey - capture still works.
#
# Writes one file into your home directory. --remove undoes it.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_ID="net.prism.Prism"
DEST_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
DEST="${DEST_DIR}/${APP_ID}.desktop"
TEMPLATE="${ROOT}/packaging/${APP_ID}.desktop"

if [ "${1:-}" = "--remove" ]; then
    if [ -f "$DEST" ]; then
        rm -f "$DEST"
        command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$DEST_DIR" 2>/dev/null || true
        echo "Removed $DEST"
    else
        echo "Nothing to remove: $DEST does not exist"
    fi
    exit 0
fi

[ -f "$TEMPLATE" ] || { echo "missing template: $TEMPLATE" >&2; exit 1; }

# The Exec line is only used if someone launches Prism from the application
# menu; the portal cares that the file exists and its basename matches.
EXEC_LINE="${ROOT}/scripts/run-prism.sh"

mkdir -p "$DEST_DIR"
sed "s|@PRISM_EXEC@|${EXEC_LINE}|" "$TEMPLATE" > "$DEST"
chmod 0644 "$DEST"
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$DEST_DIR" 2>/dev/null || true

echo "Installed $DEST"
echo
sed 's/^/  /' "$DEST"
echo
echo "Restart Prism; the diagnostics window's 'Global shortcuts' section should"
echo "then report the portal state as bound rather than an app-id error."
