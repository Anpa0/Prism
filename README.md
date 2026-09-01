# Prism

A Windows/Proton screen-capture host for Linux, with ReShade support.

Prism is a Windows application that runs under Proton or Wine on Linux. It
captures a Linux window, game window, monitor or the whole desktop through the
normal Wayland screen-capture path — KWin, the XDG ScreenCast portal, PipeWire —
presents those frames through a Direct3D 11 swap chain, and lets ReShade,
installed alongside `Prism.exe` in the ordinary Windows way, process that
presentation.

```
Captured game / application
        │
    KDE Plasma / KWin
        │
    XDG ScreenCast Portal          (the desktop's own source picker)
        │
      PipeWire
        │
  PrismCapture.dll                 (native Linux bridge, loaded by Prism)
        │
     Prism.exe                     (running under Proton/Wine)
        │
  Direct3D 11 texture
        │
  Direct3D 11 swap chain
        │
      ReShade                      (attached to Prism.exe, nothing else)
        │
   Prism's window                  (or fullscreen gameplay output over the game)
```

## What Prism is not

Prism is only a capture host. It does not record to disk, encode, stream, buffer
clips, capture audio, upscale, generate frames, or do any kind of AI processing.
It is a live capture-and-presentation application, not a recorder.

It also never touches the application it is capturing. There is no injection, no
API hooking, no process or memory access, and nothing is installed into a game's
directory. The only channel to the source is the compositor's own screen-capture
interface, which the user explicitly authorises through the portal dialog.
ReShade exists inside `Prism.exe` and nowhere else.

## Quick start

```sh
# Build both halves (see docs/BUILDING.md for dependencies)
./scripts/build.sh

# Run it
./scripts/run-prism.sh
```

Then: **Capture → Select Capture Source…**, pick a window or monitor in KDE's
dialog, and the feed appears in Prism's window. Drop ReShade's `dxgi.dll` next
to `Prism.exe` and its effects apply to that feed.

To check the renderer and ReShade before wiring up capture at all:

```sh
./scripts/run-prism.sh --test-pattern
```

## Playing while watching Prism

**Display → Gameplay Output** puts Prism borderless-fullscreen over the game,
on top and click-through, so the keyboard and mouse keep driving the game.

| Key | Action |
| --- | --- |
| `Ctrl+Shift+F11` | Toggle gameplay ↔ configuration mode (configuration makes Prism interactive for ReShade) |
| `Ctrl+Shift+F12` | Hide the gameplay output immediately; capture keeps running |

Both arrive through the XDG GlobalShortcuts portal, so they fire while the game
has focus. One-time setup:

```sh
./scripts/install-desktop-file.sh
```

## Two GPUs

Prism can render on a second GPU while the game stays on the first, because the
ScreenCast stream is the only thing between them — Prism never touches the
game's device.

```sh
./scripts/check-gpus.sh                    # read-only report
./scripts/configure-gpu-policy.sh          # --status first; --apply pins KWin to AMD
./scripts/run-prism.sh --gpu nvidia        # Prism only, this launch only
```

None of it is required: with one GPU, Prism runs on it with no configuration.

## Documentation

| Document | Contents |
| --- | --- |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Design, the D3D11 decision, how `Prism.exe` talks to the native `.so`, portal integration, latest-frame semantics, gameplay/configuration modes |
| [docs/FEDORA_SETUP.md](docs/FEDORA_SETUP.md) | Fedora/KDE prerequisites, NVIDIA Blackwell driver install, Secure Boot, PCIe checks |
| [docs/GPU_POLICY.md](docs/GPU_POLICY.md) | Keeping the session on AMD and Prism opt-in on NVIDIA |
| [docs/MULTI_GPU.md](docs/MULTI_GPU.md) | The dual-GPU topology, adapter selection, display output, OCuLink bandwidth |
| [docs/PROTON.md](docs/PROTON.md) | Adding Prism to Steam, launch options, prefix layout |
| [docs/HOTKEYS.md](docs/HOTKEYS.md) | Global shortcuts, the portal app-id requirement, troubleshooting |
| [docs/RESHADE.md](docs/RESHADE.md) | ReShade installation and configuration |
| [docs/BUILDING.md](docs/BUILDING.md) | Directory layout, dependencies, build system |
| [docs/RUNNING.md](docs/RUNNING.md) | Launching under Wine and Proton, menus, settings |
| [docs/TESTING.md](docs/TESTING.md) | Phase-by-phase test procedure, including the isolation check |
| [docs/ROADMAP.md](docs/ROADMAP.md) | Performance-optimisation roadmap (DMA-BUF and friends) |

## Reporting a problem

```sh
./scripts/run-prism.sh --dump-diagnostics
```

writes `build/dist/prism-diagnostics.txt` — capture state, negotiated format and
stride, frame accounting, per-stage timings, the DXGI adapter in use, ReShade
detection, global-shortcut state, and every GPU the bridge found with its PCI
address, driver and PCIe link. Attach that, plus `./scripts/check-gpus.sh`.

## Status

Version 0.2. Implemented: bridge load, ScreenCast portal session, PipeWire
stream with full buffer validation, latest-frame mailbox, D3D11 present with
format-matched textures, ReShade attach, fullscreen gameplay output with
click-through input, global shortcuts through the GlobalShortcuts portal,
explicit GPU selection, monitor targeting, tray operation and diagnostics.

Frame transport is CPU-side by design for this version; see the roadmap for the
zero-copy work that follows.

## Licence

GPL-3.0-or-later. The portal and PipeWire code is derived from ShaderGlass's
WineCap and, through it, from OBS Studio's screencast portal implementation
(© Georges Basile Stavracas Neto, GPL-2.0-or-later). See [LICENSE](LICENSE).
