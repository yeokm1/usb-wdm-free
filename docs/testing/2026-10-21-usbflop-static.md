# USB floppy INF static checks, 2026-10-21

The NT and 9x source INFs remain unchanged. These checks operate on the
original CB/CBI M0 experiments, not release packages. The already mounted
`out/storage/usbflop/inf/free/nt/` folder was not rewritten during this
work; only the checked stage was built. Guest and physical-media outcomes
are recorded separately by the hardware test, not inferred here.

## Policy gate and mutation tests

The original build gate counted recognizable model lines and looked for
dependency strings anywhere in the INF. It could miss an extra model line
with a different install section, or accept dependencies that were placed
in the wrong section. `validate.ps1` now checks the complete section set
and every non-comment entry. It verifies both model-to-transport and NT
DDInstall-to-service-section pairing, the target class/provider/signature,
architecture models, version syntax and required string definitions.
The 9x variant additionally checks the NTKERN loader, USBSTOR driver,
USBNTMAP hardware filter and each transport's DWORD DriverFlags. Extra
payload, registry or install sections fail closed. This small policy
checker is deliberately limited to this prototype package; it does not
implement Windows Setup's complete INF grammar.

```
powershell -NoProfile -ExecutionPolicy Bypass -File src\storage\usbflop\test.ps1
src\storage\usbflop\build.cmd inf checked
```

Both pass. The test accepts the NT/9x originals and case/spacing variations,
then rejects 27 mutations: broader class matching, Bulk-Only addition,
unrecognized model entries, wrong model transport, extra executable
payload, duplicate sections, invalid dates, missing strings, non-ASCII
text, crossed NT transport/service references, foreign includes, missing
AMD64 models, an extra NT hardware filter, wrong 9x transport flags,
mapping filter, loader or file-copy behavior. Build records now hash the
build scripts and validator as well as source and staged INFs.

Neither a static pass marker nor the checked flavour constitutes a target
installation, signed catalog or driver load test. Both flavours contain
the same INF bytes; the flavour separates build records.

## WDK checker result

The installed WDK 7.1 ChkInf tool was run against the NT source INF:

```
tools\WinDDK71\tools\Chkinf\perl.exe -Itools\WinDDK71\tools\Chkinf tools\WinDDK71\tools\Chkinf\chkinf.bat src\storage\usbflop\nt\USBFLOP.INF /O out\storage\usbflop\chkinf
```

ChkInf 2.5 (2002-10-23) fails with two errors, no warnings:

| Diagnostic | Experimental state |
|---|---|
| E22.1.1081, CatalogFile required for WHQL digital signature | Catalog/signing intentionally absent from this M0 stage. |
| E22.1.1017, primary revision must be greater than zero | DriverVer remains the prototype `0.0.0.1`. |

The report is retained under git-ignored `out/storage/usbflop/chkinf/`.
These diagnostics are not suppressed and this is not a ChkInf pass.
The checker does not prove target copy-source resolution, 2000 decoration
selection or installation acceptance on x64.

## Include/Needs and file-source limits

The source facts were already read and recorded in
[floppy.md](../design/floppy.md) sections 2 and 4: NT systems provide
`USBSTOR_CBI.NT`/`.Services` and `USBSTOR_CB.NT`/`.Services`; their install
sections select the transport flags and copy Microsoft's own driver.
Keeping both dependencies on the same transport lets each target retain
its own service definition. No local service or binary is introduced.
No target Microsoft binary or DDK/WDK sample was read for these checks.

Microsoft's [DDInstall documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/inf-ddinstall-section)
defines the Include/Needs processing used here. Its
[source/target location documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/specifying-the-source-and-target-locations-for-device-files)
describes included source-media sections and requires copy destinations
to be specified with the copy list. This package delegates copying to the
included target INF's own list and destination; it has no direct copy list
and does not invent a package-local source for USBSTOR.SYS.

Those documented mechanisms support the design, but do not demonstrate
old-target behavior. In particular, a successful bind while USBSTOR.SYS
is already present may avoid the path that locates a missing source file.
It cannot by itself establish fresh-copy lookup from 2000/XP layout/CABs
or Vista+ driver-store handling. M0 must record copy prompts, source paths,
errors and Setup logs for the actual target. A failure there requires a
tested source-resolution change, not a claim that the static gate passed.

For 9x the first-cut INF copies nothing. Its precondition is the complete
storage/mapping stack and floppy child binding already installed by ME or
the 98 SE storage driver set. Whether a fresh ME installation supplies all
those files before the first USB storage device remains the separate T1
gate; the copy-free prototype does not settle it.

The current Mitsumi fixture is CBI and already in Windows' hardware-ID
list. It does not establish CB transport, Bulk-Only support, automatic
selection for an unlisted drive, media-format support or an EHCI leg.
