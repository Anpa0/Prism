# GPU policy

The goal, stated once:

```
KWin, Plasma, Steam, native games, Proton games, browsers, everything   ->  RX 7900 XT
Prism, and only Prism                                                   ->  RTX 5050
```

The NVIDIA card being installed must not make it the default for anything. It is
opt-in, per launch, for one application.

## Why the usual advice does not apply

Most multi-GPU documentation on the web describes X11 PRIME offload on a laptop:
an integrated GPU driving the display and a discrete GPU rendering on demand.
This machine is neither — two full desktop GPUs, a Wayland session, and KWin
choosing its own render device. `xrandr --setprovideroffloadsink` and friends are
X11-only and irrelevant here.

The mechanisms that do apply on Fedora KDE Plasma Wayland:

| Layer | Mechanism | Scope |
| --- | --- | --- |
| Compositor | `KWIN_DRM_DEVICES` | KWin's render device, session-wide |
| Vulkan (all ICDs) | `MESA_VK_DEVICE_SELECT`, `VK_DEVICE_SELECT_PCI_BUS_ID` | per process |
| OpenGL (Mesa) | `DRI_PRIME` | per process |
| OpenGL/Vulkan (NVIDIA) | `__NV_PRIME_RENDER_OFFLOAD`, `__GLX_VENDOR_LIBRARY_NAME`, `__VK_LAYER_NV_optimus` | per process |
| DXVK | `DXVK_FILTER_DEVICE_NAME` | per process |
| Prism | its own **GPU** menu (DXGI adapter selection) | per process |

`MESA_VK_DEVICE_SELECT` is worth a note: despite the name it is implemented by
`VK_LAYER_MESA_device_select`, a Vulkan **loader** layer, so it reorders the
physical devices of every ICD including NVIDIA's. Appending `!` makes the
selection exclusive rather than merely preferred.

## Never key off card numbers

`card0` and `card1` are assigned in probe order and can swap between boots. On
the very tree this was developed against, `card0` was the NVIDIA GPU and `card1`
the AMD one — the opposite of the intuitive guess.

Everything in Prism keys off the **PCI address** instead, and uses the stable
symlinks under `/dev/dri/by-path/`:

```
/dev/dri/by-path/pci-0000:03:00.0-card
/dev/dri/by-path/pci-0000:03:00.0-render
```

`./scripts/check-gpus.sh` prints both, along with the PCI address to use.

## The one session-wide change

Pinning KWin to the AMD card is the only setting that needs to persist:

```sh
./scripts/check-gpus.sh              # see what is present
./scripts/configure-gpu-policy.sh    # --status: shows the proposed file, changes nothing
./scripts/configure-gpu-policy.sh --apply
```

It writes exactly one file:

```
~/.config/plasma-workspace/env/10-prism-gpu-policy.sh
    export KWIN_DRM_DEVICES=/dev/dri/by-path/pci-0000:03:00.0-card
```

Plasma sources that directory at session start, so it takes effect at your next
login. Nothing outside your home directory is touched, no root is needed, and
`--restore` puts back the previous file or removes Prism's if there was none.
Every apply backs up what it replaced under `~/.local/share/prism/backups/`.

Verify after logging back in:

```sh
./scripts/check-gpus.sh | grep 'KWin render devices'
# or directly:
tr '\0' '\n' < /proc/$(pidof kwin_wayland)/environ | grep KWIN_DRM_DEVICES
```

## Prism's opt-in

Nothing persistent. `run-prism.sh` exports the selection into Prism's process
only:

```sh
./scripts/run-prism.sh --gpu nvidia     # Prism renders on the NVIDIA card
./scripts/run-prism.sh --gpu amd        # Prism renders on the AMD card
./scripts/run-prism.sh                  # session default; correct while single-GPU
```

`--gpu nvidia` sets, for that process:

```
MESA_VK_DEVICE_SELECT=10de:2d18!
VK_DEVICE_SELECT_PCI_BUS_ID=0000:0d:00.0
DRI_PRIME=pci-0000_0d_00_0
__NV_PRIME_RENDER_OFFLOAD=1
__GLX_VENDOR_LIBRARY_NAME=nvidia
__VK_LAYER_NV_optimus=NVIDIA_only
```

Prism's own **GPU** menu is still the final word: DXGI enumerates whatever
Vulkan exposes, and Prism picks an adapter from that list by vendor and device
ID rather than trusting adapter 0. Both layers agreeing is what the diagnostics
window is for.

## Steam titles are untouched

A normal Steam game inherits the session default, which after the KWin pin is
the AMD card. Prism gets its override through its own launch options (see
[PROTON.md](PROTON.md)). There is deliberately no global GPU override, so
nothing has to be undone per game.

## Confirming which GPU Prism actually used

Three independent sources should agree:

```sh
# 1. What Prism thinks
./scripts/run-prism.sh --gpu nvidia --test-pattern --dump-diagnostics
#    -> build/dist/prism-diagnostics.txt, "Renderer" and "System" sections

# 2. What the driver thinks
nvidia-smi                # Prism.exe / wine should appear under Processes

# 3. What the kernel thinks
./scripts/check-gpus.sh   # link state, drivers, DRM nodes
```

The diagnostics report cross-references them itself: each detected GPU is marked
`Used by Prism: Yes/No` by matching the PCI vendor/device ID against the adapter
DXGI handed Prism.

## Single-GPU operation

Everything above is optional. With only the RX 7900 XT installed, Prism runs on
it with no configuration at all, and every feature except the dual-GPU split is
testable. `configure-gpu-policy.sh --apply` is harmless in that state and makes
the policy explicit before the second card arrives.
