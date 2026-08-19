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

## Still to do

1. **Host loop** — implement the libretro callbacks (`environment`,
   `video_refresh`, `audio_sample_batch`, `input_poll`, `input_state`),
   call `retro_load_game()` with the pak path, drive `retro_run()` at the
   rate from `retro_get_system_av_info()`.
2. **RGB565 → RGB888** expansion in front of the backend.
3. **Input** — parse kitty `CSI unicode ; mods : event-type u` sequences into
   press/release and feed the core's `retro_keyboard_callback`. Fallback path
   for terminals without the protocol: synthesise a release after an idle
   timeout and lean on the terminal's auto-repeat.
4. **Audio** — libretro hands us signed 16-bit stereo; SDL2 or ALSA out, or
   `--no-sound`.
5. **Aspect** — Quake renders 4:3 at 320×200; terminal cells are roughly 1:2,
   so the cell grid needs correcting or everything is squashed.
6. **Packaging** — see `packaging/PKGBUILD`.
