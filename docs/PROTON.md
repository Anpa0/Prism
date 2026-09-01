# Running Prism under Proton

Prism is an ordinary Windows executable, so Steam can run it like any other
non-Steam application. That is the recommended route on Fedora: Proton brings a
known-good DXVK, and Steam gives Prism its own prefix.

## Adding Prism to Steam

1. **Games → Add a Non-Steam Game → Browse**, and pick
   `build/dist/Prism.exe`.
2. Right-click the new entry → **Properties → Compatibility**.
3. Tick **Force the use of a specific Steam Play compatibility tool** and choose
   **Proton Experimental** (or another Proton you have tested).
4. In **Launch Options**, paste one of the lines below.

Steam creates a prefix under
`~/.steam/steam/steamapps/compatdata/<appid>/pfx`, separate from every game's.

### Launch options — single GPU (now)

```
WINEDLLOVERRIDES="d3dcompiler_47,dxgi=n,b" %command%
```

### Launch options — once the RTX 5050 is installed

```
WINEDLLOVERRIDES="d3dcompiler_47,dxgi=n,b" MESA_VK_DEVICE_SELECT=10de:2d18! __NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia %command%
```

Replace `10de:2d18` with the real vendor:device pair from
`./scripts/check-gpus.sh`. The trailing `!` makes the Vulkan device-select layer
exclusive rather than merely preferential.

**These are Prism's launch options only.** No other Steam title needs a GPU
override; everything else inherits the session default, which
[GPU_POLICY.md](GPU_POLICY.md) pins to the AMD card.

## `WINEDLLOVERRIDES` is not optional

Wine prefers its own builtin `dxgi` unless told otherwise. Without
`dxgi=n,b`, ReShade's proxy DLL sitting next to `Prism.exe` is never loaded and
the effect chain silently does nothing. `d3dcompiler_47` is in the same list
because ReShade needs it to compile effects and Prism uses it for its own
presentation shaders.

## Without Steam

```sh
./scripts/run-prism.sh                       # plain wine, own prefix
./scripts/run-prism.sh --gpu nvidia          # pin Prism to the NVIDIA card
./scripts/run-prism.sh --proton "$HOME/.steam/steam/steamapps/common/Proton - Experimental/proton"
```

`run-prism.sh` sets `WINEDLLOVERRIDES` and, with `--gpu`, the GPU-selection
variables — for that process only.

## Prefix layout

Everything ReShade and Prism need lives in one directory:

```
build/dist/
├── Prism.exe
├── PrismCapture.dll        the Winelib capture bridge (an ELF, despite the name)
├── dxgi.dll                ReShade
├── ReShade.ini
├── ReShadePreset.ini
├── reshade-shaders/
├── Prism.ini               written by Prism on exit
└── prism-diagnostics.txt   written by View -> Save Diagnostics Report
```

Prism loads `PrismCapture.dll` by absolute path from its own directory, so a
copy elsewhere on the search path can never be picked up by accident.

## Which Proton

Proton Experimental is the recommendation: its DXVK is current, and Prism's
requirements are unremarkable — D3D11 feature level 11_0, a flip-model swap
chain, and `d3dcompiler_47`. Any Proton 8 or newer should work. If a specific
build misbehaves, the diagnostics window's `Renderer` section says whether the
flip model and tearing were accepted, which is usually the difference.

Plain `wine` works too and is what the development testing used. Proton is
preferred mainly because it ships DXVK, whereas a bare Wine prefix may fall back
to wined3d and lose a lot of performance.

## Verifying the prefix

```sh
./scripts/run-prism.sh --test-pattern --dump-diagnostics
cat build/dist/prism-diagnostics.txt
```

Look for:

```
Renderer
  Direct3D API:         D3D11
  GPU:                  AMD Radeon RX 7900 XT
  Present path:         Fullscreen triangle
```

`Present path: Copy blit (no compiler)` means `d3dcompiler_47` is missing —
install it with `winetricks -q d3dcompiler_47` against the same prefix, or let
ReShade's installer pull it in.
