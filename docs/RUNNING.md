# Running Prism

## Requirements at run time

* A Wayland session with a working XDG ScreenCast portal. On Fedora KDE that is
  `xdg-desktop-portal` plus `xdg-desktop-portal-kde`, both started by the
  session.
* PipeWire running (`systemctl --user status pipewire`).
* Wine 8+ or any recent Proton.
* `d3dcompiler_47.dll` in the prefix. ReShade needs it too, so installing it
  once covers both. Prism still starts without it, using an unscaled copy blit.

## Under Wine

```sh
./scripts/run-prism.sh
```

or by hand:

```sh
cd build/dist
WINEPREFIX="$HOME/.local/share/prism/pfx" \
WINEDLLOVERRIDES="d3dcompiler_47,dxgi=n,b" \
wine Prism.exe
```

`WINEDLLOVERRIDES` is what makes Wine prefer the DLLs sitting next to
`Prism.exe` over its own builtins. Without `dxgi=n,b`, ReShade's proxy
`dxgi.dll` is ignored and the effect chain never loads.

## Under Proton

Proton expects to be invoked with a compatibility data directory:

```sh
STEAM_COMPAT_CLIENT_INSTALL_PATH="$HOME/.steam/steam" \
STEAM_COMPAT_DATA_PATH="$HOME/.local/share/prism/proton" \
WINEDLLOVERRIDES="d3dcompiler_47,dxgi=n,b" \
"$HOME/.steam/steam/steamapps/common/Proton - Experimental/proton" run \
    /path/to/build/dist/Prism.exe
```

`scripts/run-prism.sh --proton /path/to/proton` does the same thing.

Alternatively, add `Prism.exe` to Steam as a non-Steam game, force a Proton
version in its compatibility settings, and put
`WINEDLLOVERRIDES="d3dcompiler_47,dxgi=n,b" %command%` in its launch options.

## Command line

| Flag | Effect |
| --- | --- |
| `--test-pattern` | Feed a synthetic pattern instead of capturing. Validates the renderer and ReShade without a portal or a game. |
| `--gameplay` | Start straight into fullscreen gameplay output. |
| `--monitor=N` | Put gameplay output on monitor N (see the Output Display menu). |
| `--diagnostics` | Open the diagnostics window at startup. |
| `--dump-diagnostics` | Write `prism-diagnostics.txt` next to `Prism.exe` ten seconds after start, and echo it to stderr. This is the file to attach when reporting a problem. |

`run-prism.sh` adds one of its own:

| Flag | Effect |
| --- | --- |
| `--gpu amd\|nvidia\|auto` | Which GPU Prism renders on. Affects Prism's process only — not the compositor, not other applications. |

## Using it

1. **Capture → Select Capture Source…** (or `F2`). KDE shows its own screen
   sharing dialog.
2. Choose a window, a screen, or the whole desktop and confirm.
3. The feed appears. The title bar and tray tooltip show incoming and presented
   frame rates.

Menus:

* **Capture** — source selection, stop, which source kinds to offer the picker
  (any / monitors only / windows only), cursor inclusion.
* **Display** — Original Size, Fit, Stretch, Integer Scale; Windowed,
  Borderless, Fullscreen (`Alt+Enter` toggles, `Esc` leaves fullscreen); V-Sync.
* **Frame Rate** — the consumption ceiling: Unlimited / Match Source, 30, 60,
  90, 120, 144, 165, 240. Prism never fabricates frames to reach these numbers.
* **GPU** — automatic (high-performance adapter) or an explicit adapter.
* **Output Display** — which monitor gameplay output goes fullscreen on, or
  follow the Prism window.
* **View** — Hide Output, toggle Gameplay/Configuration, Configure Global
  Shortcuts, Diagnostics, Save Diagnostics Report, About.

## Gameplay output

**Display → Gameplay Output** puts Prism borderless-fullscreen over the game on
the chosen monitor, on top and click-through: the keyboard and mouse keep driving
the game while you watch Prism's processed image.

| Key | Action |
| --- | --- |
| `Ctrl+Shift+F11` | Toggle gameplay / configuration mode (configuration makes Prism interactive so ReShade's overlay can be used) |
| `Ctrl+Shift+F12` | Hide the gameplay output at once, leaving capture and the tray running |

Both go through the XDG GlobalShortcuts portal so they fire while the game has
focus. See [HOTKEYS.md](HOTKEYS.md) — including the one-time `.desktop` file the
portal needs.

Capture the **game window**, not the monitor: Prism is covering that monitor, so
capturing it feeds Prism its own output.

## Tray operation

Prism keeps a tray icon showing `Prism — Idle`, `Prism — Capturing` or
`Prism — Gameplay Output` with the current rates. Right-click for capture source,
start/stop, show/hide gameplay output, Show Prism, diagnostics, global-shortcut
settings and quit. Double-click toggles the output window.

Hiding the output stops presentation but leaves capture running, so the session
and its counters survive. Bring it back from the tray.

## Settings

`Prism.ini`, written next to `Prism.exe` on exit. It stores the display and
window mode, frame ceiling, V-Sync, cursor and source-type preferences, adapter
index, target monitor, hotkey triggers, window size and diagnostics visibility. It is separate from `ReShade.ini`
and the two never interact.

## Logging

Both halves log to stderr, so they interleave:

```sh
wine Prism.exe 2>&1 | tee prism.log
```

`[Prism]` lines come from the executable, `[PrismCapture]` from the bridge. For
per-frame detail, rebuild the bridge with `make -C bridge debug`.
