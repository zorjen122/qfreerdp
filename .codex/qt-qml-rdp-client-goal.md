# Qt6 + QML minimal FreeRDP client goal

This document records the working plan for building a minimal cross-platform RDP client with Qt6 + QML on top of libfreerdp.

The goal is not to copy the full Windows client. The first goal is to understand, observe, and control the FreeRDP event/data flow one piece at a time. Only after a part is well understood should it be connected to the Qt/QML UI.

## Target

Build a minimal RDP client that supports:

- connection setup and shutdown
- framebuffer rendering
- mouse input
- keyboard input
- basic desktop resize handling only when needed later

- sound
- clipboard
- dynamic window resize
- multi-monitor
- RAIL
- smart sizing
- remote cursor shape
- advanced keyboard layout and IME handling

## References
- [FreeRDP GitHub](https://github.com/FreeRDP/FreeRDP)