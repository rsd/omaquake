# Packaging notes

Decisions still open for the Arch recipe:

- **Engine source.** tyrquake is a git submodule. `makepkg` cannot fetch
  submodules itself, so either list it as a second `source=()` entry and
  rewire the submodule URL in `prepare()` (what the skeleton does), or vendor
  it at a pinned commit. The second option makes the package reproducible
  without network access to GitHub.
- **Licence.** tyrquake is GPL-2.0 (id's Quake source release). OmaQuake's
  own code must therefore be GPL-2.0-compatible; the skeleton declares
  `GPL-2.0-only`.
- **Game data.** `pak0.pak` cannot be shipped. Shareware data could be a
  separate `omaquake-shareware-data` package; retail data stays the user's
  problem. Decide on a default search path (`/usr/share/omaquake/id1`,
  `~/.local/share/omaquake/id1`).
- **`check()`.** The binary requires a tty, so any smoke test has to run
  under `script -qec`. Simplest is to keep `check()` to `--help`.
- **Optional deps.** chafa and libcaca are both optional at build time; the
  Makefile compiles whichever is present. For a distro package, hard-depend
  on both so the runtime `--video=` switch always works.
