# OmaQuake design notes

## Why not "SDL redirects to caca"

The obvious approach — `SDL_VIDEODRIVER=caca` — is dead. Those backends
existed only in **SDL 1.2** (`--enable-video-caca`). SDL2 removed them and
SDL3 never had them; on a current Arch box `strings libSDL3.so.0 | grep caca`
returns nothing, and `sdl12-compat` only reimplements the SDL 1.2 *API* on
top of SDL2/3, so it inherits SDL3's driver list.

## Why tyrquake and not QuakeSpasm

QuakeSpasm has no software rasterizer at all — its `Quake/` directory is
`gl_*.c` with no `d_*.c`. Getting CPU-side pixels means software OpenGL
(llvmpipe/OSMesa) plus a `glReadPixels` every frame.

tyrquake keeps the original software renderer (`d_edge.c`, `d_scan.c`,
`r_edge.c`, …) drawing into `vid.buffer` as 8-bit palettized pixels, which
the libretro layer converts to RGB565 and pushes through `video_cb`. That
callback is the seam we tap.

## Layering

    src/oq_term.c      raw mode, alt screen, size polling, kitty kbd negotiation
    src/oq_present.h   backend interface: init/resize/frame/shutdown
    src/oq_chafa.c     chafa backend  (glyph and colour dials independent)
    src/oq_caca.c      libcaca backend (16-colour ceiling)
    src/oq_main.c      CLI, demo mode, host loop

Backends are compiled in conditionally (`OQ_HAVE_CHAFA`, `OQ_HAVE_CACA`) and
selected at runtime, so the same frame can be A/B'd between renderers.

Both backends render into a detached canvas and return a string that *we*
write. Notably we do not use `caca_create_display()` — its ncurses/slang
drivers seize the tty and would fight our own raw-mode and keyboard-protocol
handling.

## Pixel plumbing

libcaca reads `bpp/8` bytes little-endian into a word, so for in-memory byte
order R,G,B the word is `B<<16 | G<<8 | R`. The red and blue masks passed to
`caca_create_dither()` therefore look swapped relative to an ARGB literal.
The `--demo` test pattern includes saturated primaries in a known order
specifically so this class of mistake is visible at a glance.

## Host loop

`src/oq_retro.c` answers the core's environment queries, takes RGB565 off
`video_refresh`, expands to RGB888 and hands it to the presentation backend.

Two things that bite:

- **`STATIC_LINKING=1` drops libretro-common.** `Makefile.common:119` excludes
  `file_path.c`, `compat_strl.c`, `vfs_implementation.c` and friends when
  building the archive, on the assumption the frontend already has them. Our
  Makefile compiles that exact list into the binary. Networking
  (`net_compat.c`, `net_socket.c`) is excluded the same way a few lines down.
- **Core logs must never reach stdout.** stdout is the picture; one log line
  shreds the frame. `RETRO_ENVIRONMENT_GET_LOG_INTERFACE` is answered with a
  callback that writes to `--log=PATH` or discards.

`tyrquake_compute_rendering` is forced to `disabled` so the software
rasterizer stays the active backend rather than the Vulkan compute path.

## Input

`src/oq_input.c` parses kitty `CSI key ; mods : event u` sequences, where the
event type is a *sub-parameter* of the modifier field (1 press, 2 repeat,
3 release). Repeats are dropped — the engine tracks its own repeat state.
Keycodes map onto `RETROK_*`, which is ASCII-compatible below 127, and kitty's
private range 57441–57449 covers the modifier keys.

The legacy path synthesises a release 260 ms after the last press, relying on
the terminal's own auto-repeat to keep a held key alive. That interval has to
exceed the repeat period (~30 ms) or a held key stutters.

Ctrl-\\ is intercepted as a quit hatch and never reaches the engine.

## Still to do

1. **Audio** — the core's samples are accepted and dropped. libretro hands us
   signed 16-bit stereo; ALSA or SDL2 out, plus a `--no-sound` switch.
2. **Mouse look** — a terminal offers no usable relative pointer motion, so
   turning is stuck on the arrow keys. SGR mouse reporting gives absolute cell
   positions only, which is too coarse; warp-to-centre is not available.
3. **Packaging** — see `packaging/PKGBUILD`.
