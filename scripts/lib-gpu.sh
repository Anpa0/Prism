#!/usr/bin/env bash
# Prism - shared GPU detection helpers
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Everything here keys off PCI addresses read from sysfs. DRM card numbering
# (card0, card1) depends on probe order and can change between boots, so it is
# reported but never used to identify a device.

# PRISM_SYSFS_ROOT relocates the sysfs and /dev lookups, so the detection logic
# can be exercised against a recorded tree. Unset in normal use; the C bridge
# honours the same variable.
PRISM_SYSFS_ROOT="${PRISM_SYSFS_ROOT:-}"

# prism_gpus: prints one line per display-class PCI device, tab separated:
#   pci_address  vendor_id  device_id  driver  card  render  by_path
#         cur_speed  cur_width  max_speed  max_width  boot_vga  name
prism_gpus() {
    local dev pci class vendor device driver card render by_path
    local cur_speed cur_width max_speed max_width boot_vga name

    for dev in "$PRISM_SYSFS_ROOT"/sys/bus/pci/devices/*/; do
        class=$(cat "$dev/class" 2>/dev/null || echo 0)
        case "$class" in
            0x03*) ;;
            *) continue ;;
        esac

        pci=$(basename "$dev")
        # sysfs writes these as 0x1002 / 0x744c; take the low 16 bits either way.
        vendor=$(sed 's/^0x//' "$dev/vendor" 2>/dev/null | tail -c 5)
        device=$(sed 's/^0x//' "$dev/device" 2>/dev/null | tail -c 5)
        driver=$(sed -n 's/^DRIVER=//p' "$dev/uevent" 2>/dev/null)
        [ -n "$driver" ] || driver="(none bound)"

        card=$(ls -1 "$dev/drm" 2>/dev/null | grep -E '^card[0-9]+$' | head -1)
        render=$(ls -1 "$dev/drm" 2>/dev/null | grep -E '^renderD[0-9]+$' | head -1)
        [ -n "$card" ] || card="-"
        [ -n "$render" ] || render="-"

        by_path="-"
        [ -e "$PRISM_SYSFS_ROOT/dev/dri/by-path/pci-$pci-card" ] && by_path="/dev/dri/by-path/pci-$pci-card"

        cur_speed=$(cat "$dev/current_link_speed" 2>/dev/null || echo "?")
        cur_width=$(cat "$dev/current_link_width" 2>/dev/null || echo "?")
        max_speed=$(cat "$dev/max_link_speed" 2>/dev/null || echo "?")
        max_width=$(cat "$dev/max_link_width" 2>/dev/null || echo "?")
        boot_vga=$(cat "$dev/boot_vga" 2>/dev/null || echo 0)

        name=$(prism_pci_name "$vendor" "$device")

        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$pci" "$vendor" "$device" "$driver" "$card" "$render" "$by_path" \
            "$cur_speed" "$cur_width" "$max_speed" "$max_width" "$boot_vga" "$name"
    done
}

# prism_pci_name <vendor_hex> <device_hex> - resolve via hwdata, fall back to IDs.
prism_pci_name() {
    local vendor="$1" device="$2" ids name=""
    for ids in /usr/share/hwdata/pci.ids /usr/share/misc/pci.ids /usr/share/pci.ids; do
        [ -r "$ids" ] || continue
        name=$(awk -v v="$vendor" -v d="$device" '
            /^[0-9a-f]{4}/ { invendor = ($1 == v); next }
            invendor && /^\t[0-9a-f]{4}/ {
                line = $0; sub(/^\t/, "", line)
                id = substr(line, 1, 4)
                if (id == d) { sub(/^[0-9a-f]{4}[ \t]+/, "", line); print line; exit }
            }' "$ids")
        [ -n "$name" ] && break
    done
    if [ -z "$name" ]; then
        name="$vendor:$device"
    fi
    printf '%s %s\n' "$(prism_vendor_name "$vendor")" "$name"
}

prism_vendor_name() {
    case "$1" in
        1002) echo "AMD" ;;
        10de) echo "NVIDIA" ;;
        8086) echo "Intel" ;;
        *) echo "Unknown" ;;
    esac
}

# prism_find_gpu <amd|nvidia|intel> - prints the first matching GPU line.
prism_find_gpu() {
    local want="$1" vendor
    case "$want" in
        amd) vendor=1002 ;;
        nvidia) vendor=10de ;;
        intel) vendor=8086 ;;
        *) return 1 ;;
    esac
    prism_gpus | awk -F'\t' -v v="$vendor" '$2 == v { print; exit }'
}

# DRI_PRIME wants the by-path form with underscores, e.g. pci-0000_03_00_0.
prism_dri_prime_tag() {
    printf 'pci-%s\n' "$(echo "$1" | tr ':.' '__')"
}
