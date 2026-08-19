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
    src/oq_input.c      terminal bytes -> RETROK_* key events
    src/oq_retro.c      libretro host: environment callbacks, video/audio pumps
    src/oq_render.c      dedicated presentation thread, double-buffered
    src/oq_audio.c       ALSA sink, non-blocking
    src/oq_present.h     backend interface: init/resize/frame/shutdown
    src/oq_chafa.c       chafa backend  (glyph and colour dials independent)
    src/oq_caca.c        libcaca backend (16-colour ceiling)
    src/oq_main.c        CLI, demo mode, host loop

Backends are compiled in conditionally (`OQ_HAVE_CHAFA`, `OQ_HAVE_CACA`,
`OQ_HAVE_ALSA`) and selected/detected at runtime, so the same frame can be
A/B'd between renderers and a machine without a given library still builds
and runs (just without that feature). `make backends` reports which of the
three were actually found via `pkg-config`.

Both video backends render into a detached canvas and return a string that
*we* write. Notably we do not use `caca_create_display()` — its
ncurses/slang drivers seize the tty and would fight our own raw-mode and
keyboard-protocol handling.

## Pixel plumbing

libcaca reads `bpp/8` bytes little-endian into a word, so for in-memory byte
order R,G,B the word is `B<<16 | G<<8 | R`. The red and blue masks passed to
`caca_create_dither()` therefore look swapped relative to an ARGB literal.
The `--demo` test pattern includes saturated primaries in a known order
specifically so this class of mistake is visible at a glance.

## Host loop

`src/oq_retro.c` answers the core's environment queries, takes RGB565 off
`video_refresh`, expands to RGB888 and hands it to the presentation backend
via a `want_frame`/`sink` pair supplied by `oq_main.c`.

Two things that bite:

- **`STATIC_LINKING=1` drops libretro-common.** `Makefile.common:119` (in
  `third_party/tyrquake`) excludes `file_path.c`, `compat_strl.c`,
  `vfs_implementation.c` and friends when building the static archive, on
  the assumption the frontend already links them in. Our top-level
  `Makefile` compiles that exact file list from `libretro-common/` into the
  binary itself (`LRC_SRC`); if the core ever needs another
  libretro-common source, expect a link failure that names it explicitly,
  not a runtime crash.
- **Core logs must never reach stdout.** stdout is the picture; one log
  line shreds the frame mid-escape-sequence. `RETRO_ENVIRONMENT_GET_LOG_INTERFACE`
  is answered with a callback that writes to `--log=PATH` or is a no-op if
  no log path was given.

`tyrquake_compute_rendering` is forced to `disabled` via
`RETRO_ENVIRONMENT_GET_VARIABLE` so the software rasterizer stays the active
backend rather than the Vulkan compute path.

## Render thread

Presentation (glyph conversion + writing escape sequences to stdout) runs on
its own thread, `src/oq_render.c`, separate from the thread driving
`retro_run()`. This is not an optimisation, it is load-bearing: the engine
needs roughly 16.7 ms/frame on its own to hold its internal timing target and
saturates a full core doing only that. Character conversion is real
additional CPU work; running it inline with the engine loop would steal from
the engine's own budget and both would miss their targets.

The two threads hand off frames through a double buffer (`pending`/`front`)
swapped under a mutex — the render thread never blocks the engine thread
waiting for a slow terminal, and a frame the render thread hasn't gotten to
yet is simply overwritten by the next one (tracked in `oq_render_dropped()`),
newest-wins, rather than queued.

**All terminal writes happen on the render thread, including the clear on
resize.** Two threads writing to the same fd can interleave mid escape
sequence and corrupt the frame, so `oq_render_reconfigure()` only flags that
a resize happened; the actual `oq_term_clear()` + `backend->resize()` runs
inside `render_loop()` on the next iteration.

## Canvas cap

Filling a large terminal from a 320×200 source is close to pure waste. With
`--symbols=fine`, one character cell already resolves a 2×4 sub-pixel block
(quadrant/sextant/octant glyphs), so beyond `res_w/2` columns by `res_h/4`
rows the backend is upscaling detail that isn't there, at several times the
conversion cost and — the part that actually hurt — several times the
escape-sequence volume the terminal emulator has to parse.

Measured effect: before the render thread and this cap existed, a 400×100
terminal ran at 11.8 fps while emitting 1.1 MB of escape sequences per frame.
With both in place the same setup holds roughly 61 fps, independent of
terminal size. `--cells=WxH` overrides the cap explicitly; `--cells=0x0`
disables it and fills the terminal.

## Input

### Two paths

`src/oq_input.c` supports two input regimes, chosen by whether the kitty
keyboard protocol negotiation in `oq_term.c` succeeded:

- **Kitty keyboard protocol**: `CSI key ; mods : event u` sequences, where
  the event type is a *sub-parameter* of the modifier field (1 press, 2
  repeat, 3 release). Repeats are dropped on receipt — the engine tracks its
  own repeat state, so forwarding them would double-fire actions. Real
  key-up events map straight onto the `down` flag of libretro's
  `retro_keyboard_callback`, so "hold W to walk forward" behaves correctly.
- **Legacy fallback** (no kitty support, or negotiation failed): the
  terminal only ever reports presses. Each press arms a timer
  (`HOLD_MS` = 260 ms) and synthesises a release when it expires; the
  terminal's own key-repeat re-arms the timer while the key is actually
  held down. 260 ms has to comfortably exceed the terminal's own repeat
  period (~30 ms) or a held key stutters — release, then immediate re-press,
  visible as jitter in movement.

Keycodes map onto `RETROK_*`, which is ASCII-compatible below 127; kitty's
private-use range 57441–57449 covers the modifier keys (`RETROK_LCTRL` etc).

### Quit hatch

Ctrl-Q, Ctrl-\\, and F10 all quit, and are intercepted from the **decoded**
key event, not a raw control byte. Under the kitty protocol, Ctrl-\\ never
arrives as the literal 0x1c byte — it arrives as `CSI 92;5u` — so a raw-byte
check silently stops catching it the moment the protocol activates. See
`emit()` in `src/oq_input.c`.

### The per-screen keyboard-flag stack pitfall

The kitty keyboard protocol's flag stack (`CSI > flags u` / `CSI < u`) is
**per alternate/primary screen**, not global to the terminal session. Pushing
flags before switching to the alternate screen looks like it works — the
terminal accepts the push — and then the switch to the alt screen
(`\033[?1049h`) silently discards it, because that screen has its own,
empty, flag stack. The result is a program that believes it negotiated key
releases while the terminal is actually sending legacy press-only bytes, so
every key looks like it never comes back up.

The fix in `src/oq_term.c` is ordering: switch to the alternate screen
*first*, and only then query/push the kitty flags. The push, and the
verifying re-query, both happen after `\033[?1049h`.

### Lone-ESC grace period

A bare ESC keypress and the start of every CSI/SS3 escape sequence look
identical for one byte. `oq_input.c` cannot tell "the user pressed Escape"
from "an escape sequence is about to arrive" until either more bytes show up
or a short window elapses. It gives an unterminated ESC a 50 ms grace period
(`esc_since`, checked against a 50 ms threshold) before deciding it really
was a standalone Escape keypress and delivering `RETROK_ESCAPE`. Anything
that completes a recognisable sequence within that window is parsed as that
sequence instead, and the grace timer resets to zero on any non-ESC byte.

## Audio

`src/oq_audio.c` wraps ALSA (`OQ_HAVE_ALSA`, conditionally compiled) behind
a device-agnostic API; without ALSA the functions are no-op stubs that
report failure from `oq_audio_init()`, so the rest of the codebase does not
need `#ifdef`s.

Design points:

- **Never blocks the frame loop.** The device is opened
  `SND_PCM_NONBLOCK`; a write that doesn't fit is dropped
  (`oq_audio_dropped()` counts this) rather than waited on. A blocking
  `snd_pcm_writei()` would stall the whole frame loop inside the sound
  driver and the picture would stutter in lockstep with the audio buffer.
- **Silent ALSA error handler.** ALSA's default handler writes to stderr,
  which typically shares the terminal with the picture; one "underrun
  occurred" would corrupt a frame the same way a stray log line would.
  `snd_lib_error_set_handler()` is pointed at a no-op.
- **`OMAQUAKE_ALSA_DEVICE`** selects the ALSA PCM name (default `default`);
  set it to `null` to run against ALSA's bit-bucket device, which is how
  automated/CI runs avoid making noise on the host.
  `--no-sound` skips opening the device entirely.
- **Latency budget is 80 ms** (`OQ_AUDIO_LATENCY_US`), picked empirically:
  measured drop rates at 40 ms and 160 ms were the same, because what
  actually overflows the buffer in practice is a loop stall (e.g. a level
  load), not shallow buffering — so 80 ms sits in the middle of a flat
  curve rather than trading against anything.
- The core is only asked for its actual sample rate after
  `retro_get_system_av_info()` returns from `retro_load_game()`, so the
  ALSA device can't be opened any earlier; `soft_resample=1` is passed so a
  sound card that insists on 48 kHz doesn't cause the open to fail outright.

## Terminal raw-mode gotchas

- **`OPOST` is cleared in raw mode**, which means the kernel no longer
  translates bare `\n` (LF) into `\r\n` on the way out. Every place that
  emits an embedded newline inside a frame (`oq_term_present`,
  `oq_term_present_at`) has to emit `\r\n` itself, or subsequent lines just
  keep advancing the same column and the picture appears to scroll/shear
  down the screen instead of drawing in place.
- See "The per-screen keyboard-flag stack pitfall" above for the alt-screen
  ordering requirement — it belongs with terminal setup as much as input.

## Still to do

1. **Mouse look** — in progress by another workstream at time of writing. A
   terminal offers no native relative pointer motion; SGR mouse reporting
   only gives absolute cell positions, which is coarse and has no
   warp-to-centre equivalent. Do not assume any particular scheme is final.
2. **Packaging** — see `packaging/PKGBUILD`.
