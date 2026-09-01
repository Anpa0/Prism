# Prism

A screen-capture host for Proton/Wine, with ReShade support.

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
   Prism's window
```

## What Prism is not

Prism is only a capture host. It does not record, encode, stream, buffer clips,
capture audio, upscale, generate frames, or do any kind of AI processing.

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

## Documentation

| Document | Contents |
| --- | --- |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | Design, the D3D11 vs D3D12 decision, how `Prism.exe` talks to the native `.so`, PipeWire and portal integration, latest-frame semantics |
| [docs/BUILDING.md](docs/BUILDING.md) | Directory layout, dependencies, build system, Fedora instructions |
| [docs/RUNNING.md](docs/RUNNING.md) | Launching under Wine and under Proton, environment variables, settings |
| [docs/RESHADE.md](docs/RESHADE.md) | ReShade installation and configuration |
| [docs/TESTING.md](docs/TESTING.md) | Test procedure, including the isolation check |
| [docs/ROADMAP.md](docs/ROADMAP.md) | Performance-optimisation roadmap (DMA-BUF and friends) |

## Status

Version 0.1. The bring-up path — bridge load, portal session, PipeWire stream,
latest-frame mailbox, D3D11 present, ReShade attach, diagnostics, tray — is
implemented. Frame transport is CPU-side by design for this version; see the
roadmap for the zero-copy work that follows.

## Licence

GPL-3.0-or-later. The portal and PipeWire code is derived from ShaderGlass's
WineCap and, through it, from OBS Studio's screencast portal implementation
(© Georges Basile Stavracas Neto, GPL-2.0-or-later). See [LICENSE](LICENSE).
