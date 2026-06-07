# qfreerdp

A minimal Qt 6/QML + FreeRDP 3 experimental client

## Features

- Displays the remote desktop in a Qt/QML window
- Forwards mouse movement, clicks, and wheel events to the RDP session
- Maps common keyboard keys to FreeRDP scancodes
- Provides basic two-way clipboard sync for text and images

## Dependencies

- CMake 3.16+
- Clang/Clang++
- Qt 6: `Core`, `Gui`, `Qml`, `Quick`
- FreeRDP 3 development packages, including `freerdp3-dev` and `libwinpr3-dev`

The public FreeRDP development packages provide the headers and libraries used
by this project. The `cliprdr` client add-in implementation is not installed as
a public development interface, so the small local copy in [ref-tmp](ref-tmp) is
used for the clipboard channel.

## Build

```bash
./configure.sh
cmake --build build
```

The executable is usually generated at:

```bash
./build/qf-client
```

## Run

```bash
./build/qf-client
```

The current project is in a demo development state. Connection settings are
still hard-coded in [mini-qf-client.cc](mini-qf-client.cc).
