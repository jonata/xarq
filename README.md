# XarQ

![CI](https://github.com/jonata/xarq/actions/workflows/ci.yml/badge.svg)

XarQ is a modern, self-contained vector-graphics, photo and web-design
application — a Qt6 reimplementation and spiritual successor of Xara Xtreme /
Xara Designer Pro. It reads and writes native Xara `.xar` files at near-oracle
render fidelity, paired with a contemporary UI (InfoBars, Colour Line,
galleries, dockable panels) and a broad toolset: illustration, photo editing,
text & typography, prepress (CMYK/PDF/PSD/EPS/SVG) and responsive website
building.

Built with **Qt6 Widgets + C++17** and rendered natively with **QPainter** —
there is **no wxWidgets dependency**.

## Download

Alpha builds for Linux (AppImage) and Windows (installer or portable zip) are on the
[Releases](https://github.com/jonata/xarq/releases) page.

## Build & run

```sh
cd src
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./build/xarq
```

**Requirements:** Qt6 (Widgets, Svg, PrintSupport), zlib, HarfBuzz, pkg-config,
a C++17 compiler and CMake ≥ 3.16. On Debian/Ubuntu:

```sh
sudo apt install cmake g++ pkg-config zlib1g-dev libharfbuzz-dev \
                 qt6-base-dev qt6-svg-dev qt6-base-dev-tools libgl1-mesa-dev
```

Some features load their libraries on demand via `dlopen`, and are simply
unavailable if the library is missing: **lcms2** (ICC colour management) and
**libraw** (camera-RAW import).

### Command line

```sh
./build/xarq --open drawing.xar --export out.png   # open + export, headless
./build/xarq --shot out.png                        # render the window to a PNG
```

### Tests

```sh
env QT_QPA_PLATFORM=offscreen QT_HASH_SEED=0 ./build/xarq --selftest
```

A deterministic interaction + model harness (1600+ assertions) that injects real
mouse/key events into the canvas and asserts on the document — no screen
scraping. `QT_HASH_SEED=0` pins text layout so output is byte-identical.

## Project layout

```
src/                 the XarQ application (Qt6 Widgets, C++17, CMake)
  *.cpp / *.h        model, render, canvas, tools, XAR I/O, importers/exporters
  canvas_*.inc       canvas input + I/O, #included by canvas.cpp
  assets/            bundled fonts and logo/icon SVGs (see assets/fonts/NOTICE.md)
  CMakeLists.txt     build definition
.github/workflows/   continuous integration (build + selftest)
```

## Heritage & licence

XarQ is licensed under the **GNU General Public License, version 2**; the full
text is in [`COPYING`](COPYING). See [`NOTICE`](NOTICE) for the Xara heritage
notice and its additional linking permission.

It descends from **Xara Xtreme / Xara LX**, the GPL open-source release of Xara
(© 1993–2006 Xara Group Ltd), whose format documentation and reference output
informed the `.xar` support and render-fidelity work here. XarQ contains no
wxWidgets code; the original wxWidgets Xara LX sources remain available upstream
at <https://github.com/samuell/xara-xtreme>.
