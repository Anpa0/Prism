# ReShade with Prism

ReShade attaches to `Prism.exe` and only to `Prism.exe`. The captured
application's process, files and rendering are untouched — Prism never installs
anything into a game directory, and there is nothing to configure on the game's
side.

```
Captured source → PipeWire → Prism.exe → D3D11 back buffer → ReShade → presentation
```

## Installation

Prism is a Direct3D 11 application, so ReShade's `dxgi.dll` proxy is the right
choice.

### Using ReShade's installer

1. Run `ReShade_Setup_x.y.z.exe` in the same prefix as Prism:
   ```sh
   WINEPREFIX="$HOME/.local/share/prism/pfx" wine ReShade_Setup_6.5.1.exe
   ```
2. Browse to `build/dist/Prism.exe`.
3. Choose **Direct3D 10/11/12** as the rendering API.
4. Skip the effect packages for now — the first validation should use a single
   obvious effect, not a shader pack.

### By hand

Take `ReShade64.dll` from the ReShade distribution and drop it next to
`Prism.exe` as `dxgi.dll`:

```
build/dist/
├── Prism.exe
├── PrismCapture.dll
├── dxgi.dll             ← ReShade64.dll, renamed
├── ReShade.ini
├── ReShadePreset.ini
└── reshade-shaders/
    ├── Shaders/
    └── Textures/
```

`d3d11.dll` works as the proxy name too, if you prefer it.

## Making Wine load it

Wine prefers its own builtin `dxgi` unless told otherwise:

```sh
WINEDLLOVERRIDES="d3dcompiler_47,dxgi=n,b" wine Prism.exe
```

`scripts/run-prism.sh` sets this already. Without it ReShade never loads, and
Prism's diagnostics will say so.

`d3dcompiler_47` is in the same override list because ReShade needs it to
compile effects, and Prism uses it for its own presentation shaders. Install the
native DLL with `winetricks d3dcompiler_47` if the prefix does not have it.

## Minimal `ReShade.ini`

```ini
[GENERAL]
EffectSearchPaths=.\reshade-shaders\Shaders
TextureSearchPaths=.\reshade-shaders\Textures
PresetPath=.\ReShadePreset.ini

[INPUT]
KeyOverlay=36,0,0,0
```

`Home` opens ReShade's overlay over Prism's window.

## Confirming it loaded

Open **View → Diagnostics**. The ReShade section reports:

```
ReShade
  Loaded:               Yes
  Proxy:                dxgi.dll
  Path:                 Z:\home\you\Prism\build\dist\dxgi.dll
  Add-on API:           Yes
```

Prism identifies ReShade two ways: the proxy module exports
`ReShadeRegisterAddon`, or it was loaded from Prism's own directory rather than
from the system. Either is enough.

## First effect

Validate with one obvious, cheap effect before touching a shader pack. Any of
these makes success unmistakable:

* **`Monochrome.fx`** — the feed goes greyscale.
* **`Invert`/`Negative`** — colours flip.
* **`Vibrance.fx`** with saturation pushed high.
* **`LUT.fx`** with any LUT.

Success looks like this:

* the captured game still looks completely normal on its own display;
* Prism's window shows the same content with the effect applied;
* the game's process and files are unchanged.

Complicated shader packs (qUINT, CRT chains, depth-based effects) come later.
Depth-based effects in particular cannot work here by design: Prism receives a
flat colour image, so there is no depth buffer to give ReShade.

## Notes

* ReShade processes Prism's back buffer, which holds the capture plus the black
  letterbox bars. In **Stretch** mode the image fills the buffer entirely, which
  is usually what you want when a screen-space effect is involved.
* Prism draws no overlay of its own onto the back buffer — diagnostics live in a
  separate window — so the only thing ReShade ever sees is the captured image.
* Changing GPU in the **GPU** menu rebuilds the device and swap chain. ReShade
  re-initialises with it; Prism re-detects it afterwards.
