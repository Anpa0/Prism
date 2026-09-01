# Testing Prism

The order matters: each stage removes a variable from the next.

## 1. The build is what it claims to be

```sh
file build/dist/Prism.exe        # PE32+ executable (GUI) x86-64
file build/dist/PrismCapture.dll # ELF 64-bit LSB shared object, x86-64
nm -D --defined-only build/dist/PrismCapture.dll | grep -c PrismCapture  # 7
```

An ELF `PrismCapture.dll` is correct — it is a Winelib module, not a PE.

## 2. Renderer, without capture

```sh
./scripts/run-prism.sh --test-pattern --diagnostics
```

Expect a window showing eight colour bars over a grey gradient with a white band
scrolling down it, and in the log:

```
[Prism] renderer: D3D11 device on '<your GPU>' (feature level 0xb100, flip=1, tearing=1)
[Prism] renderer: using d3dcompiler_47.dll for shader compilation
[Prism] renderer: capture texture is now 1280x720
```

Check that the red bar is red. If red and blue are swapped, the channel order is
wrong somewhere. Then cycle **Display → Original Size / Fit / Stretch / Integer
Scale** and resize the window: Fit should letterbox and keep the bars square,
Stretch should distort them, Integer Scale should snap.

`flip=1` means the flip-model swap chain was accepted. `flip=0` is a fallback,
not a failure.

## 2a. Global shortcuts, without a compositor

The portal handshake is a D-Bus conversation, not a key event, so it can be
driven by a stand-in. This catches the whole path — app-id registration, session
creation, binding, and the activation callback reaching the UI thread — without
a KDE session:

```sh
dbus-run-session -- sh -c \
  'python3 tests/mock_globalshortcuts_portal.py & sleep 2; \
   cd build/dist && wine ./Prism.exe --test-pattern --gameplay'
```

Expect, in order:

```
Registry.Register('net.prism.Prism') from :1.2
[PrismCapture] registered with the host portal as 'net.prism.Prism'
[PrismCapture] GlobalShortcuts portal version 2
CreateSession -> /org/freedesktop/portal/desktop/session/1_2/prismsession1
  bind 'toggle-mode' ... trigger='CTRL+SHIFT+F11'
  bind 'hide-output' ... trigger='CTRL+SHIFT+F12'
[PrismCapture] global shortcuts: bound by the compositor
*** Activated toggle-mode ***
[Prism] configuration mode: Prism is interactive, ...
*** Activated hide-output ***
[Prism] gameplay output off; capture continues
```

Requires `python3-dbus` and `python3-gi`. The mock never touches a real session
bus: `dbus-run-session` gives it a private one.

## 3. ReShade, still without capture

Install ReShade per [RESHADE.md](RESHADE.md), then run the test pattern again.

* **View → Diagnostics** reports `ReShade → Loaded: Yes`.
* `Home` opens ReShade's overlay.
* Enable `Monochrome.fx`: the colour bars go grey.

Getting this far proves ReShade hooks Prism's renderer, with no capture, no
portal and no game involved.

## 4. The bridge loads

```sh
./scripts/run-prism.sh
```

```
[PrismCapture] ScreenCast portal ready (sources mask 0x3)
[Prism] bridge: PrismCapture.dll loaded, ABI 1
```

Failure modes and what they mean:

| Log | Cause |
| --- | --- |
| `LoadLibraryW failed` | The bridge is not next to `Prism.exe`, or you are not under Wine/Proton |
| `cannot reach the session bus` | No `DBUS_SESSION_BUS_ADDRESS` reaching the prefix |
| `ScreenCast portal unavailable` | `xdg-desktop-portal` or its KDE backend is not running |
| `reports ABI n but Prism.exe expects m` | The two halves are from different builds |

## 4a. GPUs and the system report

Read-only, changes nothing:

```sh
./scripts/check-gpus.sh
```

It should list every GPU with its PCI address, bound driver, DRM nodes, stable
`/dev/dri/by-path` name and PCIe link state, and flag any link running below its
maximum. Cross-check against Prism's own view:

```sh
./scripts/run-prism.sh --test-pattern --dump-diagnostics
cat build/dist/prism-diagnostics.txt
```

The `System (via the capture bridge)` section should agree with the script, and
mark exactly one GPU `Used by Prism: Yes`.

## 5. Live capture

1. **Capture → Select Capture Source…**
2. KDE's screen-sharing dialog appears. This is the desktop's own dialog, not
   Prism's — Prism never enumerates windows itself.
3. Pick a monitor first (simpler than a window: no crop rectangle involved).

```
[PrismCapture] ScreenCast session created
[PrismCapture] Source selected, PipeWire node 63
[PrismCapture] stream format: 2560x1440 BGRx @ 0/1
[PrismCapture] stream state: streaming
[Prism] renderer: capture texture is now 2560x1440
```

Then repeat with a window source, and with a fullscreen game, and check:

* aspect ratio is preserved in Fit mode for 16:9, 16:10, 4:3 and ultrawide
  sources;
* a window source is not padded or offset (that is the crop rectangle working);
* resizing Prism's window does not distort the image except in Stretch.

## 6. Frame rate and pacing

With a high-refresh source running, open the diagnostics window and set
**Frame Rate → Unlimited / Match Source**.

* `Incoming FPS` should track the source's real unique-frame rate — 93 for a
  93 fps source, not 60 and not 120.
* `Presented FPS` should sit close behind it.
* `Dropped as stale` should stay near zero while the renderer keeps up.

Now set the ceiling to 60. `Incoming FPS` should fall to roughly 60 and
`Dropped at bridge` should climb — the ceiling limits consumption. Set it back
to 120 with a 93 fps source: incoming stays at 93. **Prism must never report a
presented rate above the source's unique-frame rate.** If it does, something is
duplicating frames, which is a bug.

## 7. Latest-frame behaviour

Load the machine (run something GPU-heavy, or set a very low ceiling) so the
renderer falls behind. `Dropped as stale` should climb while
`Capture → present` latency stays roughly flat. Latency that grows without bound
means frames are queueing somewhere, which the mailbox is specifically designed
to prevent.

## 7a. Gameplay output and input routing

The part that most needs a real KDE session.

```sh
# Start a game, then, in another terminal:
./scripts/run-prism.sh
```

1. Capture the **game window** — not the monitor. Prism is about to cover that
   monitor, and capturing it would feed Prism its own output.
2. **Display → Gameplay Output** (or launch with `--gameplay`).

Then check, in order:

| Check | Expected |
| --- | --- |
| Prism covers the monitor, borderless, above the game | yes |
| Typing and mouse-look still drive the game | yes — Prism is click-through and non-activating |
| Clicking Prism's image | nothing happens; the game keeps focus |
| `Ctrl+Shift+F11` | Prism becomes interactive, ReShade's overlay key works |
| `Ctrl+Shift+F11` again | input returns to the game |
| `Ctrl+Shift+F12` | the overlay disappears at once; capture keeps running; the tray icon stays |
| Tray → Show Gameplay Output | it comes back |

If the hotkeys do nothing, read the `Global shortcuts` section of the
diagnostics report before anything else. `RegisterHotKey: armed` there means the
portal was not used, and [HOTKEYS.md](HOTKEYS.md) explains why.

If Prism steals focus or swallows clicks, that is KWin disagreeing with Wine's
mapping of `WS_EX_NOACTIVATE` / `WS_EX_TRANSPARENT`. Report which of the two
misbehaves — they fail independently.

## 8. ReShade on the live feed

With capture running and `Monochrome.fx` enabled:

* Prism's window is greyscale;
* the captured game's own window is still in full colour;
* toggling the effect changes only Prism.

## 9. Isolation check — the non-negotiable one

While capturing a game, confirm Prism has not touched it.

```sh
GAME_PID=$(pgrep -f YourGame | head -1)

# Nothing of Prism's is mapped into the game
grep -icE 'reshade|prism|dxgi' /proc/$GAME_PID/maps        # expect 0

# Nothing has been written into the game's directory
find /path/to/game -newermt '-1 hour' -type f              # expect nothing new

# Prism holds no handle on the game
ls -l /proc/$(pgrep -f Prism.exe | head -1)/fd 2>/dev/null | grep -c "$GAME_PID"  # expect 0
```

Prism's own process should show a D-Bus socket and a PipeWire socket, and
nothing that reaches the game. That is the entire attack surface by design.

Broaden it if you like — the answer should stay empty:

```sh
# No ptrace relationship
grep -i tracerpid /proc/$GAME_PID/status          # expect 0

# Prism opened no handle on the game
sudo ls -l /proc/$(pgrep -f Prism.exe | head -1)/fd | grep -c "$GAME_PID"   # expect 0

# The game's directory is untouched
find /path/to/game -newermt '-1 hour'             # expect nothing
```

## 9a. Dual-GPU (once the RTX 5050 is installed)

```sh
# 1. The session is on AMD
./scripts/check-gpus.sh | grep 'KWin render devices'

# 2. Prism is on NVIDIA
./scripts/run-prism.sh --gpu nvidia --dump-diagnostics
grep -B4 'Used by Prism: Yes' build/dist/prism-diagnostics.txt

# 3. The driver agrees
nvidia-smi          # Prism.exe should appear; the game should not

# 4. The link is what it should be
sudo lspci -vv -s <nvidia-pci> | grep -E 'LnkCap|LnkSta'
```

Then capture a game running on the AMD card and confirm the feed is correct.
Nothing about the game changes: it never learns which GPU is watching it.

## 9b. NVIDIA display output

With the monitor on the NVIDIA card's DisplayPort:

```sh
kscreen-doctor -o                        # find the output
./scripts/run-prism.sh --gpu nvidia --gameplay --monitor=1
```

Prism's **Output Display** menu lists the same monitors. The final frame should
be rendered and scanned out by NVIDIA while the game stays on AMD.

## 10. Shutdown

Quit from the tray and from the window; both should close cleanly with no
lingering `Prism.exe`, and `Prism.ini` should be written. A capture session left
running at exit should close its portal session — KDE's "screen is being shared"
indicator should clear.
