# Packaging notes

Decisions still open for the Arch recipe:

- **Engine source.** *(settled)* tyrquake stays a submodule. `makepkg` cannot
  fetch submodules itself, so it is a second `source=()` entry and `prepare()`
  rewires the submodule URL at makepkg's own checkout. That entry needs no
  `#commit=` of its own: `git submodule update` checks out the gitlink commit
  this repo records, so the engine revision is pinned by the superproject.
- **Licence.** tyrquake is GPL-2.0 (id's Quake source release). OmaQuake's
  own code must therefore be GPL-2.0-compatible; the skeleton declares
  `GPL-2.0-only`.
- **Game data.** *(settled)* `pak0.pak` cannot be shipped in this repo, but
  the 1996 shareware release is freely redistributable, so the recipe fetches
  `quake106.zip` at build time and splits the result into a second package,
  `omaquake-shareware-data`, installing `/usr/share/omaquake/id1/pak0.pak`
  (already on `oq_find_pak`'s system search list). Retail `pak1.pak` stays the
  user's problem. Two things to know about the archive: the payload inside the
  zip is `resource.1`, a DOS self-extracting LHA blob that `bsdtar` reads
  directly (libarchive speaks LHA -- no `lhasa` or DOS emulator needed), and
  its members are uppercase `ID1/PAK0.PAK`, so `prepare()` renames to the
  lowercase layout the engine expects. Verified: the extracted pak is
  byte-identical (sha256 `35a9c55e...`) to the `id1/pak0.pak` used in
  development, and the binary renders from the packaged path.
  Cost of the split: every `makepkg` run downloads the 9 MB zip even when only
  the binary package is wanted -- `source=()` is per-pkgbase, not per-package.
- **`check()`.** The binary requires a tty, so any smoke test has to run
  under `script -qec`. Simplest is to keep `check()` to `--help`.
- **Optional deps.** chafa and libcaca are both optional at build time; the
  Makefile compiles whichever is present. For a distro package, hard-depend
  on both so the runtime `--video=` switch always works.

## Status: the recipe builds

`makepkg -f` completes from the public GitHub remote and produces `omaquake`, `omaquake-shareware-data` and
the automatic `omaquake-debug`; the packaged binary renders from the packaged
pak. Three things had to be right to get there, all easy to undo by accident:

- The git source must be **named** (`$pkgbase::git+file://...`), or the working
  copy is called after the checkout directory (`quake`) and every
  `cd "$srcdir/$pkgbase"` misses.
- `git submodule update` needs `-c protocol.file.allow=always`; git has refused
  `file://` submodule transport since CVE-2022-39253, and the submodule URL is
  rewired to makepkg's local tyrquake checkout.
- `alsa-lib` is a genuine link-time dependency of the binary, not merely an
  optional backend. namcap catches it if it goes missing.

The source is pinned to the `v0.1.1` tag. Bumping `pkgver` means tagging
first and moving the `#tag=` too.

Two loose ends, neither a blocker:

- The binary lacks FULL RELRO, because the Makefile does not pick up makepkg's
  `LDFLAGS`. It is the one thing between this and a namcap-clean package.
- `source=()` is per-pkgbase, so the 9 MB shareware zip is fetched even by
  someone who only wants the binary package.

## Published on AUR

`ssh://aur@aur.archlinux.org/omaquake.git` (https://aur.archlinux.org/packages/omaquake),
first pushed 2026-08-20 as 0.1.0-1. The AUR repo holds only `PKGBUILD`,
`.SRCINFO` and a `.gitignore`; `packaging/PKGBUILD` here is the source of
truth. To release: tag here, bump `pkgver`/`#tag=` (or `pkgrel`), copy the
PKGBUILD into a clone of the AUR repo, `makepkg --printsrcinfo > .SRCINFO`,
commit both, push to `master`. namcap's split-package check is what surfaced
the makedepends fix: per-package `depends` are not installed before `build()`,
and the Makefile drops a backend it cannot `pkg-config` without failing.
