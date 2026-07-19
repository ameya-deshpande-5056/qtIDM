# Contributing

## Requirements

- Linux.
- C++20 compiler.
- Qt6 Core, Gui, Widgets, DBus, Test.
- libcurl.
- SQLite3.
- CMake and Ninja.

## Local Check

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

## Rules

- Keep the project Linux-only.
- Do not add Electron, QML, WebEngine, or heavy runtimes.
- Submit only original work or material whose licence is compatible with the project.
- Add tests for core behavior changes.
- Keep generated build artifacts out of git.

## Branding assets

`assets/io.qtidm.Qtidm.svg` is the canonical qtIDM logo. After changing it, run:

```bash
packaging/generate-icons.sh
```

This refreshes the committed desktop and browser-extension PNG variants.
