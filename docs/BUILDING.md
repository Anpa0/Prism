# Building Prism

## Project layout

```
Prism/
├── CMakeLists.txt              Prism.exe build (MinGW-w64 cross-compile)
├── cmake/
│   └── mingw-w64-x86_64.cmake  toolchain file
├── include/
│   └── PrismCapture.h          ABI shared by both halves
├── src/                        Prism.exe
│   ├── main.cpp                entry point, single-instance guard
│   ├── App.cpp/.h              window, menus, tray, render loop
│   ├── Renderer.cpp/.h         D3D11 device, swap chain, present
│   ├── CaptureBridge.cpp/.h    loads PrismCapture.dll
│   ├── FrameMailbox.cpp/.h     latest-frame triple buffer
│   ├── TestPattern.cpp/.h      synthetic source for bring-up
│   ├── Diagnostics.cpp/.h      counters, ReShade detection, report
│   ├── Settings.cpp/.h         Prism.ini
│   └── Common.h                shared helpers, ComPtr
├── bridge/                     PrismCapture.dll (Winelib ELF)
│   ├── Makefile                winegcc build
│   ├── PrismCapture.spec       export table
│   ├── prism_bridge.c          ABI surface, capture thread
│   ├── screencast.c/.h         XDG ScreenCast portal driver
│   ├── portal.c/.h             Request/Response helper
│   ├── pipewire.c/.h           stream negotiation, frame arrival
│   └── prism_log.h             logging, monotonic clock
├── scripts/
│   ├── build.sh                builds both halves into build/dist
│   └── run-prism.sh            launches under Wine or Proton
└── docs/
```

The two halves are built by two toolchains on purpose. `Prism.exe` is a plain
PE64 binary and cross-compiles with MinGW-w64. `PrismCapture.dll` is a Winelib
module and needs `winegcc`, which cannot share a MinGW cross-compilation
environment. `scripts/build.sh` runs both and stages the result.

## Dependencies

### Fedora

```sh
sudo dnf install -y \
    mingw64-gcc mingw64-gcc-c++ mingw64-headers \
    cmake make git \
    wine-devel wine-core \
    pipewire-devel glib2-devel
```

* `mingw64-gcc-c++` provides `x86_64-w64-mingw32-g++` and the `d3d11.h` /
  `dxgi1_6.h` headers Prism.exe needs. No Windows SDK is involved.
* `wine-devel` provides `winegcc` and `winebuild`.
* `pipewire-devel` and `glib2-devel` provide the headers and `.pc` files the
  bridge links against.

To run the result you also want the Plasma portal backend, which Fedora KDE
installs by default:

```sh
sudo dnf install -y xdg-desktop-portal xdg-desktop-portal-kde pipewire
```

### Debian / Ubuntu

```sh
sudo apt install -y \
    mingw-w64 cmake make git \
    wine64-tools libwine-dev \
    libpipewire-0.3-dev libglib2.0-dev
```

## Build

```sh
./scripts/build.sh
```

This produces:

```
build/dist/
├── Prism.exe
└── PrismCapture.dll
```

Both files must sit in the same directory at run time — Prism loads the bridge
by absolute path from its own directory, so a stray copy elsewhere on the search
path can never be picked up.

### Building the halves separately

```sh
# Prism.exe
cmake -B build/exe -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build/exe -j"$(nproc)"

# PrismCapture.dll
make -C bridge                # release
make -C bridge debug          # adds -DPRISM_DEBUG, verbose per-frame logging
```

`make -C bridge` emits `PrismCapture.dll.so` and renames it to
`PrismCapture.dll`. That rename is not cosmetic: it is the name `Prism.exe`
passes to `LoadLibraryW`, and Wine identifies the file as a Winelib module by
inspecting it, not by its extension.

Prism.exe links `-static -static-libgcc -static-libstdc++`, so it drops into a
Proton prefix without needing MinGW runtime DLLs next to ReShade.

## Verifying the build

```sh
file build/dist/Prism.exe
#  PE32+ executable (GUI) x86-64, for MS Windows

file build/dist/PrismCapture.dll
#  ELF 64-bit LSB shared object, x86-64   ← correct, it is a Winelib module

nm -D --defined-only build/dist/PrismCapture.dll | grep PrismCapture
#  should list all seven exports
```

Then run the renderer without needing a capture source at all:

```sh
./scripts/run-prism.sh --test-pattern
```

A colour-bar and gradient pattern with a scrolling white band means the D3D11
device, shader compilation, mailbox and present path are all working.
