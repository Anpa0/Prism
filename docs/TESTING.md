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

## 10. Shutdown

Quit from the tray and from the window; both should close cleanly with no
lingering `Prism.exe`, and `Prism.ini` should be written. A capture session left
running at exit should close its portal session — KDE's "screen is being shared"
indicator should clear.
