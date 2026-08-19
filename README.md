# OmaQuake

Quake 1 rendered as **characters** in a terminal — not as images.

The point is text: every frame is converted into glyphs and ANSI colour, so
the picture is real selectable characters in the terminal grid. Terminal
graphics protocols (kitty, sixel, iTerm2) send actual bitmaps and are
deliberately **not** used here.

## How it works

| Layer | Choice | Why |
|---|---|---|
| Engine | [libretro/tyrquake](https://github.com/libretro/tyrquake) | Keeps Quake's original 8-bit software rasterizer. QuakeSpasm is GL-only and would need llvmpipe/OSMesa plus a readback. |
| Linkage | `STATIC_LINKING=1` → `tyrquake_libretro.a` | One self-contained `omaquake` binary rather than a dlopen'd core. |
| Video out | [chafa](https://hpjansson.org/chafa/) (default), libcaca (alternative) | chafa sets glyph repertoire and colour depth *independently*, so you can have pure ASCII in 24-bit colour. libcaca is capped at 16 ANSI colours in a terminal. |
| Input | kitty keyboard protocol, with fallback | A plain tty reports key **presses only** — see below. |

The engine hands us RGB565 frames through the libretro `video_refresh`
callback; we expand to RGB888 and hand that to a presentation backend, which
returns a string of characters we write to stdout.

## The key-release problem

A normal terminal never tells you that a key was *released*. That makes
"hold W to walk forward" impossible, and synthesising releases from a timer
feels terrible.

The [kitty keyboard protocol](https://sw.kovidgoyal.net/kitty/keyboard-protocol/)
solves it: flag `0b10` makes the terminal report event types, so we get
press, repeat and release. OmaQuake probes for it at startup and reports
what it got. Conveniently, the tyrquake core registers a libretro
`retro_keyboard_callback`, which takes a `down` flag — so real key-up events
map straight through to the engine.

Terminals known to support it: kitty, Ghostty, foot, WezTerm.

## Building

    sudo pacman -S chafa libcaca      # either one is enough; both is better
    git submodule update --init
    make engine                        # builds tyrquake_libretro.a
    make

`make backends` reports which presentation backends were compiled in.

## Running

    ./build/omaquake --demo                        # test pattern, no game data
    ./build/omaquake ~/games/quake/id1/pak0.pak    # the real thing

Options:

    --video=chafa|caca     presentation backend
    --symbols=ascii|block|fine
    --color=mono|16|256|true
    --cell=WxH             character cell pixel size (default 10x20)
    --res=WxH              engine render resolution (default 320x200)
    --log=PATH             engine log destination
    --frames=N

Press **Ctrl-\\** to quit; the engine never sees it. Escape opens Quake's own
menu as usual.

The bigger your terminal, the more pixels you get -- but note the engine
renders at `--res` and the picture is downsampled to the cell grid, so a
huge terminal with `--res=320x200` just magnifies the same detail.

`--symbols=ascii` restricts the glyph set to letters and punctuation for the
classic look; `fine` allows quadrant/sextant/octant glyphs, which are still
characters but give 2×4 sub-cell detail.

## Game data

Not included. The shareware `pak0.pak` is freely redistributable; the full
game's `pak1.pak` requires owning Quake.

## Status

Playable. The engine boots, renders, and takes keyboard input.

Measured at 200x50 cells, all three presentation modes hold the engine's
72 fps pacing target, so the character conversion is not the bottleneck.

Not yet done: **audio** (accepted from the core and discarded) and **mouse
look** (a terminal gives no usable relative motion; turning is on the arrow
keys). See `docs/DESIGN.md`.

## Licence

GPL-2.0-only, matching the Quake engine source this builds on.
