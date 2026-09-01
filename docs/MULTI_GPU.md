# Dual-GPU operation

The end state:

```
        RX 7900 XT                                   RTX 5050
        ──────────                                   ────────
        the game renders here                        Prism renders here
        KWin composites here                         ReShade runs here
        Plasma, Steam, browsers, everything          Prism's monitor output
              │                                            ▲
              │  Wayland surface                           │
              ▼                                            │
           KWin  ──►  XDG ScreenCast  ──►  PipeWire  ──►  PrismCapture  ──►  Prism.exe
```

## Why the split works at all

Prism never talks to the GPU that rendered the source. It has no handle on the
game's device, no shared texture, no cross-vendor import. The ScreenCast stream
is the abstraction boundary: KWin hands PipeWire a frame, and PipeWire hands
Prism bytes. What produced those bytes is not Prism's business.

That is what makes an AMD-rendered game feeding an NVIDIA-rendered Prism a
non-event rather than an integration project. For version 0.1 the frames cross
as CPU-visible buffers, which costs a copy on each side and works between any
two vendors. Zero-copy DMA-BUF import is a later optimisation and is explicitly
not a prerequisite — see [ROADMAP.md](ROADMAP.md).

## What has to be true

1. **KWin stays on AMD.** `KWIN_DRM_DEVICES` pinned to the AMD card's
   `/dev/dri/by-path` name. See [GPU_POLICY.md](GPU_POLICY.md).
2. **The game stays on AMD.** It inherits the session default; nothing is
   overridden for it.
3. **Prism opts into NVIDIA.** Per-launch environment plus Prism's own DXGI
   adapter selection. Nothing persistent, nothing session-wide.
4. **Both drivers coexist.** `amdgpu` and `nvidia` loaded together; neither
   blacklisted.

## Selecting the adapter inside Prism

The **GPU** menu lists every DXGI adapter with its description, and Prism reports
the details it used in diagnostics:

```
Renderer
  GPU:                  NVIDIA GeForce RTX 5050
  Vendor / device:      NVIDIA  10de:2d18
  Dedicated VRAM:       8192 MB
  Adapter LUID:         00000000:0001a3f0
  Adapter index:        1  (explicit)
```

Choosing a different adapter tears down the device and swap chain and rebuilds
them in place, pausing and resuming any live capture session around the rebuild.
ReShade re-initialises with the new swap chain.

Prism never assumes adapter 0. With no explicit choice it asks
`IDXGIFactory6::EnumAdapterByGpuPreference` for the high-performance adapter and
maps the result back to an enumeration index so the menu can show which one is
active.

## Confirming the split is real

Three sources, which should agree:

```sh
# What Prism used
./scripts/run-prism.sh --gpu nvidia --test-pattern --dump-diagnostics
grep -A3 'Used by Prism' build/dist/prism-diagnostics.txt

# What NVIDIA sees running
nvidia-smi

# What the game used - AMD utilisation should move, NVIDIA's should not
sudo dnf install -y radeontop && radeontop
```

The diagnostics report does the cross-referencing itself: every GPU the bridge
found in sysfs is listed with `Used by Prism: Yes/No`, decided by matching PCI
vendor/device IDs against the adapter DXGI actually handed Prism. If Prism says
`Yes` against the NVIDIA card while `radeontop` shows the game loading the AMD
card, the split is working.

## Display output

Both cards can drive DisplayPort. Nothing in Prism assumes otherwise, and no
connector name is hardcoded anywhere.

When the monitor is connected to the NVIDIA card, that output shows up in KDE as
another display, and therefore inside Wine as another Windows monitor. Prism's
**Output Display** menu lists them; pick the NVIDIA-driven one and gameplay
output goes fullscreen there:

```sh
kscreen-doctor -o            # KDE's view: connector names, geometry, enabled state
./scripts/run-prism.sh --gameplay --monitor=1
```

Prism's final frame is produced by whichever GPU Prism renders on, and scanned
out by whichever GPU drives that output. Nothing routes it back through the AMD
card deliberately.

Turning the NVIDIA-connected output off when Prism is not in use is a KDE
operation, not a Prism one:

```sh
kscreen-doctor --outputs                     # find the real connector name first
kscreen-doctor output.DP-3.disable           # substitute the actual name
kscreen-doctor output.DP-3.enable
```

Prism deliberately ships no script for this. Connector names differ per machine,
and disabling an output is exactly the kind of change that should be a
deliberate command rather than a side effect of launching a capture tool.

## PCIe over OCuLink

The RTX 5050 is planned to arrive over M.2 → OCuLink → external dock, so PCIe
4.0 x4 is the ceiling. What matters is what the link *trained* at, which the
adapter itself will never tell you:

```sh
./scripts/check-gpus.sh      # flags any link below its maximum
sudo lspci -vv -s <pci> | grep -E 'LnkCap|LnkSta'
```

Healthy: `16GT/s, Width x4`. `8GT/s` is Gen3; `Width x2` is half the lanes. Both
are worth chasing before blaming Prism for latency. Note that an idle GPU
downtrains deliberately — re-check under load.

Bandwidth matters here in one direction only. Prism uploads captured frames from
system memory to the NVIDIA card every frame: 2560×1440 at 32bpp is ~14.7 MB per
frame, so 240 fps needs roughly 3.5 GB/s. Gen4 x4 is about 7.9 GB/s, so a
correctly trained link has headroom; Gen3 x2 (about 1.97 GB/s) does not.

## Single-GPU fallback

None of this is required. With only the RX 7900 XT, Prism runs on it and every
feature except the split is testable — capture, ReShade, gameplay output,
hotkeys, diagnostics, display targeting. Performance in that mode is not
representative of the final configuration, because the game and Prism contend
for one GPU.
