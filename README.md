# OmaQuake

*Oh Mah Quake! for Opinionated Linux*

Quake 1 rendered as **characters** in a terminal — not as images.

Every frame is converted into glyphs and ANSI colour codes, so the picture on
screen is made of real, selectable characters sitting in the terminal grid.
Terminal graphics protocols that send actual bitmaps — kitty graphics, sixel,
iTerm2 inline images — are deliberately **not** used. That restriction is the
whole point of the project: it has to look like Quake while staying text.

## How it works

| Layer | Choice | Why |
|---|---|---|
| Engine | [libretro/tyrquake](https://github.com/libretro/tyrquake) | Keeps Quake's original 8-bit software rasterizer. QuakeSpasm is GL-only — getting CPU-side pixels out of it would mean software OpenGL (llvmpipe/OSMesa) plus a `glReadPixels` every frame. |
| Linkage | `STATIC_LINKING=1` → `tyrquake_libretro_unix.a` | One self-contained `omaquake` binary, not a dlopen'd core. This also means the core drops libretro-common (see `Makefile.common:119` in tyrquake) and our own Makefile has to compile the frontend-support files back in — see `docs/DESIGN.md`. |
| Video out | [chafa](https://hpjansson.org/chafa/) (default), libcaca (alternative) | chafa sets glyph repertoire and colour depth **independently**, so you can have pure ASCII rendered in 24-bit colour. libcaca in a terminal is capped at 16 ANSI colours no matter what glyphs you pick. |
| Input | kitty keyboard protocol, with a legacy fallback | A plain tty only ever reports key **presses**, never releases — see below. |
| Audio | ALSA (optional) | Samples come off the libretro audio callbacks; see Audio below. |

The engine hands over RGB565 frames through the libretro `video_refresh`
callback; the host expands them to RGB888 and hands that to a presentation
backend, which turns pixels into a string of characters that gets written to
stdout on a dedicated render thread.

## Building

    git submodule update --init
    make engine       # builds third_party/tyrquake as a static archive
    make               # builds ./build/omaquake
    make backends      # reports which optional presentation/audio backends were found

Presentation and audio backends are optional and detected via `pkg-config`
(`chafa`, `caca`, `alsa`). `make backends` tells you what actually got
compiled into the binary — trust that output over anything below, since a
backend can be silently absent if its `-dev`/pkg-config file is missing.

### As an Arch package

`packaging/PKGBUILD` builds two packages: `omaquake` (the binary) and
`omaquake-shareware-data` (the freely redistributable `pak0.pak`, fetched and
extracted at build time — see [Game data](#game-data)).

    cd packaging
    makepkg -si         # -s pulls the build dependencies, -i installs the result

`makepkg` builds from the **tag** named in `source=()`, not from your working
tree, so local edits are not in the package until they are committed, tagged,
and the pin is moved. It fetches the 9 MB shareware archive even if you only
want the binary package; `source=()` is per-pkgbase, so there is no way to skip
it.

Installed that way, `omaquake` needs no arguments: the data package puts the
pak at `/usr/share/omaquake/id1/pak0.pak`, which is already on the search path
below.

## Game data

Not included. The freely redistributable shareware release ships
`pak0.pak`, available at
<https://ftp.gwdg.de/pub/misc/ftp.idsoftware.com/idstuff/quake/quake106.zip>
(also mirrored at gamers.org/pub/idgames/idstuff/quake/). Inside that
archive, `resource.1` is an LHA self-extracting executable; `bsdtar` can
open it directly.

You do not normally need to tell OmaQuake where the data is — run it with no
argument and it searches, in order:

| |
|---|
| the path you gave, or `$OMAQUAKE_PAK` |
| `./id1/` |
| next to the binary, and the directory above it (so `build/omaquake` finds a checkout's `id1/`) |
| `$XDG_DATA_HOME/omaquake/`, `~/.local/share/omaquake/`, `~/.omaquake/` |
| `~/.quakespasm/`, `~/.quake/`, and the usual Steam Quake directories |
| `/usr/local/share/omaquake/`, `/usr/share/omaquake/`, `/usr/share/quake/`, `/usr/share/games/quake/`, `/opt/quake/` |

Each is tried both as the `id1` directory itself and as a directory
*containing* `id1`, and both `pak0.pak` and `PAK0.PAK` are accepted — the
shareware archive uses uppercase. If nothing is found, the error lists every
place it looked.

The pak has to live in a directory named `id1`: the engine locates the rest
of the game relative to it, so a pak elsewhere would load and then fail to
find `progs.dat`. An explicit path is honoured or refused, never quietly
replaced by a search result.

The full game's `pak1.pak` requires owning Quake and is not distributed
here.

None of this is necessary if you installed `omaquake-shareware-data` — it does
exactly the above at package build time.

## Running

    ./build/omaquake --demo                          # test pattern, no game data needed
    ./build/omaquake id1/pak0.pak                     # the real thing

Options, as reported by `--help`:

    --video=NAME     presentation backend: chafa, caca (whichever were built)
    --symbols=SET    ascii | block | fine        (default: fine)
    --color=DEPTH    mono | 16 | 256 | true      (default: true)
    --demo           render a test pattern instead of the game
    --find-pak       print the pak0.pak a normal run would load, then exit;
                     exit status 1 and nothing on stdout when none exists
    --keytest        show decoded key events; diagnoses input problems
    --frames=N       stop after N frames (demo/benchmark)
    --cell=WxH       character cell pixel size (default 10x20)
    --res=WxH        engine render resolution (default 320x200)
    --fps=N          presentation rate cap, 0 = every frame (default 30)
    --cells=WxH      cap the canvas; 0x0 fills the terminal
    --log=PATH       write the engine log here (never to stdout)
    --no-sound       do not open the audio device
    --help

`--symbols=ascii` restricts the glyph set to letters and punctuation for the
classic look; `fine` (the default) allows quadrant/sextant/octant glyphs,
which are still plain characters but resolve 2×4 sub-pixels per cell.

By default the canvas is capped to `res_w/2 × res_h/4` cells (160×50 for the
default 320×200 resolution), not the full terminal — see Performance below
for why. Pass `--cells=0x0` to fill the terminal anyway, or `--cells=WxH` for
an explicit cap.

## Controls

Quit: **Ctrl-Q**, **Ctrl-\\**, or **F10**. These are intercepted before the
engine ever sees them, from the decoded key event rather than a raw control
byte — under the kitty keyboard protocol Ctrl-\\ never arrives as a literal
0x1c byte, it arrives as `CSI 92;5u`, so a raw-byte check would silently stop
working the moment the protocol activates.

Quake 1 predates the WASD convention. Stock `default.cfg` has movement and
turning on the **arrow keys**, fire on **Ctrl**, strafe on **comma/period**,
and — surprisingly — `a` bound to `+lookup` and `d` to `+moveup`.

Copy `share/autoexec.cfg` to your `id1/` directory to get a modern layout:
it rebinds WASD to movement, raises `cl_yawspeed` from 140 to 280 (140 is
tuned assuming you have a mouse for fine aiming; keyboard-only turning at
that rate is painfully slow), and — critically — rebinds `LCTRL`/`LSHIFT`/
`LALT` rather than `CTRL`/`SHIFT`/`ALT`. This engine renumbers keys to match
libretro's `RETROK_*` values, which renamed the modifier keys; the stock
config's `bind CTRL +attack` (etc.) references a keyname that no longer
exists, `Key_StringToKeynum` fails, and the bind is dropped **silently** —
fire, run and strafe simply do nothing until you load `autoexec.cfg`.

### Mouse look

There are two pointer sources. `--mouse=auto` (the default) prefers evdev and
falls back to the terminal.

**evdev (`--mouse=evdev`)** reads the mouse directly from `/dev/input` and
takes it with `EVIOCGRAB`. This is the good one: the deltas are true unbounded
relative motion, so aiming is plain 1:1 like a native game, with no edge band
and no window boundary. The grab is exclusive, so while OmaQuake runs the
pointer freezes in place and cannot leave the terminal or stray onto another
monitor. Under Hyprland with `cursor:hide_on_key_press` (the default) the
pointer also disappears on your first keystroke and stays hidden, since no
motion ever reaches the compositor.

Requires read access to `/dev/input/event*` — on most distributions that means
membership of the `input` group. `--mouse-list` shows which devices are seen
and how each was classified. Only a device with `REL_X`, `REL_Y` and
`BTN_LEFT` is accepted, and anything advertising keyboard keys is refused
outright even if it also looks like a pointer — many gaming mice expose a
second keyboard node, and reading that would be keylogging.

A keyboard can also expose a completely genuine *pointer* interface — a Razer
BlackWidow presents one that udev labels `-event-mouse` with `ID_INPUT_MOUSE=1`
and the same five buttons as a real mouse, so no capability test separates
them. Such a node is detected by its name matching a keyboard's name exactly
(a keyboard's pointer node carries the keyboard's own name, whereas a mouse's
companion key node is suffixed "… Keyboard") and is skipped. Among whatever
remains, a device named like a mouse and having a horizontal wheel wins.
`--mouse-dev` still overrides all of this if you want a specific node.

**Ctrl-G** releases the pointer and gives it back to the desktop; press it
again to retake it. The grab is also dropped automatically whenever the
terminal loses focus and retaken when it returns, so alt-tabbing away just
works. Beyond that it is released on exit, on Ctrl-C, and by the kernel when
the process dies, so a `kill -9` always recovers your pointer. Quitting is on
the keyboard (Ctrl-Q / Ctrl-\ / F10), never the mouse.

**terminal (`--mouse=term`)** parses SGR-Pixels reports (mode 1016) from the
terminal itself. Portable — no special permissions, works over SSH — but it
only yields *absolute* positions, and terminals offer no pointer lock or warp,
so the pointer eventually reaches the window edge and cannot be re-centred.
To stay usable it blends 1:1 motion in the centre with a steering term in a
band along each edge (ramped quadratically, so entering the band is a nudge
rather than a step): push to the edge and hold to keep turning. Aiming feels
mouse-like; sustained 180s feel more like a good analog stick. Needs a
terminal that supports mode 1016 — Ghostty, kitty, foot and WezTerm do.

| option | default | |
|---|---|---|
| `--mouse=` | `auto` | `auto`, `evdev`, `term` or `none` |
| Ctrl-G | | release / retake the pointer while running |
| `--mouse-dev=PATH` | | force a specific evdev node |
| `--mouse-list` | | list input devices and their classification, then exit |
| `--no-mouse` | | same as `--mouse=none` |
| `--mouse-sens=F` | 2.0 | sensitivity |
| `--mouse-edge=F` | 0.15 | steering band, fraction of each side; terminal source only |
| `--mouse-turn=F` | 220 | steering rate at the edge, deg/sec; terminal source only |
| `--mouse-invert` | | invert pitch |

`--mouse-sens` is likely to want different values between the two sources:
terminal deltas are screen pixels, evdev deltas are raw mouse counts.

## Resolution

`--res=auto` (the default) renders at exactly the detail the cell grid can
display: one character cell resolves 2x4 pixels with sub-cell glyphs, so a
200x50 terminal renders at 400x200 and a 400x100 terminal at 800x400. Below
the engine's 320x200 floor it clamps.

Raising the engine resolution is nearly free — 1920x1200 still holds the
60 fps target — because presentation runs on another thread. Pin it with
`--res=640x400` if you want a fixed size, and use `--cells=WxH` to cap the
canvas below the terminal size (the picture is then centred).

## Audio

ALSA output is optional (built when `pkg-config --exists alsa` succeeds; see
`make backends`). Disable it with `--no-sound`, or point it at ALSA's null
sink with `OMAQUAKE_ALSA_DEVICE=null` — useful for CI or any run where you
don't want sound coming out of the machine's speakers. `OMAQUAKE_ALSA_DEVICE`
otherwise names any ALSA PCM device; it defaults to `default`.

The audio path never blocks the frame loop: writes are non-blocking and a
write that doesn't fit is simply dropped, because a blocking
`snd_pcm_writei()` would stall presentation in lockstep with the sound card.

## Performance

Presentation runs on its own thread, separate from the engine thread. The
engine needs roughly 16.7 ms per frame to hold its own timing target and
saturates one core doing that alone; character conversion and writing to the
terminal is real additional work and has to happen in parallel or the frame
pacing degrades.

The canvas is capped by default to `res_w/2 × res_h/4` cells because
sub-cell glyphs (the `fine` symbol set) already resolve 2×4 pixels per
character cell — filling a large terminal from a 320×200 source past that
point doesn't add detail, it just upscales the same 320×200 picture at
several times the conversion cost and several times the escape-sequence
volume the terminal has to parse.

Before the render thread and canvas cap existed, a 400×100 terminal ran at
**11.8 fps**, emitting **1.1 MB of escape sequences per frame**. With both in
place it holds roughly **61 fps**, independent of terminal size.

## Status

Playable: the engine boots, renders, takes keyboard input, and (when ALSA is
available) plays sound.

Known gaps:

- **Held keyboard keys are not released when the terminal loses focus.**
  Mouse buttons are; keys are not, so alt-tabbing mid-strafe can leave a key
  stuck down. Needs held-key tracking in `oq_input`.
- **The `CSI 14 t` text-area probe is untested against a live terminal.**
  Terminal mouse source only; evdev does not use it.
- **If the evdev device disappears** (unplug), aiming goes dead rather than
  falling back to the terminal source.
- The evdev grab is taken before the level loads, so the desktop pointer
  freezes for a second or two before the game is interactive.
- `--res=auto` is resolved once at startup. Resizing the terminal rescales
  the picture but does not re-render the engine at the new resolution.
- See `docs/DESIGN.md` for the remaining to-do list and the reasoning behind
  design choices made along the way.

## Licence

GPL-2.0-only, inherited from the Quake engine source this builds on.
