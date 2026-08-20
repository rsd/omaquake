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
cp -r omarchy-plugin ~/.config/omarchy/plugins/raul.omaquake
omarchy plugin validate ~/.config/omarchy/plugins/raul.omaquake
omarchy plugin enable raul.omaquake
omarchy restart shell
```

**The restart is not optional for a newly added plugin.** The bar resolves
widgets through a registry built at shell startup, so a plugin that did not
exist when the shell started is discovered by `omarchy plugin list` and written
into `shell.json` while still never appearing on the bar. Neither
`omarchy-shell shell rescanPlugins` nor `omarchy-shell shell reloadConfig`
rebuilds that registry — both were tried, and the widget stayed invisible with
no error logged anywhere. Only a full restart registers it. Once registered,
editing the QML in place *does* hot-reload normally.

Do **not** install by symlinking the directory: `omarchy plugin validate`
rejects it outright with `symlinks are not allowed inside a plugin folder`.
Develop by editing the installed copy under `~/.config/omarchy/plugins/` and
copying back, or copy forward after each edit.

Remove with `omarchy plugin remove raul.omaquake`.

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

## Hyprland 0.56 notes

Two behaviours this plugin has to work around, both found the hard way:

- **`move X Y` in a window rule is monitor-relative, not global.** Asking for
  `660,32` on a multi-monitor layout lands at the active monitor's origin plus
  that offset. The spawn rule therefore pins `monitor <name>` and passes
  monitor-local coordinates.
- **`hyprctl dispatch` now compiles its arguments as Lua** (`hl.dispatch(...)`,
  wanting the `hl.dsp.*` dispatcher API). Classic forms such as
  `dispatch movewindowpixel exact X Y,address:0x…` fail outright.
  `dispatch exec "[rules] cmd"` still parses, which is why spawning goes
  through it.

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
- This directory is not a standalone git repo, so `omarchy plugin add <url>`
  cannot install it directly from the OmaQuake repository — the manifest has
  to sit at a repo root. Install by copy for now.
