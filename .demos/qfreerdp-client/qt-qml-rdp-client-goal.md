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

Explicitly out of scope for the first version:

- sound
- clipboard
- dynamic window resize
- multi-monitor
- RAIL
- smart sizing
- remote cursor shape
- advanced keyboard layout and IME handling

## Current references

Use these files as the starting map:

- `client/Sample/tf_freerdp.c`
  - best minimal FreeRDP client skeleton
  - useful for `PreConnect`, `PostConnect`, `BeginPaint`, `EndPaint`, `DesktopResize`, and the main FreeRDP event loop
- `client/Windows/wf_client.c`
  - useful for framebuffer ownership, `gdi_init_ex`, invalid-region handling, and post-connect setup
- `client/Windows/wf_event.c`
  - useful for mouse and keyboard event translation ideas
- `.demos/mini-handless.c`
  - current minimal headless protocol/event-loop experiment
- `.demos/headless.cc`
  - current C++ FreeRDP connection experiment
- `.demos/gdi-observe.c`
  - Phase 2 graphics-update observation experiment
  - logs GDI framebuffer metadata, invalid rectangles, and lightweight dirty-rect checksums
- `.demos/.qtenv/`
  - current Qt6/QML test environment

## Core principle

Do not connect a subsystem to Qt/QML until its FreeRDP data flow is understood in isolation.

For each subsystem:

1. observe which FreeRDP callback or API is involved
2. log the event/data shape
3. verify that the data is stable and meaningful
4. create a tiny local abstraction for it
5. connect that abstraction to Qt/QML

## Phase 1: connection lifecycle

Purpose: know exactly how a minimal client starts, connects, loops, disconnects, and fails.

Tasks:

- keep a minimal `freerdp_new` / `freerdp_context_new` experiment
- configure hostname, port, username, password, desktop width, desktop height, color depth, and certificate behavior
- register `PreConnect`, `PostConnect`, and `PostDisconnect`
- run the event loop with `freerdp_get_event_handles` and `freerdp_check_event_handles`
- log connect success, connect failure, disconnect reason, and `freerdp_get_last_error`

Acceptance:

- a test program can connect to a known RDP server
- the loop stays alive after login
- disconnect is clean
- failures produce useful logs

Notes:

- this phase can stay headless
- no Qt integration is needed here

## Phase 2: graphics update observation

Purpose: understand what graphics events FreeRDP emits and where the final pixels live.

Tasks:

- enable GDI decoding with `gdi_init` or `gdi_init_ex`
- avoid `FreeRDP_DeactivateClientDecoding = TRUE` for rendering tests
- implement `BeginPaint`
- implement `EndPaint`
- inspect `context->gdi->primary`
- inspect `gdi->primary->hdc->hwnd->invalid`
- log invalid rectangles from `hwnd->cinvalid`
- log framebuffer width, height, stride, pixel format, and buffer pointer
- on `DesktopResize`, log the new width and height and call `gdi_resize`

Acceptance:

- after login, `EndPaint` receives non-empty invalid regions
- the framebuffer contains changing pixel data
- invalid rectangles correspond to visible screen changes
- no Qt rendering is involved yet

Suggested experiment:

- dump a small number of frames or dirty rectangles to files
- optionally write a `QImage` or raw BGRA/XRGB screenshot from the FreeRDP buffer for verification

Current artifact:

- source: `.demos/gdi-observe.c`
- binary after local build: `.demos/gdi-observe`
- build command:

```sh
gcc .demos/gdi-observe.c -o .demos/gdi-observe $(pkg-config --cflags --libs freerdp3 winpr3)
```

- run command shape:

```sh
.demos/gdi-observe <host> <user> <password> [port] [width] [height] [max-paints] [domain]
.demos/gdi-observe <host> <user> <password> /d:<domain-or-machine-name>
```

- environment fallback:

```sh
RDP_HOST=192.168.1.10 RDP_USER=demo RDP_PASS=secret .demos/gdi-observe
```

This artifact intentionally has no Qt integration. Its only job is to prove what FreeRDP GDI exposes after decoding.

Troubleshooting note:

- `.demos/gdi-observe.c` forces `FreeRDP_ProxyType = PROXY_TYPE_IGNORE`.
- This is intentional because local lab RDP targets such as `192.168.*` should not be routed through `http_proxy`, `https_proxy`, or `all_proxy`.
- If FreeRDP logs `Parsed proxy configuration` for this observer, rebuild the binary and verify the current source is being used.
- `.demos/gdi-observe.c` also forces `FreeRDP_AuthenticationPackageList = "ntlm"` for NLA.
- `.demos/gdi-observe.c` currently uses `FreeRDP_AuthenticationPackageList = "none,ntlm"` for NLA.
- This disables Kerberos and then enables only NTLM, avoiding local Kerberos realm configuration warnings while testing ordinary Windows username/password logon.
- If the last error is `ERRCONNECT_LOGON_FAILURE [0x00020014]`, the TCP/TLS/NLA path is already reached; check username, password, and optional domain.
- `/cert:ignore` is accepted and ignored by the observer because certificate ignoring is already enabled for this lab tool.
- `/sec:nla`, `/sec:tls`, `/sec:rdp`, and `/sec:neg` are accepted for security-layer experiments.
- On the current lab target, forced TLS/RDP security is rejected by the server, so NLA credentials must succeed before Phase 2 can observe desktop pixels.
- A tested `ERRCONNECT_PASSWORD_EXPIRED [0x0002000E]` means the account/password pair is recognized but Windows requires a password change before RDP logon.

## Phase 3: minimal Qt framebuffer display

Purpose: display the already-understood framebuffer in QML.

Tasks:

- create a C++ `RdpSession` object that owns the FreeRDP instance and runs the RDP loop in a worker thread
- create a QML-facing item, likely `QQuickPaintedItem` for the first version
- expose the framebuffer as a `QImage`-compatible memory view or copy
- on FreeRDP `EndPaint`, post dirty rects back to the GUI thread with `Qt::QueuedConnection`
- in the Qt item, repaint only the dirty area if practical

Acceptance:

- QML window shows the remote desktop
- rendering updates after remote screen changes
- FreeRDP callbacks never directly mutate QML or scene graph state from the RDP thread

First implementation preference:

- use a simple copy from the FreeRDP framebuffer into a Qt-owned `QImage`
- optimize to shared memory / texture later only after correctness is stable

## Phase 4: mouse event observation

Purpose: understand how Qt mouse events map to RDP pointer events before wiring them permanently.

Tasks:

- capture mouse move, press, release, and wheel events in the Qt item
- log Qt local coordinates, button, modifiers, and wheel delta
- map local item coordinates to remote desktop coordinates
- call `input->MouseEvent(input, flags, x, y)` only after coordinate mapping is verified
- use these RDP flags:
  - `PTR_FLAGS_MOVE`
  - `PTR_FLAGS_DOWN`
  - `PTR_FLAGS_BUTTON1`
  - `PTR_FLAGS_BUTTON2`
  - `PTR_FLAGS_BUTTON3`
  - `PTR_FLAGS_WHEEL`
  - `PTR_FLAGS_WHEEL_NEGATIVE`
  - `PTR_FLAGS_HWHEEL` later if needed

Acceptance:

- mouse movement is visible remotely
- left, right, and middle click work
- wheel works vertically
- coordinates match the visual position in the QML item

Keep out of scope at first:

- smart sizing
- scrollbars
- remote cursor shape
- extra mouse buttons

## Phase 5: keyboard event observation

Purpose: understand key input separately from rendering and mouse input.

Tasks:

- capture Qt key press and key release events
- log Qt key, native scan code, native virtual key, text, modifiers, auto-repeat
- decide per event whether to send Unicode or RDP scancode
- for printable text, test `freerdp_input_send_unicode_keyboard_event`
- for control keys, arrows, function keys, enter, backspace, tab, escape, modifiers, test `freerdp_input_send_keyboard_event_ex`
- create a small Qt-key-to-RDP-scancode table only for keys that have been tested

Acceptance:

- ASCII text input works
- enter, backspace, tab, escape, arrows, delete, home/end, page up/down work
- Ctrl, Alt, Shift combinations are understood well enough for basic shortcuts

Keep out of scope at first:

- full keyboard layout correctness
- IME
- dead keys
- AltGr
- lock LED synchronization

## Phase 6: minimal integrated client

Purpose: combine the proven pieces into a small usable client.

Tasks:

- expose connect/disconnect properties and methods to QML
- show connection state
- show the framebuffer item as the main view
- send mouse and keyboard events only when the item has focus
- cleanly stop the RDP worker thread on window close

Acceptance:

- user can start the app, connect, view the remote desktop, click, type, and disconnect
- no known cross-thread Qt misuse
- no crash on failed connection
- no crash on normal disconnect

## Suggested class shape

Keep the first design small:

```text
RdpSession : QObject
  - connectToHost(...)
  - disconnectFromHost()
  - sendMouseEvent(...)
  - sendKeyEvent(...)
  - signal framebufferReady(width, height, format)
  - signal dirtyRect(x, y, w, h)
  - signal connectionStateChanged(...)

RdpView : QQuickPaintedItem
  - owns or references RdpSession
  - paints the latest QImage
  - translates mouse events
  - translates key events
```

Later, if `QQuickPaintedItem` is too slow, replace the rendering layer with a `QSGTexture` based item. Do that after the framebuffer and invalid rect behavior are well understood.

## Suggested pursue-goal prompts

Use focused goals rather than asking for the whole client at once:

- "Using `.demos/qt-qml-rdp-client-goal.md`, pursue Phase 2 only: create or modify a demo that logs FreeRDP GDI framebuffer metadata and invalid rectangles. Do not add Qt integration yet."
- "Using `.demos/qt-qml-rdp-client-goal.md`, pursue Phase 3 only: show a verified FreeRDP framebuffer in the existing Qt6/QML demo. Keep mouse and keyboard out of scope."
- "Using `.demos/qt-qml-rdp-client-goal.md`, pursue Phase 4 only: log and then send mouse events from the QML view to FreeRDP. Do not change keyboard handling."
- "Using `.demos/qt-qml-rdp-client-goal.md`, pursue Phase 5 only: build a minimal tested keyboard mapping for the existing Qt RDP view."

## Working rule

When a phase is unclear, add logging and a smaller experiment instead of expanding the client.

The long-term client should be simple, but the learning path should be explicit.
