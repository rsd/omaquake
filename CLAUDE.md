# OmaQuake — rules for working in this repo

## stdout is the picture

This program's stdout **is** the rendered game frame, written as raw escape
sequences. Never `printf`/`puts`/write anything to stdout from any code path
that can run while the terminal is in presentation mode — one stray log line
corrupts the frame mid-escape-sequence. Diagnostics go to `--log=PATH`
(`oq_retro.c`'s `log_printf`) or to `stderr` only *after* `oq_term_shutdown()`
has restored the terminal. Same rule applies to anything ALSA might try to
print — see the silent error handler in `src/oq_audio.c`; do not remove it.

## Never run the game without silencing audio

Any command that launches `omaquake` in a non-interactive context (tests,
benchmarking, CI, this agent's own tool calls) **must** pass `--no-sound` or
set `OMAQUAKE_ALSA_DEVICE=null`. Without one of those it opens the default
ALSA device and plays sound out of the user's speakers.

## The binary requires a real tty

`oq_term_init()` refuses to start unless both stdin and stdout are ttys.
Test it via a pty, not by piping:

    script -qe -c "stty cols 200 rows 50; ./build/omaquake --demo --frames=60" /dev/null

**`COLUMNS`/`LINES` environment variables do not resize a pty.** Only `stty`
(or an ioctl) does. Forgetting this silently benchmarks/tests an 80x24
terminal regardless of what `COLUMNS`/`LINES` say, which has produced
misleading numbers before — always set size with `stty` inside the `script`
command, and confirm by checking what the program itself reports.

## Threading: writes to the terminal happen on the render thread only

`src/oq_render.c` is the only place that should be writing frame data to
stdout, from `render_loop()`. The engine thread (`retro_run()`,
`oq_retro.c`, `oq_main.c`'s game loop) must never write to the terminal
directly — two threads writing to the same fd can interleave mid escape
sequence and corrupt the frame. Even the clear-on-resize goes through
`oq_render_reconfigure()` so it executes on the render thread, not inline
in `video_sink()`.

## Building and checking what got compiled in

    git submodule update --init   # third_party/tyrquake
    make engine                    # builds the static libretro core
    make                            # builds ./build/omaquake
    make backends                   # reports which of chafa/caca/alsa were found

chafa, libcaca and ALSA are all optional and detected via `pkg-config`.
`make backends` is the source of truth for what's actually linked in — a
missing pkg-config file silently drops a backend, it does not fail the
build. Verify what's compiled in before assuming a feature (or its absence)
is real.

## Do not edit third_party/

`third_party/tyrquake` is a git submodule. Do not modify files inside it;
if the engine itself needs a change, that's a different, larger
undertaking than anything expected here.

## Code style

- 4-space indentation, K&R brace style.
- Must compile clean under `-Wall -Wextra -std=gnu99` (see `Makefile`
  `CFLAGS`) — no new warnings.
- Comments explain *why*, not *what* — see any of `src/oq_*.c` for the
  house style. Non-obvious constraints (threading, protocol quirks, ALSA
  timing) get a comment at the point a future change would plausibly get
  it wrong.

## This IS a git repository

The session's environment banner may claim otherwise — it is captured before
`git init` ran and is stale. `git log` works and the history is worth reading:
the commit messages carry the reasoning and the measured numbers behind most
of the non-obvious code here, and several of them document bugs that are easy
to reintroduce. Check with `git rev-parse --is-inside-work-tree` rather than
trusting the banner. (An agent has already been caught by this and skipped the
history entirely.)

## Verify, don't assume

Several bugs in this project's history survived review because the output
*looked* plausible (a frame that renders but is silently the wrong colours,
a keybind that's silently dropped, a canvas that's silently capped to
80x24). Prefer running the actual binary — via the `script`/`stty` pattern
above — over reasoning from source alone when a change touches terminal
I/O, input decoding, or timing.
