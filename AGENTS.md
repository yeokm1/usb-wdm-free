# AGENTS.md

The maintainer's rules for this repository. `README.md` says what it is.

## Standalone

- The repository works on its own at build, release and run time, and
  depends on no other repository.
- Everything a build needs is in this repository or named in it: no path
  into another repository, no file fetched from one at build time.
  Toolchains (DDKs, WDK 7.1, compilers) are installed per host into the
  git-ignored `tools/`, and the build documents what goes there.
- Code taken from xHCI98 is copied in, not referenced, and keeps its
  GPL-2.0 header.
- A driver's legs run under both host controller drivers: a USB 2.0
  (EHCI) one (Windows' own, or NUSB's on Windows 98 SE) and xHCI98
  (owner, 2026-10-22). A driver is not called ready until both have
  passed on its systems. The drivers are made for xHCI98 and may be
  bundled in its release downloads, but stay usable on their own.
- Every driver works on Windows 98 SE at minimum (owner, 2026-10-22),
  the webcam (`uvc98`) and UAC2 audio (`uac2`) above all, and every other
  driver too. A driver is not called ready until its 98 SE legs pass,
  under NUSB's EHCI and under xHCI98.

## Sources

As `README.md`, "Where the code comes from": specifications, Microsoft's
published driver kit documentation and headers, xHCI98's own code, and
GPL-2.0-compatible code (Linux, ReactOS) adapted with its notices and
SPDX tags kept and a row in `NOTICE.md` per file. Never a Microsoft or
vendor driver binary's code, its disassembly, or code whose licence does
not permit this use.

- The export table of a Microsoft binary that ships in a target system may
  be read, for function names and ordinals only, and what was read is
  recorded as such (which binary, which system, read as an export table).
  Nothing else of the binary is read.
- DDK and WDK sample sources (sample drivers, sample INFs, sample
  libraries) are not read. Where a design leaned on a sample, it cites the
  driver kit documentation or headers instead, or marks the point as
  needing a non-sample source.
- Besides kernel-mode drivers, a package may hold user-mode code, a VxD,
  or only an INF that binds a driver already in Windows, when it exists
  only to make a driver here work or to fill a gap Windows leaves.

## Source and binaries

- Source lives in `src/<class>/<package>/`; each package has its own build
  script and INFs there. Source shared by the packages of one class lives
  in `src/<class>/common/` and is compiled into each package's own binary
  by that package's build; it is never released on its own.
- A release copies the built files and their INFs into
  `free/<class>/<package>/<systems>/`, one complete install per folder, and
  rewrites `SHA256.TXT` to list every file under `free/`. Nothing under
  `free/` is edited by hand. A release is a commit and a tag
  (`<package>-<version>`).
- `<systems>` folder names follow the one scheme in `README.md`, "Layout".
- A release updates `README.md`'s "Tested by the author" table for its
  package. A cell becomes "Tested" only from a recorded run: the device(s),
  the USB host stack and the date.
- `.gitattributes` keeps `free/` byte for byte.

## Names

8.3 for every file and folder a user's machine sees (everything under
`free/`, and `SHA256.TXT`), unless an INF or the system fixes a longer name.

## Commits

No attribution trailer (`Co-Authored-By` and the like) in a commit message.
