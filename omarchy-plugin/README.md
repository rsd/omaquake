# OmaQuake — Omarchy shell plugin

A bar widget for Omarchy 4 (Quattro) that drops Quake out of the top bar.

The popout is not a QML reimplementation of a terminal. It is a **real
terminal emulator** (`foot`) running the real `omaquake` binary, positioned by
Hyprland to sit exactly behind a transparent hole in a layer-shell surface.
The chrome around the hole is QML; everything inside it is a tty.

## Why it works

Verified on Hyprland 0.56.2 / Omarchy 4.0.0.alpha:

- A layer-shell surface on the **overlay** layer composites above toplevel
  windows, so any region the surface never paints shows the window beneath it.
- `mask` subtracts the hole from the input region, so clicks and keystrokes
  land on the terminal rather than being swallowed by the chrome.

Consequence: `omaquake` needs no changes at all. No grid protocol, no socket,
no cell renderer in QML. It stays a plain terminal program that does not know
it is inside a panel.

## Install

```
omarchy plugin add https://github.com/rsd/omaquake.git --enable
omarchy restart shell
```

`manifest.json` sits at the **repo root**, not in this directory. `omarchy
plugin add` clones a URL and validates the clone root, and there is no
subdirectory or branch-path syntax to point it deeper. Entry points, though,
may be any safe relative path, so the manifest stays at the root and names
`omarchy-plugin/Panel.qml`; the QML lives here. The whole engine repo is what
lands in `~/.config/omarchy/plugins/rsd.omaquake/` -- about 550 KB, since
`plugin add` does not fetch submodules.

If a copy-installed `rsd.omaquake` is already present, `add` refuses the id;
`omarchy plugin remove rsd.omaquake` first.

Installing by hand means cloning the repo to
`~/.config/omarchy/plugins/rsd.omaquake/`, not copying this directory --
copying it alone leaves the manifest behind.

**The restart is not optional for a newly added plugin.** The bar resolves
widgets through a registry built at shell startup, so a plugin that did not
exist when the shell started is discovered by `omarchy plugin list` and written
into `shell.json` while still never appearing on the bar. Neither
`omarchy-shell shell rescanPlugins` nor `omarchy-shell shell reloadConfig`
rebuilds that registry — both were tried, and the widget stayed invisible with
no error logged anywhere. Only a full restart registers it.

**Editing the QML in place does not hot-reload either**, whatever the shell's
own log says. Changing `appId` in the installed copy produced
`Local plugin changed, reloading: rsd.omaquake` in the journal, and the next
popout still spawned under the *old* app id; only `omarchy restart shell`
picked the edit up. Assume a restart after every QML change. Settings are the
exception — see below.

Do **not** install by symlinking: `omarchy plugin validate` rejects a symlink
anywhere inside a plugin folder (`.git` excepted). No symlink is needed now
anyway -- the installed plugin *is* a clone of this repo, so develop in
`~/.config/omarchy/plugins/rsd.omaquake/` directly and push from there. QML
edits hot-reload in place once the plugin is registered.

Remove with `omarchy plugin remove rsd.omaquake`.

## Settings

Set inline on the widget's entry in `~/.config/omarchy/shell.json`:

| Key | Default | Meaning |
|---|---|---|
| `cols` / `rows` | `100` / `30` | Terminal size **in cells**, not pixels |
| `cellW` / `cellH` | `8` / `17` | Assumed cell size in logical px |
| `padding` | `10` | Chrome ring thickness |
| `gap` | `6` | Space between bar and popout |
| `binary` | `omaquake` | Path to the binary |
| `gameDir` | *(none)* | Working directory holding `id1/` |
| `pak` | *(none)* | Path to `pak0.pak`; omit for the test pattern |

Sizing is in **cells** deliberately: fixing cells rather than pixels keeps
Quake framed identically across monitors of different scale.

Unlike the QML, these **do** apply live: editing `shell.json` re-reaches the
widget within a second or two, no restart involved. Verified by watching the
engine probe below re-run and flip its verdict as `binary` was edited back and
forth.

## When the engine is missing

A plugin cannot declare a dependency on `omaquake`. The manifest schema has no
field for one, and `omarchy plugin add` only clones and validates — it never
builds anything, runs a hook, or calls a package manager. So the widget checks
for itself: a `command -v` probe (which covers both a bare name on `PATH` and
an absolute path) runs at load and again whenever `binary` changes, and a click
with no engine present sends a notification pointing at `makepkg -si` and this
repo instead of opening a popout onto nothing. `launchTerminal()` carries the
same guard, so an IPC-driven open explains itself too.

## Hyprland 0.56 notes

Two behaviours this plugin has to work around, both found the hard way:

- **`move X Y` in a window rule is monitor-relative, not global.** Asking for
  `660,32` on a multi-monitor layout lands at the active monitor's origin plus
  that offset. The spawn rule therefore pins `monitor <name>` and passes
  monitor-local coordinates.
- **`hyprctl dispatch` is read as Lua under the Lua config manager** — Omarchy
  4's default. The request is compiled verbatim as `return hl.dispatch(...)`,
  wanting the `hl.dsp.*` API, so classic forms such as
  `dispatch movewindowpixel exact X Y,address:0x…` fail outright, and
  `dispatch exec [float; pin; …] cmd` is a Lua *syntax* error (`;` inside a
  bare `[...]`). The classic spelling only parses under a hyprlang config,
  which in turn has no `hl.dsp` table. The plugin therefore spawns with
  `hl.dsp.exec_cmd("[rules] cmd")` and falls back to the classic form whenever
  that reply is not `ok`. This went unnoticed for a while because the
  development machine carries a local `hyprctl` legacy-compat shim in
  `/usr/local/bin` that rewrote the classic form on the fly; a stock Omarchy 4
  install has no such shim, and there the popout flashed its chrome and died
  on the 5 s watchdog. Test dispatch behaviour against `/usr/bin/hyprctl`.

Windows are also translucent by default under Omarchy's rules, hence the
`opacity 1.0 override` in the spawn rule and `-o colors.alpha=1.0` on foot.

## Status

Working prototype, verified end to end: clicking the bar icon opens a popout
showing Quake running from `id1/pak0.pak`, and closing it tears down both the
terminal and the chrome surface with no strays left behind.

Closing on focus loss is implemented and verified: the popout arms itself only
once the terminal has actually taken focus, then tears down terminal and chrome
together as soon as focus leaves. Arming matters — between `open()` and foot
mapping its window the active toplevel is still whatever the user was in, and
an unarmed check slams the popout shut instantly. A 5s watchdog closes the
popout if the terminal never appears at all, so a bad `binary` path cannot
strand a chrome ring over an empty hole.

Known gaps:

- Input pass-through through the `mask` is designed but **not yet verified**
  end to end; the compositing half is confirmed by screenshot.
- A popout opened over IPC onto a monitor that is not the focused one never
  takes focus, so it never arms, and the watchdog closes it after 5s. Opening by
  clicking the bar icon does not have this problem — the click focuses that
  monitor, the terminal takes focus, and the popout stays up until focus leaves.
- Does not draw its own `PopupCard`, because `PopupCard` paints a background
  behind its content and would fill the hole. The chrome here is hand-rolled,
  so it tracks the theme only through `Color.popups.*`.
- The terminal is a real toplevel, so it appears in the window list.
- Installing pulls the engine's git history along with the QML; the plugin's
  version therefore moves whenever the engine is tagged. A standalone plugin
  repo would separate the two, at the cost of maintaining two repos.
