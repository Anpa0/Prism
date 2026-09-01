# Prism architecture

## 1. Recommended architecture

One process, two halves, no daemon and no IPC.

```
                     Prism.exe  (PE64, MinGW-built, runs under Proton/Wine)
                     ├── App            window, menus, tray, modes, main loop
                     ├── Renderer       D3D11 device, swap chain, present
                     ├── FrameMailbox   latest-frame triple buffer
                     ├── Hotkeys        portal shortcuts + RegisterHotKey fallback
                     ├── Diagnostics    counters, ReShade and GPU reporting
                     └── CaptureBridge  LoadLibraryW + GetProcAddress
                                │
                                │  direct function calls, same address space
                                ▼
                     PrismCapture.dll  (Winelib ELF, winegcc-built)
                     ├── prism_bridge.c  ABI surface, capture thread
                     ├── screencast.c    XDG ScreenCast portal driver
                     ├── shortcuts.c     XDG GlobalShortcuts portal driver
                     ├── sysinfo.c       PCI/DRM/PCIe introspection from sysfs
                     ├── portal.c        Request helper + host app-id registration
                     └── pipewire.c      stream negotiation and frame arrival
                                │
                                ▼
                     libpipewire-0.3, GIO/GDBus  (native Linux, unmodified)
```

`PrismCapture.dll` is not a Windows DLL. It is an ELF shared object built by
`winegcc` from a `.spec` file, carrying a PE-style export table that Wine's
loader understands. Prism loads it with `LoadLibraryW()` like any other DLL. The
same object links against the system's real `libpipewire-0.3` and `libgio-2.0`,
which is what lets a Windows process speak D-Bus and PipeWire without any
marshalling layer at all.

On real Windows the load simply fails and Prism reports that no capture bridge
is available. That is the intended behaviour: Prism is a Linux application that
happens to be shaped like a Windows executable.

### Why no helper process

A separate Linux daemon plus a socket would mean serialising every frame, a
second copy on each side, an extra scheduling hop per frame, and a lifecycle to
manage (start, crash, restart, orphan cleanup). The Winelib bridge removes all
of it: the PipeWire callback runs on a thread inside `Prism.exe` and calls the
frame handler directly. The user launches one binary.

The pattern is the one ShaderGlass uses for its WineCap module, and it is proven
in the field on KDE and GNOME.

## 2. D3D11 vs D3D12 — recommendation: **Direct3D 11**

| Criterion | D3D11 | D3D12 |
| --- | --- | --- |
| Proton/Wine translation | DXVK, the most exercised path in Proton | VKD3D-Proton, newer and more sensitive to feature levels |
| ReShade support | Oldest, most mature backend; `dxgi.dll` proxy is the standard install | Supported, but the D3D12 backend sees far less use and more edge cases |
| Presenting a CPU-uploaded texture | `USAGE_DYNAMIC` + `Map(WRITE_DISCARD)`, three lines | Upload heap, fences, per-frame resource lifetime tracking |
| High-framerate presentation | Flip model with tearing, no manual synchronisation | Same, but the caller owns all synchronisation |
| Code volume for this job | ~600 lines | Roughly triple, with no functional gain |

Prism's entire GPU workload is one texture upload and one fullscreen triangle
per frame. D3D12's advantages — explicit multithreaded command recording, fine
grained residency, low driver overhead on draw-call-heavy scenes — are all
irrelevant here, and every one of its costs is real. The API that makes Prism
easiest to build and ReShade easiest to support is D3D11, so that is the choice.

`Prism.exe` creates a `DXGI_FORMAT_B8G8R8A8_UNORM` flip-model swap chain, which
matches PipeWire's `BGRx` byte-for-byte and is the format ReShade handles best.

## 3. The capture bridge

`PrismCapture.dll` exposes seven `__stdcall` functions, declared once in
`include/PrismCapture.h` and compiled by both sides:

| Export | Purpose |
| --- | --- |
| `PrismCaptureVersion` | ABI check; Prism refuses a mismatched bridge |
| `PrismCaptureInit` | Connect to the session bus and the ScreenCast portal, start the capture thread |
| `PrismCaptureStart` | Open the portal picker and begin streaming |
| `PrismCaptureStop` | Close the stream and the portal session |
| `PrismCaptureSetMaxFps` | Change the delivery ceiling of a live session |
| `PrismCaptureGetStats` | Lock-free counters for the diagnostics panel |
| `PrismCaptureShutdown` | Stop the capture thread before the process exits |
| `PrismShortcutsStart` | Create a GlobalShortcuts session and bind Prism's two actions |
| `PrismShortcutsGetStatus` / `GetBindings` | What the compositor decided |
| `PrismShortcutsConfigure` / `Stop` | Open the compositor's editor; tear down |
| `PrismSystemInfoQuery` | GPUs, PCI addresses, kernel drivers, PCIe link state |

### Threading

The bridge runs exactly one thread of its own, created with **`CreateThread`,
not `pthread_create`**. That matters: a Win32 thread is registered with Wine's
PE loader, so it is legal for code on that thread to call back into
`Prism.exe`. A raw pthread would not be, and the first callback would fault.

That thread runs a GLib main loop with the PipeWire loop attached as a
`GSource`, so D-Bus replies and frame arrivals are serialised onto one thread
with no locking between them.

### What crosses the boundary

The frame callback hands over a `PrismFrame`: a pointer into PipeWire's mapped
buffer, geometry, pixel format, the PipeWire presentation stamp, the bridge's
own arrival timestamp, and a sequence number. The pointer is valid only for the
duration of the call — PipeWire recycles the buffer as soon as it returns — so
`CaptureBridge::OnFrame` does exactly one thing: copy it into the mailbox.
Direct3D is never touched from the capture thread.

### Loaded is not the same as capture-ready

The module being usable and the ScreenCast portal being available are tracked
separately. A session with no ScreenCast backend still gets working global
shortcuts, system diagnostics and the test pattern; only capture is disabled.
Collapsing the two would mean one missing portal took out the hotkeys as well,
which is how the coupling was found — see [TESTING.md](TESTING.md).

### Module lifetime

Once `PrismCaptureInit` has run, the bridge is never `FreeLibrary`'d, even when
initialisation fails. GDBus starts a worker thread the moment it touches the
session bus, and unmapping the shared object out from under that thread faults
the process. The module stays mapped and inert instead; `CaptureBridge` tracks
usability separately.

## 3a. Host application registration

Before anything else touches the bus, the bridge calls:

```
org.freedesktop.host.portal.Registry.Register("net.prism.Prism")
```

xdg-desktop-portal 1.20 introduced this handshake for unsandboxed applications,
and 1.21 made `GlobalShortcuts.CreateSession` fail outright when the calling
connection has no application id. Registration is one-shot per connection and
must precede every other portal call, so it happens the instant the session bus
connection is created. Older portals have no such interface, which is not an
error.

The id has to match the basename of an installed `.desktop` file;
`scripts/install-desktop-file.sh` writes one. See [HOTKEYS.md](HOTKEYS.md).

## 4. XDG ScreenCast portal integration

The bridge drives the standard handshake and nothing else:

```
CreateSession   → Request → Response(session_handle)
SelectSources   → Request → Response()            ← KDE shows its source picker
Start           → Request → Response(streams)     ← user has chosen
OpenPipeWireRemote                                 → PipeWire fd (via SCM_RIGHTS)
```

Prism does not enumerate Wayland windows, does not query KWin, and does not ask
for anything beyond what the user grants in that dialog. `SelectSources` offers
the intersection of what Prism was asked for (any / monitors / windows) and what
the portal advertises in `AvailableSourceTypes`, and requests `multiple: false`.
Cursor mode follows the **Capture → Include Cursor** setting, downgraded to
`hidden` when the portal does not offer `embedded`.

KDE's portal has historically answered a single-source request with several
streams, of which only the last is the selection. The response handler skips
ahead to the last entry rather than failing, and logs when it has to.

### Signal delivery

The `Response` signals are subscribed with `G_DBUS_SIGNAL_FLAGS_NONE`, not
`NO_MATCH_RULE`. xdg-desktop-portal exports each `Request` as an ordinary
skeleton and emits `Response` as a **broadcast** with no destination, so the bus
only routes it to us if a match rule exists. Suppressing the `AddMatch` saves one
round trip per request and loses every reply — which is what a mock portal
demonstrated before this was corrected.

## 5. PipeWire integration

The stream is negotiated for `BGRx` (preferred), `BGRA`, `RGBx` or `RGBA`. The
first two are byte-identical to `DXGI_FORMAT_B8G8R8A8_UNORM`; the second two are
byte-identical to `DXGI_FORMAT_R8G8B8A8_UNORM`. Prism picks the texture format to
match whatever was negotiated, so the sampler returns the same RGB either way and
**no conversion happens at all** — not on the CPU and not on the GPU. Any other
format is refused with a clear error rather than silently guessed at.

The one exception is the shader-free fallback path, where
`CopySubresourceRegion` cannot cross format families. There an RGBx source is
swapped once on the CPU in the mailbox, timed, and reported in diagnostics as
`Pixel path: CPU channel swap`. An invisible conversion would be worse than a
slow one.

### Nothing is assumed about layout

A negotiated format is not a promise about memory layout, so every buffer is
checked against what it reports rather than what it ought to contain:

* `chunk->stride` is used when present and falls back to `width * 4`;
* `chunk->offset` is honoured, so a chunk starting partway into the mapping is
  handled;
* the readable extent is derived from `maxsize`, `offset` and `chunk->size`;
* a crop rectangle is validated against the frame before being applied;
* the frame is accepted only if `(height - 1) * stride + width * 4` fits inside
  that extent — the last row needs its visible pixels, not a full padded stride.

Anything failing those checks is counted and recycled, never copied. The mailbox
repeats the bounds check on the PE side, so a malformed buffer cannot become an
out-of-bounds read whatever the portal backend does.

The framerate range is negotiated open-ended (up to 1000/1) so a 144 Hz or
240 Hz source is not clamped to 60.

Two metadata blocks are requested:

* `SPA_META_VideoCrop` — KWin pads window buffers; the crop rectangle gives the
  real visible region, and the frame pointer is offset into it.
* `SPA_META_Header` — carries `pts`, on the same `CLOCK_MONOTONIC` timebase as
  the bridge's arrival stamp, which is what the diagnostics panel reports as
  PipeWire receive latency.

`on_process` first drains the whole PipeWire queue and keeps only the newest
buffer, recycling the rest uncopied. Latest-frame semantics start here, before
anything has cost a memcpy.

## 6. Latest-frame mailbox

Prism is a viewer, not a recorder: when the renderer is busy, the correct answer
is always "the newest frame, discard the rest".

Three buffers circulate between the capture thread and the render thread:

```
producer buffer   written by the capture thread, no lock held
ready buffer      the newest complete frame
consumer buffer   held by the render thread while it uploads
```

`Publish()` fills the producer buffer, then takes a mutex just long enough to
swap two `unique_ptr`s. `Acquire()` takes the same mutex to swap the ready and
consumer buffers. Neither side ever blocks on the other's work, the buffers are
recycled rather than reallocated, and the queue cannot grow past three. A
publish that overwrites an unread ready buffer is counted as a stale drop and
shown in diagnostics.

An auto-reset event is signalled on every publish, so the render loop sleeps in
`MsgWaitForMultipleObjectsEx` on the frame event and the message queue together
rather than spinning.

## 7. Frame pacing

Frame arrival is assumed irregular throughout — no fixed source FPS anywhere.

The FPS ceiling is a **consumption limit, never a generator**. If the source
produces 93 unique frames per second and the ceiling is 120, Prism processes 93.
If the ceiling is 60, Prism drops the excess without copying it. Prism never
duplicates a frame to reach a target rate.

The ceiling is enforced twice: in the bridge, before the copy (so a throttled
frame costs only a dequeue), and in the render loop, before acquiring from the
mailbox. Presentation is otherwise driven purely by arrivals; the loop
re-presents an unchanged image only every 100 ms, which keeps animated ReShade
effects running and repaints after occlusion without turning idle into a spin.

## 8. Rendering path

```
Acquire newest frame  →  Map(WRITE_DISCARD) a dynamic BGRA8 texture
                      →  ClearRenderTargetView (black bars)
                      →  Draw(3, 0)  — fullscreen triangle from SV_VertexID
                      →  Present()   — ReShade's effect chain runs here
```

No vertex buffer, no input layout, no intermediate render target. The pixel
shader samples the capture texture through a scale/offset mapping computed on
the CPU for the current display mode and returns the sampled RGB unchanged: no
gamma adjustment, no tone mapping, no sharpening, no colour-temperature shift.

Display modes: **Original Size** (1:1, point-sampled, centred), **Fit** (aspect
preserved, letterboxed; point-sampled when the scale lands on exactly 1.0),
**Stretch**, and **Integer Scale** (largest whole multiple that fits,
point-sampled). Any source aspect ratio works — 16:9, 16:10, 4:3, ultrawide —
because the mapping is derived from the actual source and back-buffer sizes.

The shaders are compiled at runtime through `d3dcompiler_47.dll`, which ReShade
requires anyway. If it is missing, Prism falls back to a shader-free
`CopySubresourceRegion` blit: unscaled and centred, but still correct and still
enough to validate ReShade.

Diagnostics are deliberately drawn in a **separate Win32 window**, not as an
overlay on the back buffer. An overlay would be processed by ReShade along with
the captured image, which would both corrupt the diagnostic and pollute the
picture.

## 8a. Gameplay and configuration modes

Prism has two output postures, switched by a global shortcut.

**Gameplay output** is a borderless fullscreen window on the chosen monitor,
stacked above the game and deliberately inert:

* `WS_EX_NOACTIVATE` keeps it out of the focus chain, so showing it or clicking
  it never takes the foreground from the game;
* `WS_EX_TRANSPARENT` makes hit-testing fall through to the window underneath;
* `WM_MOUSEACTIVATE` returns `MA_NOACTIVATEANDEAT` as a second line of defence.

Together those mean the real keyboard and mouse keep driving the real game while
the user watches Prism. **Prism forwards nothing and synthesises nothing** —
there is no input injection anywhere in the codebase, which is both simpler and
the only approach that survives anti-cheat and Wayland's input model.

**Configuration mode** clears both styles, takes the foreground, and lets ReShade's
overlay be used normally. The game loses focus for as long as it lasts, which is
the honest trade: one click cannot drive both ReShade and the game.

Prism does not try to press ReShade's overlay key for the user. Synthesising
input into ReShade is exactly the kind of fragility configuration mode exists to
avoid; making Prism interactive and letting the normal overlay hotkey work is
both simpler and more reliable.

Wine maps both extended styles onto its X11/Wayland windows, but a compositor is
free to disagree about stacking and focus. This is the one part of Prism that
genuinely needs checking on a real KDE session rather than reasoning about.

### Capture the window, not the screen

In gameplay mode Prism covers the monitor it is displaying on. Capturing that
monitor would feed Prism its own output — Prism inside Prism inside Prism. Pick
the **game window** as the source, not the screen, unless recursion is what you
are testing.

## 8b. Global shortcuts

Wayland gives no application a key grab, so the hotkeys go through the XDG
GlobalShortcuts portal, driven by the bridge on the same GLib loop as everything
else. `Activated` arrives as a D-Bus signal whatever has focus, is dispatched on
the capture thread, and is posted straight to the UI thread untouched.

`RegisterHotKey` is kept as a fallback and armed only once the portal has
definitively failed, so a bound shortcut never fires twice. Under Wayland that
fallback is focus-bound and near-useless for gameplay mode; the diagnostics
window says so when it is in use. Details in [HOTKEYS.md](HOTKEYS.md).

## 9. GPU selection

Adapter 0 is not assumed to be correct — on a laptop it is often the integrated
GPU. Prism enumerates adapters with `IDXGIFactory1::EnumAdapters1` and lists
them in the **GPU** menu with their descriptions. The default is
`IDXGIFactory6::EnumAdapterByGpuPreference` with
`DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE`, mapped back to an enumeration index for
display; an explicit choice overrides it and is stored in `Prism.ini`. Changing
the adapter rebuilds the device and swap chain in place, pausing and resuming
any live capture session around the rebuild.

## 9a. Linux-side introspection

DXGI tells Prism an adapter's description, IDs and VRAM, and nothing else. It has
no idea about PCI topology, which kernel driver is bound, or what a PCIe link
actually trained at — which matters when the second GPU arrives over
M.2-to-OCuLink and can quietly come up at Gen3 x2.

So the bridge reads it from sysfs and hands it back: PCI address, vendor/device
IDs, bound driver, DRM card and render nodes, the stable `/dev/dri/by-path` name,
current and maximum link speed and width, and the GPU-selection environment in
effect. Devices are keyed by PCI address throughout — `card0` and `card1` are
assigned in probe order and can swap between boots.

The diagnostics panel cross-references the two sides, marking each detected GPU
`Used by Prism: Yes/No` by matching PCI IDs against the adapter DXGI handed over.

## 10. Captured-application isolation

This is structural, not a policy that could be relaxed. Prism's only inputs are
a D-Bus connection to `org.freedesktop.portal.Desktop` and a PipeWire file
descriptor that the portal hands over after the user consents. There is no code
path in Prism that opens a process handle, reads foreign memory, loads anything
into another process, hooks a graphics API, or writes to a game's directory —
and no dependency that would let one exist. ReShade is loaded by `Prism.exe`
from `Prism.exe`'s own directory.
