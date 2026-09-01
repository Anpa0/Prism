# Fedora setup

Target: Fedora Linux, KDE Plasma, Wayland, PipeWire, x86-64.

## Verify the session is what Prism expects

```sh
echo "$XDG_SESSION_TYPE"        # wayland
echo "$XDG_CURRENT_DESKTOP"     # KDE
systemctl --user status pipewire xdg-desktop-portal xdg-desktop-portal-kde
```

All three services should be running. `xdg-desktop-portal-kde` is the piece that
draws the screen-sharing dialog and delivers global shortcuts; without it Prism
loads but reports no ScreenCast portal.

```sh
# Which portal version is in play - relevant to the app-id requirement
rpm -q xdg-desktop-portal xdg-desktop-portal-kde
```

## Build dependencies

```sh
sudo dnf install -y \
    mingw64-gcc mingw64-gcc-c++ mingw64-headers \
    cmake make git \
    wine-devel wine-core \
    pipewire-devel glib2-devel \
    vulkan-tools
```

`mingw64-gcc-c++` supplies the cross compiler and the `d3d11.h` / `dxgi1_6.h`
headers; no Windows SDK is involved. `wine-devel` supplies `winegcc`, which
builds the Winelib capture bridge.

Then:

```sh
./scripts/build.sh
./scripts/run-prism.sh --test-pattern
```

## One-time application registration

Global hotkeys need Prism to have an application id the portal recognises:

```sh
./scripts/install-desktop-file.sh
```

See [HOTKEYS.md](HOTKEYS.md) for why. Capture works without it; hotkeys do not.

## Installing the NVIDIA driver (only when the second GPU arrives)

Skip this entirely while running single-GPU on the RX 7900 XT.

The RTX 5050 is **Blackwell**. NVIDIA's proprietary kernel modules do not
support Blackwell at all — the **open** kernel modules are the only option. Do
not install `akmod-nvidia`; install the `-open` variant.

Package names and the state of RPM Fusion's prebuilt modules move, so check what
your Fedora release actually offers before committing:

```sh
dnf search nvidia | grep -Ei 'akmod|kmod|open'
dnf info akmod-nvidia-open xorg-x11-drv-nvidia-cuda 2>/dev/null | head -40
```

### RPM Fusion

```sh
sudo dnf install -y \
  "https://mirrors.rpmfusion.org/free/fedora/rpmfusion-free-release-$(rpm -E %fedora).noarch.rpm" \
  "https://mirrors.rpmfusion.org/nonfree/fedora/rpmfusion-nonfree-release-$(rpm -E %fedora).noarch.rpm"
sudo dnf makecache
```

### The driver

```sh
sudo dnf install -y akmod-nvidia-open xorg-x11-drv-nvidia-cuda
```

`akmod-*` rebuilds the module against each new kernel automatically. RPM Fusion
has been moving toward prebuilt `kmod-nvidia-open` packages; if your release
offers them, they avoid the rebuild wait. Either way **wait for the build to
finish before rebooting**:

```sh
sudo akmods --force
modinfo -F version nvidia
```

Confirm it really is the open module — this is the check that catches the common
mistake of ending up with the proprietary one on a Blackwell card:

```sh
modinfo nvidia | grep -i license      # expect MIT/GPL, not NVIDIA proprietary
```

### Secure Boot

akmod-built modules are unsigned, and a Secure Boot system will refuse to load
them. Either disable Secure Boot in firmware, or enrol a Machine Owner Key:

```sh
sudo dnf install -y kmodtool akmods mokutil openssl
sudo kmodgenca -a
sudo mokutil --import /etc/pki/akmods/certs/public_key.der
sudo reboot          # complete the MOK enrolment in the blue firmware screen
```

Verify afterwards:

```sh
mokutil --sb-state
lsmod | grep -E '^nvidia'
```

### DRM/KMS

Wayland needs NVIDIA's DRM modeset enabled. Recent driver packages set it by
default; confirm rather than assume:

```sh
cat /sys/module/nvidia_drm/parameters/modeset      # expect Y
```

If it reads `N`, add `nvidia-drm.modeset=1` to the kernel command line:

```sh
sudo grubby --update-kernel=ALL --args="nvidia-drm.modeset=1"
```

### Coexistence with amdgpu

Do not blacklist or remove `amdgpu`, and do not install anything that claims to
"switch" GPUs. Both drivers are meant to be loaded at once:

```sh
lsmod | grep -E '^(amdgpu|nvidia)'
./scripts/check-gpus.sh
```

`check-gpus.sh` shows both cards with their PCI addresses, bound drivers, DRM
nodes and PCIe link state.

### Vulkan sees both

```sh
vulkaninfo --summary | grep -E 'GPU|deviceName|driverName'
```

Two devices should be listed. If only one appears, the ICD for the missing
vendor is not installed — `mesa-vulkan-drivers` for AMD, the NVIDIA driver
package for NVIDIA.

### nvidia-smi

```sh
nvidia-smi
nvidia-smi --query-gpu=name,driver_version,pci.bus_id,pcie.link.gen.current,pcie.link.width.current --format=csv
```

## PCIe link check (OCuLink)

The RTX 5050 is planned to hang off M.2-to-OCuLink, which means PCIe 4.0 x4 at
best and a real chance of training lower. The advertised capability is not the
negotiated link:

```sh
./scripts/check-gpus.sh          # flags a link below its maximum
sudo lspci -vv -s 0000:0d:00.0 | grep -E 'LnkCap|LnkSta'
```

A healthy Gen4 x4 link reads `16GT/s, Width x4`. Seeing `8GT/s` or `Width x2`
means the link trained down — reseat the adapter, check the cable, and confirm
the M.2 slot is CPU-connected rather than chipset-attached.

One caveat: idle GPUs downtrain their link to save power. Re-check under load
before concluding anything is wrong.

## Next

* [GPU_POLICY.md](GPU_POLICY.md) — keeping the session on AMD, Prism on NVIDIA
* [PROTON.md](PROTON.md) — adding Prism to Steam
* [TESTING.md](TESTING.md) — the phase-by-phase test order
