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
