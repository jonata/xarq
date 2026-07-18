# Contributing to XarQ

Thanks for your interest in XarQ. This is a Qt6 / C++17 project; the whole
application lives under [`src/`](src/).

## Build

```sh
cd src
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

See the [README](README.md) for the dependency list.

## Test before you push

Every change must keep the self-test green:

```sh
env QT_QPA_PLATFORM=offscreen QT_HASH_SEED=0 ./build/xarq --selftest
```

The suite injects real mouse/key events into the canvas and asserts on the
document model — please add a check for any behaviour you add or fix.

XarQ also aims for **byte-identical `.xar` render output**: if you have a local
sample corpus, render it before and after your change and diff the PNGs. Any new
model or render field should default to a no-op so existing files render
unchanged.

## Style

- C++17, 4-space indentation, no tabs (see [`.editorconfig`](.editorconfig)).
- Match the surrounding code's naming and comment density.
- Keep commits focused; explain the *why* in the message.

## Licence

By contributing you agree that your contributions are licensed under the
project's **GPL-2.0** licence (see [`COPYING`](COPYING)).
