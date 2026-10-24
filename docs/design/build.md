# Toolchains, builds and release gates

Status: design draft, 2026-10-20. No scripts, toolchains or binaries have
been added by this draft.

## 1. Purpose

Define the build skeleton that the package designs need before their M0
experiments. A developer must be able to prepare a fresh checkout, install
the named kits into its own `tools/`, and build one package without another
checkout, a global SDK search path or a network fetch during the build.

This document defines proposed interfaces and acceptance criteria. The
individual designs still decide their device coverage, driver model,
target systems and milestones. A successful compile is evidence of a
build, not of a working driver. Nothing in this draft changes the
README's "Tested by the author" table.

## 2. Toolchain inputs

The package scripts resolve the repository root from their own location.
All compiler, header and library paths are then rooted there. A missing
kit is a diagnostic naming that local path and the required kit; the
script must not search another checkout or download a replacement.
Host OS commands and any separately named host tools are recorded too;
this path rule applies to source, kit and project helper inputs, not to
the operating system that runs the build.

| Local root | Input | Used for |
|---|---|---|
| `tools/MSVC600/` | Licensed Visual C++ 6.0 installation; record service pack and compiler/linker versions | Legacy x86 C/C++, Win32 helpers and DLLs |
| `tools/ntddk/` | Windows 2000 DDK, with its documented build environment, headers and libraries | Legacy x86 WDM and NDIS 5 builds |
| `tools/win98ddk-extracted/` | Windows 98 DDK documentation, headers, import libraries and required VxD tools | 9x ABI evidence and the serial VxD |
| `tools/WinDDK71/` | WDK 7.1 installation, with its own compilers and libraries | amd64 builds and signing tools |

These are destination names, not claims that the kits are installed on
this host. The 98 DDK extraction may have flattened filenames; setup must
record the mapping to the published kit's paths rather than inventing a
different header layout for each package. The build must not recursively
add the kit's source directories to include or input searches.

Before a kit is accepted, the local setup record contains its product and
version, installation-media filename and SHA-256, installation or
extraction steps, executable versions, expected header/library paths, and
licence or acquisition requirements. Record only material that was
actually checked. Redistribution rights for the kits are not assumed;
their files stay under git-ignored `tools/`.

The proposed setup documentation is `docs/toolchain.md`, written when kit
preparation is implemented. It must include a small sentinel list for
each profile: compiler, linker, key header and import library. Preparation
can copy a legally held kit into these local destinations once; no build
script may retain the source location as a fallback.

Published documentation and headers are allowed design inputs. DDK/WDK
sample drivers, sample INFs and sample libraries are not read or used as
templates. Any setup script adapted from GPL-compatible code is copied
into this repository with its notices and a `NOTICE.md` row.

## 3. Build profiles

A profile selects an architecture, kit, OS-facing ABI and import policy.
It does not select a `free/` folder. Several release folders can contain
the same binary with different INFs; INF-only packages need no compiler.

| Proposed profile | Toolchain and ABI | Initial consumers |
|---|---|---|
| `wdmx86` | MSVC 6.0 + 2000 DDK, x86; 98 SE/ME compatibility checked separately | `usbmidi`, `uac2`, `uvc98`, `usbcom`, `xpad98` |
| `ndis5` | MSVC 6.0 + 2000 DDK, x86 NDIS 5.0, including the WDM lower edge | `cdcnet`; later `rtl815x`, `ax88179` |
| `wdm64` | WDK 7.1, WNET amd64 environment; XP x64 minimum | Packages whose own designs include amd64 |
| `ndis51` | WDK 7.1, WNET amd64 environment, NDIS 5.1 | `cdcnet` on XP x64 through 7 x64 |
| `win32` | MSVC 6.0, Win32 console/DLL; baseline imports declared by artifact | Codecs, port monitors and other supporting DLLs; the deferred installer's `WDMINST.EXE` |
| `win64` | WDK 7.1 user-mode amd64 toolchain; XP x64 minimum | Supporting x64 DLLs; the deferred installer's `WDMIN64.EXE` |
| `vxd9x` | 98 DDK VxD tools/headers + MSVC 6.0; assembler still to prove | `USBCOM.VXD`, after its design's M0 |
| `inf` | Package generation and INF checks only | `usbflop`, NT `usbcom`, XP-7 RNDIS |

These profiles are proposals to prove with the installed kits. Exact
compiler switches, entry-point decoration and library lists belong in
the package's build definition once verified. A script must not silently
replace a failed profile with a newer compiler or target.

Use a fresh child environment for every profile and architecture. Start
with the recorded kit's environment, then set explicit include and
library paths; reject contamination from a modern Visual Studio prompt.
Do not reuse x86 objects in amd64 builds, or a kernel library search path
for a Win32 tool. Keep free and checked objects separate too.

Shared class code is compiled into each package's binary from
`src/<class>/common/`. No common driver or runtime library is released
separately. Headers and source needed for that compilation are tracked
in this repository.

## 4. Package entry points and output

Every package has its own build entry point under
`src/<class>/<package>/`. The common proposed command contract is:

```
build.cmd <profile> <free|checked>
```

The script validates arguments, reports its supported profiles, invokes
only the selected build, and returns nonzero on any failed gate. An
INF-only build performs generation and validation with the `inf` profile.
There is no implicit "build all and publish" operation.

Intermediates and results go under
`out/<class>/<package>/<profile>/<free|checked>/`. Kit build utilities that
require package-local object directories may use git-ignored directories
there, then copy only successful results into `out/`. Each result has:

- the proposed binary or generated INF files;
- complete compiler/linker output and the exact command lines;
- a record of source commit and dirty state, profile, kit versions,
  preprocessor definitions, include/library paths and input hashes;
- import, PE/ABI and package-check reports with explicit pass/fail states.

A failed invocation cannot leave an old result marked successful. Each
invocation starts a new result record, and success is written only after
every selected gate passes. Logs and private symbols stay out of `free/`.
Stable inputs and logged commands are required; byte-identical rebuilding
is an experiment until timestamps and signing behaviour have been measured.

## 5. Import and ABI gates

### 5.1 Evidence for availability

Track machine-readable policies and their provenance under a proposed
`toolspec/` directory, outside ignored `tools/`. Each policy entry names
the target system and service pack, architecture, module and symbol (or
ordinal), evidence kind and source location. Evidence may be a published
header/import-library entry or an actual export-table reading. Preserve
which kind it is: a declaration or library stub alone is not proof that
the target exports the routine.

An export-table record identifies the binary filename, target system and
service pack, acquisition medium and table-reading method, explicitly
"export table, names and ordinals only". Do not scan binary strings,
read imports or version resources, disassemble, or dump whole Microsoft
or vendor binaries to derive these policies. A parser used for exports
must limit its input reads to the headers and export metadata needed to
locate and decode that table. Missing evidence remains an unresolved gate.

On 98 SE and ME a WDM driver's kernel and NDIS imports are resolved by
`NTKERN.VXD` and `NDIS.VXD`. These are VxDs (LE images), not PE images,
so they have no PE export table to read, and reading anything else of
them is outside what `AGENTS.md` allows. Evidence for those systems is
therefore the 98 DDK documentation and headers, plus a load test on the
guest: a test driver that imports the candidate routines either loads or
is refused by the loader, and the record says which. The load test
observes the system; it reads nothing of the Microsoft binary.

Keep policies per system rather than treating 98 SE as the universal
minimum. ME and the NT systems also require evidence for the imports the
package uses. A single binary offered across a range must pass the policy
of every tested system in that range (section 8). Where a non-tested
system's evidence is missing, such as ME's guest load test, the gate
records it as unresolved for that system, the release notes list it, and
the release is not blocked. NDIS, HID, streaming, audio and user-mode
imports need their own module policies; a kernel-only list is insufficient.

### 5.2 Checking our artifacts

For binaries built here, inspect the PE headers, normal and delay imports,
and linker map. Match imports by module and exact name/ordinal; a name
found in an unrelated module does not pass. Resolve any permitted
forwarder from export metadata with a bounded chain and evidence for the
destination. Unknown module, symbol, ordinal or forwarder fails the gate.
The gate also checks:

- machine type and PE32/PE32+ form against the selected profile;
- entry point, subsystem and minimum image versions against the profile;
- pointer-sized fields, structure packing and calling conventions at
  every OS-facing or shared IOCTL boundary;
- no accidental C++ runtime, modern CRT or compiler support import;
- static layout checks for the structures used at those boundaries.

Compiler helper routines must be accounted for in the linked result;
switching off a compiler check does not by itself establish compatibility.
Runtime optional API lookups need a recorded minimum OS and a complete
fallback path, since they are not proved by a static import check.

Microsoft documents DUMPBIN's `/IMPORTS` as reporting both normal and
delay imports. Use that on artifacts built here, or an equivalent bounded
PE parser; the Microsoft/vendor binary restriction in 5.1 still applies.
An import pass does not prove structure compatibility, cancellation or
runtime behaviour. The oldest guest's load and lifecycle tests remain
required.

## 6. First skeleton and M0 experiments

The first disposable driver belongs to an explicitly named test package,
proposed `src/generic/wdmsmoke/`, with no `free/` release. Write it from
the WDM documentation and headers. It needs only enough PnP handling to
attach to a deliberately selected test devnode, start, stop and remove;
it must not claim a USB class or a real device by a broad match.

Prove x86 `wdmx86` and amd64 `wdm64` separately. Record the selected guest,
test devnode, INF and expected dispatch path. A root-enumerated smoke
device proves loading and lifetime, not USB or NDIS operation. cdcnet's
separate M0 proves NDIS-WDM binding with QEMU's USB network device.

| Experiment | Evidence required before proceeding |
|---|---|
| Local kit preparation | Sentinel paths and tool versions recorded; source, kit and helper inputs resolve locally, with no path into another checkout |
| x86 compile/link | Clean rebuild, expected machine/entry point, imports and ABI report |
| amd64 compile/link | Clean rebuild in a separate environment, no x86 input, XP x64 baseline checked |
| Basic WDM lifetime | Guest install, start, stop, restart and remove; unload drains requests and frees allocations |
| NDIS-WDM lifetime | cdcnet M0 records actual lower device objects, initialization/halt and USB submission on each stack |
| Signing/install split | On x64 guests record package staging, device installation and driver load as three separate outcomes |
| Negative gates | Wrong-architecture input, unavailable import and missing package file each fail with a useful diagnostic |

A user-mode artifact's minimum OS is separately checked: an EXE or DLL
that builds on the host may still import an API missing on its oldest
target.

## 7. Packaging and release

Builds write only to `out/`. A separate release operation, owned by the
package, copies validated files and INFs into the systems folders named
in its design. The package release definition lists each destination,
architecture or INF-only status, required files, INF model policy,
version and validation reports. A release folder is one complete install.

Before publication, the release operation must:

1. Check package version and `DriverVer`, expected INFs, architecture,
   service names, source-file references and the correct systems folders.
   Check that generic matches are interface-level. A whole-device
   hardware ID is allowed only where the package design records it: a
   verified single-function device, or the documented serial composite
   exception. A whole-device class ID never stands in for missing
   interface grouping.
2. Check every user-visible file and folder under `free/` for 8.3 names,
   case-insensitive collisions and reserved DOS names. Any system-fixed
   long name needs its INF/system requirement recorded.
3. Check every package-supplied file each INF copies against the
   release file list: present in the folder, with nothing else in it but
   the declared documentation and support files (a `README.TXT`, for one).
   Files an INF takes from Windows (`LayoutFile`, `Include`/`Needs`) are
   checked separately against the package design's documented sources and
   prerequisites, including any Windows CD it says Setup will ask for.
   (The installer is deferred; when it returns, its package scripts are
   generated from the same file list.)
4. Apply the signing policy of the package. The owner's default is an
   embedded test signature on x64 drivers and unsigned x86 drivers;
   no catalog for packages with a binary of their own; INF-only packages
   such as `usbflop` may require a catalog of their own. This policy was
   first recorded in `installer.md` section 11 and holds while that
   design is deferred.
   A missing signing key produces a clearly unsigned development artifact,
   not a release reported as test-signed.
5. Verify the signed file's integrity and expected signer, recording the
   tool, algorithm and certificate thumbprint. Keep private keys and
   passwords out of the source tree and logs. A certificate table's
   presence is insufficient evidence of a valid signature.
6. Generate catalogs, when required, after all files they cover are final;
   any subsequent covered-file change requires catalog regeneration and
   signing. Record the catalog tool version and OS identifiers. WDK 7.1's
   ability to describe Windows 8 in catalog generation is unverified;
   do not invent an `/os` value. If it cannot, name an additional kit/tool
   locally and prove it before that catalog profile can ship.
   Verify each final signed catalog, its expected signer and its coverage
   of the final package bytes after this step; the image verification in
   step 5 does not cover a catalog created later.
7. Rewrite `SHA256.TXT` from every final file under `free/`, in stable
   path order, with one entry per file and no stale entries. Verify the
   manifest against the final tree. Hash after signatures and catalogs.
8. Update the README package row using recorded evidence: "Built, not
   tested" only when its release folder exists, "Tested" only with the
   real device(s), USB host stack and date. Commit the release and tag it
   `<package>-<version>` when a release is actually requested.

An embedded signature addresses the driver's load policy; PnP package
signing and staging have their own requirements. Test mode alone does not
prove unattended package installation. Preserve the owner's signing
policy while making any failed installation leg an explicit M0 finding
in the package design. No release note may call such a leg successful.

Nothing under `free/` is edited by hand. The release gate also checks that
`.gitattributes` preserves released bytes. Source documentation is LF;
released batch files and INFs get the encoding and line endings required
by the target and their package design.

## 8. Test records and remaining decisions

Proposed records live under `docs/testing/`. A run names the date, source
commit/dirty state, binary and INF hashes, OS edition/service pack and
architecture, physical machine or guest, USB controller and host stack
version, device VID/PID and revision, steps, expected and observed result,
and relevant log locations. A VM or synthetic-device pass is recorded as
such and cannot make a README cell "Tested".

Tested systems follow README's policy (Owner, 2026-10-20): 98 SE, 2000
SP4, XP SP3 x86 and 7 x64, each where the package targets it, plus ME for
`usbflop`. An M0 reading or test leg that only another system could
settle does not block a release. That system's folder ships as "Built,
not tested", and the package's release notes list the readings left
unverified there. Other guests are kept for those readings and for bug
reports.

Test guests are QEMU disk images in the git-ignored `vm/`, one fresh
install per system, copied per host like the kits: `w98se.qcow2` (98 SE
with NUSB 3.3), `wme.qcow2`, `w2k.qcow2` (2000 SP4), `wxp.qcow2`,
`wxp64.qcow2`, `wv86.qcow2`, `wv64.qcow2`, `w786.qcow2` and `w764.qcow2`.
There is no Windows 8 guest yet. Each run starts from a throwaway overlay
(`qemu-img create -b`) so the base stays fresh. Every package's runs
include an EHCI leg on each system with an EHCI stack (README, Owner,
2026-10-20), as well as UHCI/OHCI. An EHCI leg counts only when the
record shows EHCI carried the traffic: a High Speed device, or a Full or
Low Speed one behind a High Speed hub's transaction translator. A Full
Speed device on a QEMU EHCI port is handed to a companion controller and
is a UHCI leg. Where QEMU offers no High Speed model of the device, the
EHCI leg is real hardware and stays pending until it is run. The QEMU launch settings (accelerator, CPU model, memory)
per guest are measured on this host and recorded with the first run.

The first implementation must settle the kit inventory, exact profile
switches and library order, current-host execution of old build tools,
the 9x export/ABI evidence, amd64 XP baseline, VxD assembler compatibility,
and per-system signature algorithms and installation behaviour. These are
experiments, not additional owner decisions or claims of compatibility.

After those pass, implement packages in the wave order in the README
(`uac2` first, with the INF-only `usbflop` and `usbcom` `USBCDC.INF`); the installer is deferred. The smoke driver remains a build test; only an actual
package's completed milestones justify a release.

## 9. Sources

- This repository's `AGENTS.md`, README "Layout" and "Tested by the
  author", and each package's build/toolchain and milestone sections.
- Microsoft, [Using Visual Studio or MSBuild to build a driver](https://learn.microsoft.com/en-us/windows-hardware/drivers/develop/building-a-driver):
  MSBuild replaced `Build.exe` starting in WDK 8. The proposed legacy
  profiles therefore use their own kits' tools, not a modern project
  template as evidence of old-system support.
- Microsoft, [DUMPBIN /IMPORTS](https://learn.microsoft.com/en-us/cpp/build/reference/imports-dumpbin):
  import inspection of this project's built artifacts.
- Microsoft, [SignTool](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/signtool):
  embedded and catalog verification are distinct operations; record the
  target OS policy used for verification.
- Microsoft, [Installing test-signed driver packages](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/installing-test-signed-driver-packages):
  package integrity, certificate trust and test mode are separate conditions.

Historical kit command details must be checked against the installed
kit's documentation and headers before scripts are written. Current
Microsoft documentation is not evidence that a named API or tool option
existed on 98 SE, 2000 or WDK 7.1.
