# Release definition for usb-wdm-free 0.1.10 (docs/design/build.md section 7),
# read by release\release.ps1: release 0.1.9 plus
# - uac2 0.1.11.0 and uac1 0.1.1.0: the shared transport resyncs a stalled
#   isochronous stream at explicit bus frames instead of ending it, its bound
#   counting delivered audio only (docs/design/uac2.md 26.13); an open waits
#   up to 2 s for the previous stream's queued destroy (uac2.md 27); uac2 and
#   uac1 named without "(experiment)" (uac2.md 21.3); uac1 0.1.1.0 also runs
#   PCM8, IEEE float, A-law and mu-law, MaxPacketsOnly and adaptive sources
#   (26.12), on a COPY line, not STAGE: uac1 is for testing 98 SE in a
#   virtual machine (owner, 2026-11-09);
# - rtl815x 0.2.2.0, ax88179 0.1.4.0, ax88772 0.1.1.0, sr9700 0.1.1.0 and
#   cdcnet 0.1.4.0: the Advanced tab (Energy-Efficient Ethernet, speed and
#   duplex, flow control, offloads; ethernet.md 4.10) in the shared network
#   core; cdcnet also retries a failed resume bring-up;
# - uvc98 0.1.3.0: a resync at explicit start frames after a start-frame
#   refusal (uvc.md 5.4a).
# The images of those packages are new builds from out\ (x86 unsigned; the x64
# ones signed by the owner, -X64Dir). usbmidi, xpad98 and absptr are 0.1.9's
# files from the tree (their x64 images as strip: entries), as is every other
# package.
#
# Per package: Class/Name, Kind (CLASS or DEVICE: bound by class ids or by
# VID/PID), Desc (the STAGE.TXT PACKAGE words), DeviceIds (whole-device
# hardware ids the package design records as allowed), and Folders. Per
# folder: Name, Arch (x86, x64 or inf), Files (published name = source under
# out\, or 'tree:<path>' for a file already in the tree), Extra (declared
# support files no INF copies), and per INF its STAGE.TXT line (Action,
# Systems, Note). Sources are relative to the repository root.
@{
    Version = '0.1.10'
    # INF copies kept in the tree that must equal their original but for the
    # Signature line and comments (uac2's x64 INF: 64-bit Windows refuses $CHICAGO$).
    Agree = @{ 'src\audio\uac2\x64\uac2wdm.inf' = 'src\audio\uac2\uac2wdm.inf' }
    # The unsigned form of each x64 image the owner signs ('x64:<name>' in Files,
    # one per folder): a build under out\, or 'strip:<path>', an earlier release's
    # image in the tree with its signature removed (its image digest unchanged).
    X64Builds = @{ 'uac2.wxp-x64.uac2wdm.sys' = 'out\audio\uac2\wdm64\free\uac2wdm.sys'
                   'uac2.wv-8-x64.uac2wdm.sys' = 'out\audio\uac2\wdm64\free\uac2wdm.sys'
                   'uac2.wxp-x64.UACCI1.DLL' = 'strip:free\audio\uac2\wxp-x64\UACCI1.DLL'
                   'uac2.wv-8-x64.UACCI1.DLL' = 'strip:free\audio\uac2\wv-8-x64\UACCI1.DLL'
                   'rtl815x.wxp-x64.RTL815X.SYS' = 'out\net\rtl815x\ndis51\free\rtl815x.sys'
                   'rtl815x.wv-7-x64.RTL815X.SYS' = 'out\net\rtl815x\ndis51\free\rtl815x.sys'
                   'ax88772.wxp-x64.AX772F.SYS' = 'out\net\ax88772\ndis51\free\ax772f.sys'
                   'ax88772.wv-7-x64.AX772F.SYS' = 'out\net\ax88772\ndis51\free\ax772f.sys'
                   'sr9700.wxp-x64.SR9700F.SYS' = 'out\net\sr9700\ndis51\free\sr9700f.sys'
                   'sr9700.wv-7-x64.SR9700F.SYS' = 'out\net\sr9700\ndis51\free\sr9700f.sys'
                   'usbmidi.wxp-x64.usbmidi.sys' = 'strip:free\audio\usbmidi\wxp-x64\usbmidi.sys'
                   'usbmidi.wv-8-x64.usbmidi.sys' = 'strip:free\audio\usbmidi\wv-8-x64\usbmidi.sys'
                   'xpad98.wxp-x64.xpad98.sys' = 'strip:free\hid\xpad98\wxp-x64\xpad98.sys'
                   'xpad98.wv-x64.xpad98.sys' = 'strip:free\hid\xpad98\wv-x64\xpad98.sys'
                   'absptr.xp-7-x64.absptr.sys' = 'strip:free\hid\absptr\xp-7-x64\absptr.sys'
                   'cdcnet.wxp-x64.CDCNET.SYS' = 'out\net\cdcnet\ndis51\free\cdcnet.sys'
                   'cdcnet.wv-7-x64.CDCNET.SYS' = 'out\net\cdcnet\ndis51\free\cdcnet.sys' }
    Packages = @(
        @{
            Id = 'audio/uac2'; Kind = 'CLASS'
            Desc = 'USB audio (uac2): USB Audio Class 2.0 sound cards and DACs'
            # uac2.md section 3: the X4's whole-device id, owner's request 2026-10-22.
            DeviceIds = @('USB\VID_041E&PID_3278', 'USB\VID_041E&PID_3278&MI_03')
            Services = @('uac2wdm')
            Folders = @(
                @{ Name = 'w98se-me'; Arch = 'x86'
                   Files = @{ 'uac2wdm.sys' = 'out\audio\uac2\wdmx86\free\uac2wdm.sys'; 'uac2wdm.inf' = 'out\audio\uac2\wdmx86\free\uac2wdm.inf' }
                   Stage = @{ 'uac2wdm.inf' = @('STAGE', 'W98SE WME', '0.1.11.0: an open waits up to 2 s for the previous stream''s queued destroy (uac2.md 27; found on ME under its own UHCI, record 2026-11-09 uac-uhci-stalls); a stalled stream resynced at explicit frames (uac2.md 26.13, record 2026-11-09 uac-uhci-stalls); 0.1.10.0, released build E2 (2d336fa, built at f70621a): on 98 SE under NUSB 3.3 hs24 8 of 8, Full Speed 5 of 6 (simulator, record 2026-11-07 uac2-iso-ehci); earlier builds of 0.1.10.0: no KS file prompts with the INF of 828ea8c (play and record on a clean 98 SE, over 0.1.8 after Device Manager Remove and a replug, and on ME; record 2026-11-07 98se-ks-prompts); Full Speed 3- and 4-byte feedback with build 3d1a5cb on 98 SE under xHCI98 3.4.1.0 and NUSB on UHCI (simulator): played, recorded, survived an unplug, followed the feedback; the INF change of 35bceb4 (SetupPreferredAudioDevices): a new device takes playback and recording on 98 SE under xHCI98 3.4.1.0 and NUSB EHCI (simulators); 0.1.8.26: names on 98 SE (simulator, xHCI98 3.4.0.0); the queue and the real-time worker on 98 SE and ME (simulator, builds 20 to 24, paired CPU-load sets); ME: the release kit by Update Driver (Have Disk) from 0.1.6.0, a cold start, then the product name shown, a play and WorkerPriority 16 (simulator, xHCI98 3.4.0.0); 0.1.6.0: the play position is the fetch cursor (the seek fix; its legs on XP, 2000 and 7, none on 98 SE or ME); 0.1.5.0: playback, capture and ramp on ME with Windows own UHCI (simulator, Full Speed); 98 SE own UHCI: plays and records, known issue: the first start can hang (use NUSB or xHCI98); earlier: every simulator profile under xHCI98') } }
                @{ Name = 'w2k-xp'; Arch = 'x86'
                   Files = @{ 'uac2wdm.sys' = 'out\audio\uac2\wdmx86\free\uac2wdm.sys'; 'uac2wdm.inf' = 'out\audio\uac2\wdmx86\free\uac2wdm.inf'; 'UACCI1.DLL' = 'tree:free\audio\uac2\w2k-xp\UACCI1.DLL'; 'UACCI.INF' = 'tree:src\audio\uac2\uacci.inf' }
                   Stage = @{ 'uac2wdm.inf' = @('STAGE', 'WXP', '0.1.11.0: an open waits up to 2 s for the previous stream''s queued destroy (uac2.md 27; found on ME under its own UHCI, record 2026-11-09 uac-uhci-stalls); a stalled stream resynced at explicit frames (uac2.md 26.13, record 2026-11-09 uac-uhci-stalls); 0.1.10.0, released build E2 (2d336fa, built at f70621a): XP under Windows EHCI and UHCI x4full and x4, 3 rounds, every play passed (simulator, record 2026-11-07 uac2-iso-ehci); no 2000 leg of E2; earlier builds of 0.1.10.0: Full Speed 3- and 4-byte feedback with build 3d1a5cb on XP under xHCI98 3.4.1.0 (simulator); known issue: plays under XP own UHCI fail at Full Speed, cause open; 0.1.8.26: the release kit by Have Disk on 2000 (from 0.1.6.0; the restart asked, then a cold start) and XP x86 (from 0.1.8.0, no restart), then the product name shown, a play and WorkerPriority 16 (simulator, xHCI98 3.4.0.0); the volume top and the names passed on XP x64 (simulator, the same code); the queue and the real-time worker on XP and 2000 (simulator, 2026-11-04); 0.1.6.0: the seek fix passed on XP under Windows EHCI and xHCI98 and on 2000 (simulator); 2000 asks for a restart at install, so Update Driver from Windows USB Audio Device no longer stops it, and XP installs with no restart (simulator); 0.1.5.0 on XP under xHCI98: ALC4080 capture replayed (three outputs, three inputs, play/rec/ramp each), X4 gate (simulated and real X4 via VM); 2000: co-installer leg only')
                              'UACCI.INF' = @('COPY', 'W2K WXP', 'the co-installer, set up by hand (DefaultInstall); on 2000 and XP (simulator, its test build) a UAC 2.0 function got uac2, never usbaudio') } }
                @{ Name = 'wv-8-x86'; Arch = 'x86'
                   Files = @{ 'uac2wdm.sys' = 'out\audio\uac2\wdmx86\free\uac2wdm.sys'; 'uac2wdm.inf' = 'out\audio\uac2\wdmx86\free\uac2wdm.inf'; 'UACCI1.DLL' = 'tree:free\audio\uac2\wv-8-x86\UACCI1.DLL'; 'UACCI.INF' = 'tree:src\audio\uac2\uacci.inf' }
                   Stage = @{ 'uac2wdm.inf' = @('STAGE', 'W7', '0.1.11.0: an open waits up to 2 s for the previous stream''s queued destroy (uac2.md 27; found on ME under its own UHCI, record 2026-11-09 uac-uhci-stalls); a stalled stream resynced at explicit frames (uac2.md 26.13, record 2026-11-09 uac-uhci-stalls); 0.1.10.0, released build E2 (2d336fa, built at f70621a): 7 x86 x4full (first plug, replug, restart) and multi passed, fs failed as with 0.1.8.26 (simulator, record 2026-11-07 uac2-iso-ehci); 8 x86 uac3dual passed with build E (b2fb4d8); no Vista, 8 or 8.1 leg of E2; 0.1.8.26: names and the devnode name on 7 x86 (simulator); the queue and the real-time worker on Vista, 7, 8 and 8.1 x86 (simulator, builds 22 to 24); 0.1.6.0: the seek fix passed on 7 x86 (simulator); 0.1.5.0: co-installer first plug on Vista, 7, 8 and 8.1 x86 (simulator; playback on Vista, 7 and 8.1); earlier: playback and capture on 7 x86 (simulator, real X4 via VM)')
                              'UACCI.INF' = @('COPY', 'WVISTA W7 W8', 'the co-installer, set up by hand (DefaultInstall); on Vista and 7 x86 (simulator, its test build) a UAC 2.0 function got uac2, never usbaudio; 8 and 8.1 x86 did not need it') } }
                @{ Name = 'wxp-x64'; Arch = 'x64'
                   Files = @{ 'uac2wdm.sys' = 'x64:uac2.wxp-x64.uac2wdm.sys'; 'uac2wdm.inf' = 'tree:src\audio\uac2\x64\uac2wdm.inf'; 'UACCI1.DLL' = 'x64:uac2.wxp-x64.UACCI1.DLL'; 'UACCI.INF' = 'tree:src\audio\uac2\uacci.inf' }
                   Stage = @{ 'uac2wdm.inf' = @('STAGE', 'WXP64', '0.1.11.0: an open waits up to 2 s for the previous stream''s queued destroy (uac2.md 27; found on ME under its own UHCI, record 2026-11-09 uac-uhci-stalls); a stalled stream resynced at explicit frames (uac2.md 26.13, record 2026-11-09 uac-uhci-stalls); 0.1.10.0, released build E2 (2d336fa, built at f70621a): no XP x64 leg of E2; earlier: the INF change of 35bceb4 (SetupPreferredAudioDevices) read on XP x64 with the real X4 and X1 and simulators; 0.1.8.26: the volume top at 0 dB and the names on XP x64 (simulator, unsigned build of the same code); the queue and the real-time worker on XP x64 (simulator, 2026-11-04); the seek fix: simulator legs on XP x64 (2026-11-04): the INF staged by SetupCopyOEMInf with UACCI.INF, the first plug chose uac2 with no restart prompt; play, rec, ramp and the seek test passed under xHCI98 3.4.0.0 and Windows EHCI; 0.1.5.0: co-installer, play, rec, ramp and the ALC4080 capture passed on XP x64 (simulator)')
                              'UACCI.INF' = @('COPY', 'WXP64', 'the co-installer, set up by hand (DefaultInstall); passed on XP x64 (simulator, its test build)') } }
                @{ Name = 'wv-8-x64'; Arch = 'x64'
                   Files = @{ 'uac2wdm.sys' = 'x64:uac2.wv-8-x64.uac2wdm.sys'; 'uac2wdm.inf' = 'tree:src\audio\uac2\x64\uac2wdm.inf'; 'UACCI1.DLL' = 'x64:uac2.wv-8-x64.UACCI1.DLL'; 'UACCI.INF' = 'tree:src\audio\uac2\uacci.inf' }
                   Stage = @{ 'uac2wdm.inf' = @('COPY', 'WVISTA64 W7X64 W8X64', 'copy only (owner, 2026-10-24); test signing on; 0.1.11.0: an open waits up to 2 s for the previous stream''s queued destroy (uac2.md 27; found on ME under its own UHCI, record 2026-11-09 uac-uhci-stalls); a stalled stream resynced at explicit frames (uac2.md 26.13, record 2026-11-09 uac-uhci-stalls); 0.1.10.0, released build E2 (2d336fa, built at f70621a): no Vista to 8.1 x64 leg of E2; 0.1.8.26: no Vista to 8.1 x64 leg of build 26 at preparation; 0.1.6.0 source signed in the guest with a throwaway certificate: first plug with no restart prompt and the seek test on 7 x64 (simulator); 0.1.5.0 source signed in the guest with a throwaway certificate (a catalog on 8 and 8.1): co-installer, play, rec, ramp and the ALC4080 capture passed on Vista, 7, 8 and 8.1 x64 (simulator)')
                              'UACCI.INF' = @('COPY', 'WVISTA64 W7X64 W8X64', 'the co-installer, set up by hand (DefaultInstall); passed on Vista to 8.1 x64 (simulator, its test build)') } }
            )
        }
        @{
            Id = 'audio/uac1'; Kind = 'CLASS'
            Desc = 'USB audio (uac1, Windows 98 SE): USB Audio Class 1.0 sound cards, headsets, mics'
            DeviceIds = @()
            Services = @()
            Folders = @(
                @{ Name = 'w98se'; Arch = 'x86'
                   Files = @{ 'uac1wdm.sys' = 'out\audio\uac1\wdmx86\free\uac1wdm.sys'; 'uac1wdm.inf' = 'out\audio\uac1\wdmx86\free\uac1wdm.inf' }
                   Stage = @{ 'uac1wdm.inf' = @('COPY', 'W98SE', 'copy only: for testing 98 SE in QEMU (owner, 2026-11-09); 0.1.1.0: an open waits up to 2 s for the previous stream''s queued destroy (uac2.md 27, the code uac2 shares; found with uac2 on ME under its own UHCI, record 2026-11-09 uac-uhci-stalls); PCM8, IEEE float, A-law and mu-law, MaxPacketsOnly and adaptive sources run (design uac2.md 26.12, record 2026-11-09 uac1-formats); 0.1.0.1 legs: record 2026-11-07 uac1-98se') } }
            )
        }
        @{
            Id = 'audio/usbmidi'; Kind = 'CLASS'
            Desc = 'MIDI (usbmidi): USB MIDI 1.0 keyboards and interfaces'
            DeviceIds = @()
            Services = @('usbmidi')
            Folders = @(
                @{ Name = 'w98se'; Arch = 'x86'
                   Files = @{ 'usbmidi.sys' = 'tree:free\audio\usbmidi\w98se\usbmidi.sys'; 'usbmidi.inf' = 'tree:free\audio\usbmidi\w98se\usbmidi.inf' }
                   Stage = @{ 'usbmidi.inf' = @('STAGE', 'W98SE', '0.1.9: the 9x INF has Windows install its own audio components (no KS file prompts), the same change as uac2; on 98 SE under Windows UHCI a clean install and an install over 0.1.8 (Remove, replug, restart) asked for no KS file and the MIDI loopback passed (simulator, record 2026-11-07 98se-ks-prompts); ME not run; UHCI, xHCI98 and NUSB EHCI legs passed') } }
                @{ Name = 'wme'; Arch = 'x86'
                   Files = @{ 'usbmidi.sys' = 'tree:free\audio\usbmidi\wme\usbmidi.sys'; 'usbmidi.inf' = 'tree:free\audio\usbmidi\wme\usbmidi.inf' }
                   Stage = @{ 'usbmidi.inf' = @('COPY', 'WME', '0.1.9: the 9x INF change of uac2 (no KS file prompts); by-hand INF (USBMIDI_BY_HAND only); Windows usbaudio serves the rest') } }
                @{ Name = 'w2k'; Arch = 'x86'
                   Files = @{ 'usbmidi.sys' = 'tree:free\audio\usbmidi\w2k\usbmidi.sys'; 'usbmidi.inf' = 'tree:free\audio\usbmidi\w2k\usbmidi.inf' }
                   Stage = @{ 'usbmidi.inf' = @('STAGE', 'W2K', 'UHCI, EHCI and xHCI98 legs passed') } }
                @{ Name = 'wxp'; Arch = 'x86'
                   Files = @{ 'usbmidi.sys' = 'tree:free\audio\usbmidi\wxp\usbmidi.sys'; 'usbmidi.inf' = 'tree:free\audio\usbmidi\wxp\usbmidi.inf' }
                   Stage = @{ 'usbmidi.inf' = @('COPY', 'WXP', 'by-hand INF (USBMIDI_BY_HAND only)') } }
                @{ Name = 'wv-8-x86'; Arch = 'x86'
                   Files = @{ 'usbmidi.sys' = 'tree:free\audio\usbmidi\wv-8-x86\usbmidi.sys'; 'usbmidi.inf' = 'tree:free\audio\usbmidi\wv-8-x86\usbmidi.inf' }
                   Stage = @{ 'usbmidi.inf' = @('COPY', 'WVISTA W7 W8', 'by-hand INF (USBMIDI_BY_HAND only)') } }
                @{ Name = 'wxp-x64'; Arch = 'x64'
                   Files = @{ 'usbmidi.sys' = 'x64:usbmidi.wxp-x64.usbmidi.sys'; 'usbmidi.inf' = 'tree:free\audio\usbmidi\wxp-x64\usbmidi.inf' }
                   Stage = @{ 'usbmidi.inf' = @('COPY', 'WXP64', 'by-hand INF (USBMIDI_BY_HAND only); no x64 leg') } }
                @{ Name = 'wv-8-x64'; Arch = 'x64'
                   Files = @{ 'usbmidi.sys' = 'x64:usbmidi.wv-8-x64.usbmidi.sys'; 'usbmidi.inf' = 'tree:free\audio\usbmidi\wv-8-x64\usbmidi.inf' }
                   Stage = @{ 'usbmidi.inf' = @('COPY', 'WVISTA64 W7X64 W8X64', 'by-hand INF (USBMIDI_BY_HAND only); no x64 leg; test-signed') } }
            )
        }
        @{
            Id = 'video/uvc98'; Kind = 'CLASS'
            Desc = 'Webcams (uvc98): USB Video Class 1.0 and 1.5 cameras'
            # uvc.md section 13 decision 3: per-camera lines on request (the Brio 100).
            DeviceIds = @('USB\VID_046D&PID_094C', 'USB\VID_046D&PID_094C&MI_00')
            Services = @('uvcm2')
            Folders = @(
                @{ Name = 'w98se-me'; Arch = 'x86'
                   Files = @{ 'uvcm2.sys' = 'out\video\uvc98\m2x86\free\uvcm2.sys'; 'UVCM298.INF' = 'out\video\uvc98\m2x86\free\UVCM298.INF' }
                   Stage = @{ 'UVCM298.INF' = @('STAGE', 'W98SE', '0.1.3.0: a resync at explicit start frames after a refusal (uvc.md 5.4a); xHCI98 legs passed (simulator to 640x480, real camera via VM at 160x120); ME has no leg') } }
                @{ Name = 'w2k'; Arch = 'x86'
                   Files = @{ 'uvcm2.sys' = 'out\video\uvc98\m2x86\free\uvcm2.sys'; 'UVCM2.INF' = 'out\video\uvc98\m2x86\free\UVCM2.INF' }
                   Stage = @{ 'UVCM2.INF' = @('STAGE', 'W2K', '0.1.3.0: a resync at explicit start frames after a refusal (uvc.md 5.4a); Windows EHCI legs passed (simulator, real camera via VM to 320x240)') } }
            )
        }
        @{
            # CLASS: USBCDC.INF's XP and later models match USB\Class_02&SubClass_02.
            Id = 'serial/usbcom'; Kind = 'CLASS'
            Desc = 'USB serial (usbcom): CDC ACM serial ports and modems'
            # serial.md section 5.2: the serial composite exception; boards listed whole.
            DeviceIds = @('*')
            Services = @('usbser')
            WindowsFiles = @('usbser.sys')
            # USBCDC.INF: Include=mdmcpq.inf (serial.md section 5).
            WindowsSections = @('FakeModemCopyFileSection')
            Folders = @(
                @{ Name = 'w98se-me'; Arch = 'x86'
                   Files = @{ 'USBCOM.SYS' = 'tree:free\serial\usbcom\w98se-me\USBCOM.SYS'; 'USBCOMV.VXD' = 'tree:free\serial\usbcom\w98se-me\USBCOMV.VXD'
                              'USBCOM.INF' = 'tree:src\serial\usbcom\USBCOM.INF'; 'USBCOMP.INF' = 'tree:src\serial\usbcom\USBCOMP.INF' }
                   Stage = @{ 'USBCOM.INF' = @('STAGE', 'W98SE WME', 'the USB device; 0.1.5.0 chip modules (FT-X, FT232H, FT4232H, PL2303, CP2105, CP2108) passed under xHCI98 (98 SE and ME) and NUSB EHCI (98 SE; a 64 KiB echo failed once on four parts there) (simulator); CDC-ACM boards as before')
                              'USBCOMP.INF' = @('STAGE', 'W98SE WME', 'its COM port (USBCOM\COMPORT)') } }
                @{ Name = 'w2k-8'; Arch = 'inf'
                   Files = @{ 'USBCDC.INF' = 'tree:src\serial\usbcom\USBCDC.INF' }
                   Stage = @{ 'USBCDC.INF' = @('STAGE', 'W7X64', 'binds Windows usbser.sys; 7 x64 echoes passed; 2000 and XP copy only (owner, 2026-10-24)') } }
            )
        }
        @{
            Id = 'net/cdcnet'; Kind = 'CLASS'
            Desc = 'USB Ethernet (cdcnet): CDC ECM, CDC NCM and RNDIS adapters'
            # ethernet.md section 5.1: the Linux Ethernet gadget's default id (QEMU usb-net),
            # a class 02 whole device with no interface split, bound whole as the test INFs did.
            DeviceIds = @('USB\VID_0525&PID_A4A2')
            Services = @('cdcnet')
            Folders = @(
                @{ Name = 'w98se-me'; Arch = 'x86'
                   Files = @{ 'CDCNET.SYS' = 'out\net\cdcnet\ndis5\free\cdcnet.sys'; 'CDCNET.INF' = 'out\net\cdcnet\ndis5\free\CDC98.INF' }
                   Stage = @{ 'CDCNET.INF' = @('STAGE', 'W98SE WME', '0.1.4.0: the Advanced tab (ethernet.md 4.10; simulators, record 2026-11-09 advanced-tab) and the resume retry; 0.1.3.0: the 98 SE and ME upcalls from an NDIS timer, the sleep drain, the unbounded send queue, the removal fix and the departure probe (legs on XP, XP x64, 2000, 98 SE and ME: record 2026-11-07 98se-tcp; on ME with QEMU usb-net a two-way TCP stall stays open); binds QEMU usb-net (0525:A4A2): install and ping passed on 98 SE under xHCI98 3.2.0.0; the UHCI, NUSB EHCI and xHCI98 data legs ran its test copy CDCT98.INF') } }
                @{ Name = 'w2k-xp'; Arch = 'x86'
                   Files = @{ 'CDCNET.SYS' = 'out\net\cdcnet\ndis5\free\cdcnet.sys'; 'CDCNET.INF' = 'out\net\cdcnet\ndis5\free\CDCNET.INF'; 'CDCRNDIS.INF' = 'tree:src\net\cdcnet\CDCRNDIS.INF' }
                   Stage = @{ 'CDCNET.INF' = @('STAGE', 'W2K WXP', '0.1.4.0: the Advanced tab (ethernet.md 4.10; simulators, record 2026-11-09 advanced-tab) and the resume retry; 0.1.3.0: the 98 SE and ME upcalls from an NDIS timer, the sleep drain, the unbounded send queue, the removal fix and the departure probe (legs on XP, XP x64, 2000, 98 SE and ME: record 2026-11-07 98se-tcp; on ME with QEMU usb-net a two-way TCP stall stays open); binds QEMU usb-net (0525:A4A2): install and ping passed on XP under UHCI; the UHCI and EHCI ECM data legs ran its test copy CDCTEST.INF')
                              'CDCRNDIS.INF' = @('COPY', 'WXP', 'binds Windows RNDIS driver; no passing data leg') } }
                @{ Name = 'wv-7-x86'; Arch = 'x86'
                   Files = @{ 'CDCNET.SYS' = 'out\net\cdcnet\ndis5\free\cdcnet.sys'; 'CDCNET.INF' = 'out\net\cdcnet\ndis5\free\CDCNET.INF'; 'CDCRNDIS.INF' = 'tree:src\net\cdcnet\CDCRNDIS.INF' }
                   Stage = @{ 'CDCNET.INF' = @('COPY', 'WVISTA W7', '0.1.4.0: the Advanced tab (ethernet.md 4.10; simulators, record 2026-11-09 advanced-tab) and the resume retry; 0.1.3.0: no leg on Vista or 7 x86')
                              'CDCRNDIS.INF' = @('COPY', 'WVISTA W7', 'binds Windows RNDIS driver; no passing data leg') } }
                @{ Name = 'wxp-x64'; Arch = 'x64'
                   Files = @{ 'CDCNET.SYS' = 'x64:cdcnet.wxp-x64.CDCNET.SYS'; 'CDCNET.INF' = 'out\net\cdcnet\ndis51\free\CDCNET.INF'; 'CDCRNDIS.INF' = 'tree:src\net\cdcnet\CDCRNDIS.INF' }
                   Stage = @{ 'CDCNET.INF' = @('STAGE', 'WXP64', '0.1.4.0: the Advanced tab (ethernet.md 4.10; simulators, record 2026-11-09 advanced-tab) and the resume retry; 0.1.3.0: the 98 SE and ME upcalls from an NDIS timer, the sleep drain, the unbounded send queue, the removal fix and the departure probe (legs on XP, XP x64, 2000, 98 SE and ME: record 2026-11-07 98se-tcp; on ME with QEMU usb-net a two-way TCP stall stays open); binds QEMU usb-net (0525:A4A2): simulator legs on XP x64 (2026-11-04): the INF staged by SetupCopyOEMInf, install, DHCP, ping and TFTP passed under UHCI and xHCI98 3.4.0.0; NCM by class (simulator) under EHCI: DHCP and ping; its XP x64 models list no RNDIS id')
                              'CDCRNDIS.INF' = @('COPY', 'WXP64', 'binds Windows RNDIS driver; no passing data leg') } }
                @{ Name = 'wv-7-x64'; Arch = 'x64'
                   Files = @{ 'CDCNET.SYS' = 'x64:cdcnet.wv-7-x64.CDCNET.SYS'; 'CDCNET.INF' = 'out\net\cdcnet\ndis51\free\CDCNET.INF'; 'CDCRNDIS.INF' = 'tree:src\net\cdcnet\CDCRNDIS.INF' }
                   Stage = @{ 'CDCNET.INF' = @('STAGE', 'W7X64', '0.1.4.0: the Advanced tab (ethernet.md 4.10; simulators, record 2026-11-09 advanced-tab) and the resume retry; 0.1.3.0: no 7 x64 leg at preparation; UHCI ECM leg on 7 x64 ran its test copy CDCTEST.INF (same install sections); test-signed')
                              'CDCRNDIS.INF' = @('COPY', 'WVISTA64 W7X64', 'binds Windows RNDIS driver; no passing data leg') } }
            )
        }
        @{
            Id = 'net/rtl815x'; Kind = 'DEVICE'
            Desc = 'USB Ethernet (rtl815x): Realtek RTL8152, 8153, 8156, 8157 and 8159 adapters'
            DeviceIds = @('*')
            Services = @('rtl815x')
            Folders = @(
                @{ Name = 'w98se-me'; Arch = 'x86'
                   Files = @{ 'RTL815X.SYS' = 'out\net\rtl815x\ndis5\free\rtl815x.sys'; 'RTL81598.INF' = 'out\net\rtl815x\ndis5\free\RTL81598.INF' }
                   Stage = @{ 'RTL81598.INF' = @('STAGE', 'W98SE WME', 'staged on simulator legs (owner, 2026-11-06): the RTL8156 and 8156B under xHCI98 3.4.1.0 and, on 98 SE, NUSB EHCI; 0.2.2.0: the Advanced tab (ethernet.md 4.10; simulators, record 2026-11-09 advanced-tab); 0.2.1.0: the net core fixes and the RTL8157 changes; the 98 SE and ME upcalls from an NDIS timer (record 2026-11-07 98se-tcp); the real RTL8157 passed on XP (record 2026-11-07) and on 98 SE with that fix: 300 s TCP both ways, unplug and replug (record 2026-11-07 realtek-diagnostic); the ids Realtek INFs list added (owner, 2026-11-07; no device on them seen)') } }
                @{ Name = 'w2k'; Arch = 'x86'
                   Files = @{ 'RTL815X.SYS' = 'out\net\rtl815x\ndis5\free\rtl815x.sys'; 'RTL815X.INF' = 'out\net\rtl815x\ndis5\free\RTL815X.INF' }
                   Stage = @{ 'RTL815X.INF' = @('STAGE', 'W2K', 'staged on simulator legs (owner, 2026-11-06): the RTL8156 and 8156B under xHCI98 3.4.1.0; 0.2.2.0: the Advanced tab (ethernet.md 4.10; simulators, record 2026-11-09 advanced-tab); 0.2.1.0: the net core fixes and the RTL8157 changes; the real RTL8157 passed on XP (record 2026-11-07); the ids Realtek INFs list added (owner, 2026-11-07; no device on them seen)') } }
                # XP to 7 (owner, 2026-10-27): the new chips only, one INF with a models
                # section per system (ethernet.md section 7.6).
                @{ Name = 'wxp'; Arch = 'x86'
                   Files = @{ 'RTL815X.SYS' = 'out\net\rtl815x\ndis5\free\rtl815x.sys'; 'RTLXP7.INF' = 'out\net\rtl815x\ndis5\free\RTLXP7.INF' }
                   Stage = @{ 'RTLXP7.INF' = @('STAGE', 'WXP', 'staged on simulator legs (owner, 2026-11-06): the RTL8156 and 8156B under xHCI98 3.4.1.0; 0.2.2.0: the Advanced tab (ethernet.md 4.10; simulators, record 2026-11-09 advanced-tab); 0.2.1.0: the net core fixes and the RTL8157 changes; the real RTL8157 passed on XP (record 2026-11-07); the ids Realtek INFs list added (owner, 2026-11-07; no device on them seen)') } }
                @{ Name = 'wv-7-x86'; Arch = 'x86'
                   Files = @{ 'RTL815X.SYS' = 'out\net\rtl815x\ndis5\free\rtl815x.sys'; 'RTLXP7.INF' = 'out\net\rtl815x\ndis5\free\RTLXP7.INF' }
                   Stage = @{ 'RTLXP7.INF' = @('COPY', 'WVISTA W7', 'copy only (owner, 2026-11-06): simulator legs only; the ids Realtek INFs list added (owner, 2026-11-07; no device on them seen)') } }
                @{ Name = 'wxp-x64'; Arch = 'x64'
                   Files = @{ 'RTL815X.SYS' = 'x64:rtl815x.wxp-x64.RTL815X.SYS'; 'RTLXP7.INF' = 'out\net\rtl815x\ndis51\free\RTLXP7.INF' }
                   Stage = @{ 'RTLXP7.INF' = @('STAGE', 'WXP64', 'staged on simulator legs (owner, 2026-11-06): the RTL8156 and 8156B under xHCI98 3.4.1.0; 0.2.2.0: the Advanced tab (ethernet.md 4.10; simulators, record 2026-11-09 advanced-tab); 0.2.1.0: the net core fixes and the RTL8157 changes; the real RTL8157 passed on XP (record 2026-11-07); the ids Realtek INFs list added (owner, 2026-11-07; no device on them seen)') } }
                @{ Name = 'wv-7-x64'; Arch = 'x64'
                   Files = @{ 'RTL815X.SYS' = 'x64:rtl815x.wv-7-x64.RTL815X.SYS'; 'RTLXP7.INF' = 'out\net\rtl815x\ndis51\free\RTLXP7.INF' }
                   Stage = @{ 'RTLXP7.INF' = @('COPY', 'WVISTA64 W7X64', 'copy only (owner, 2026-11-06): simulator legs only; the ids Realtek INFs list added (owner, 2026-11-07; no device on them seen); test-signed') } }
            )
        }
        @{
            # New in 0.1.1 (owner, 2026-10-25); its 0.1.1 image kept. Folders: ethernet.md section 7.4.
            Id = 'net/ax88179'; Kind = 'DEVICE'
            Desc = 'USB Ethernet (ax88179): adapters with an ASIX AX88179 or AX88178A chip'
            # Vendor dongles bound whole by their listed VID/PIDs (ethernet.md section 5.1).
            DeviceIds = @('*')
            Services = @('ax179f')
            Folders = @(
                @{ Name = 'w98se-me'; Arch = 'x86'
                   Files = @{ 'AX179F.SYS' = 'out\net\ax88179\ndis5\free\ax179f.sys'; 'AX179F98.INF' = 'out\net\ax88179\ndis5\free\AX179F98.INF' }
                   Stage = @{ 'AX179F98.INF' = @('STAGE', 'W98SE WME', '0.1.4.0: the Advanced tab (ethernet.md 4.10; simulators, record 2026-11-09 advanced-tab); 0.1.3.0: the net core fixes, legs on every package copy (record 2026-11-06 netcore-port); earlier: real AX88179 via VM: install, DHCP, ping and removal legs passed on 98 SE under xHCI98 and NUSB EHCI, on ME under xHCI98; the final source (settle cycle) ran again on 98 SE only') } }
                @{ Name = 'w2k'; Arch = 'x86'
                   Files = @{ 'AX179F.SYS' = 'out\net\ax88179\ndis5\free\ax179f.sys'; 'AX179F.INF' = 'out\net\ax88179\ndis5\free\AX179F.INF' }
                   Stage = @{ 'AX179F.INF' = @('STAGE', 'W2K', '0.1.4.0: the Advanced tab (ethernet.md 4.10; simulators, record 2026-11-09 advanced-tab); 0.1.3.0: the net core fixes (record 2026-11-06 netcore-port); simulator legs on 2000 (2026-11-04), the INF staged by SetupCopyOEMInf: under xHCI98 3.4.0.0 first plug, DHCP, ping 32 to 4000 bytes, removal under traffic and ping -t across a replug passed; under Windows EHCI DHCP, ping 32 and 1472 bytes and removal under traffic; the simulator matched the real adapter on 98 SE') } }
            )
        }
        @{
            # New in 0.1.9. Folders: ethernet.md section 7.4. AX772F98.INF: the
            # AX88772B/C and the SR9800 on 98 SE and ME; AX772F.INF: the SR9800 only
            # (ASIX's own driver, usb-wdm-nonfree, serves the AX88772B/C on 2000 and later).
            Id = 'net/ax88772'; Kind = 'DEVICE'
            Desc = 'USB Ethernet (ax88772): ASIX AX88772B and AX88772C, CoreChips SR9800'
            DeviceIds = @('*')
            Services = @('ax772f')
            Folders = @(
                @{ Name = 'w98se-me'; Arch = 'x86'
                   Files = @{ 'AX772F.SYS' = 'out\net\ax88772\ndis5\free\ax772f.sys'; 'AX772F98.INF' = 'out\net\ax88772\ndis5\free\AX772F98.INF' }
                   Stage = @{ 'AX772F98.INF' = @('STAGE', 'W98SE WME', '0.1.1.0: the Advanced tab (ethernet.md 4.10; simulators, record 2026-11-09 advanced-tab); staged (owner, 2026-11-07): simulator legs: AX88772B/C and SR9800 on 98 SE and ME under xHCI98, the SR9800 on 98 SE under NUSB EHCI too (records 2026-11-06 ax88772-sim and sr98xx-sim)') } }
                @{ Name = 'w2k'; Arch = 'x86'
                   Files = @{ 'AX772F.SYS' = 'out\net\ax88772\ndis5\free\ax772f.sys'; 'AX772F.INF' = 'out\net\ax88772\ndis5\free\AX772F.INF' }
                   Stage = @{ 'AX772F.INF' = @('STAGE', 'W2K', '0.1.1.0: the Advanced tab (ethernet.md 4.10; simulators, record 2026-11-09 advanced-tab); staged (owner, 2026-11-07): SR9800 simulator legs (record 2026-11-06 sr98xx-sim)') } }
                @{ Name = 'wxp'; Arch = 'x86'
                   Files = @{ 'AX772F.SYS' = 'out\net\ax88772\ndis5\free\ax772f.sys'; 'AX772F.INF' = 'out\net\ax88772\ndis5\free\AX772F.INF' }
                   Stage = @{ 'AX772F.INF' = @('STAGE', 'WXP', '0.1.1.0: the Advanced tab (ethernet.md 4.10; simulators, record 2026-11-09 advanced-tab); staged (owner, 2026-11-07): SR9800 simulator legs (record 2026-11-06 sr98xx-sim)') } }
                @{ Name = 'wv-7-x86'; Arch = 'x86'
                   Files = @{ 'AX772F.SYS' = 'out\net\ax88772\ndis5\free\ax772f.sys'; 'AX772F.INF' = 'out\net\ax88772\ndis5\free\AX772F.INF' }
                   Stage = @{ 'AX772F.INF' = @('STAGE', 'W7', '0.1.1.0: the Advanced tab (ethernet.md 4.10; simulators, record 2026-11-09 advanced-tab); staged (owner, 2026-11-07): SR9800 simulator legs on 7 x86; Vista x86 has no leg, so its line is 7 only') } }
                @{ Name = 'wxp-x64'; Arch = 'x64'
                   Files = @{ 'AX772F.SYS' = 'x64:ax88772.wxp-x64.AX772F.SYS'; 'AX772F.INF' = 'out\net\ax88772\ndis51\free\AX772F.INF' }
                   Stage = @{ 'AX772F.INF' = @('STAGE', 'WXP64', '0.1.1.0: the Advanced tab (ethernet.md 4.10; simulators, record 2026-11-09 advanced-tab); staged (owner, 2026-11-07): SR9800 simulator legs on XP x64 (the owner-signed f45732b image, record 2026-11-06 sha1-cert)') } }
                @{ Name = 'wv-7-x64'; Arch = 'x64'
                   Files = @{ 'AX772F.SYS' = 'x64:ax88772.wv-7-x64.AX772F.SYS'; 'AX772F.INF' = 'out\net\ax88772\ndis51\free\AX772F.INF' }
                   Stage = @{ 'AX772F.INF' = @('STAGE', 'WVISTA64 W7X64', '0.1.1.0: the Advanced tab (ethernet.md 4.10; simulators, record 2026-11-09 advanced-tab); staged (owner, 2026-11-07): SR9800 simulator legs on Vista x64 SP2 and 7 x64 (the owner-signed f45732b image, SHA-1 certificate); test-signed') } }
            )
        }
        @{
            # New in 0.1.9. Folders as ax88772's (ethernet.md sections 6.9.2, 7.4).
            Id = 'net/sr9700'; Kind = 'DEVICE'
            Desc = 'USB Ethernet (sr9700): CoreChips SR9700 adapters'
            DeviceIds = @('*')
            Services = @('sr9700f')
            Folders = @(
                @{ Name = 'w98se-me'; Arch = 'x86'
                   Files = @{ 'SR9700F.SYS' = 'out\net\sr9700\ndis5\free\sr9700f.sys'; 'SR970098.INF' = 'out\net\sr9700\ndis5\free\SR970098.INF' }
                   Stage = @{ 'SR970098.INF' = @('STAGE', 'W98SE WME', '0.1.1.0: the shared network core of 0.1.10; staged (owner, 2026-11-07): simulator legs under xHCI98 on 98 SE and ME (record 2026-11-06 sr98xx-sim); a USB 1.1 device, which NUSB EHCI does not take') } }
                @{ Name = 'w2k'; Arch = 'x86'
                   Files = @{ 'SR9700F.SYS' = 'out\net\sr9700\ndis5\free\sr9700f.sys'; 'SR9700F.INF' = 'out\net\sr9700\ndis5\free\SR9700F.INF' }
                   Stage = @{ 'SR9700F.INF' = @('STAGE', 'W2K', '0.1.1.0: the shared network core of 0.1.10; staged (owner, 2026-11-07): simulator legs (record 2026-11-06 sr98xx-sim)') } }
                @{ Name = 'wxp'; Arch = 'x86'
                   Files = @{ 'SR9700F.SYS' = 'out\net\sr9700\ndis5\free\sr9700f.sys'; 'SR9700F.INF' = 'out\net\sr9700\ndis5\free\SR9700F.INF' }
                   Stage = @{ 'SR9700F.INF' = @('STAGE', 'WXP', '0.1.1.0: the shared network core of 0.1.10; staged (owner, 2026-11-07): simulator legs (record 2026-11-06 sr98xx-sim)') } }
                @{ Name = 'wv-7-x86'; Arch = 'x86'
                   Files = @{ 'SR9700F.SYS' = 'out\net\sr9700\ndis5\free\sr9700f.sys'; 'SR9700F.INF' = 'out\net\sr9700\ndis5\free\SR9700F.INF' }
                   Stage = @{ 'SR9700F.INF' = @('STAGE', 'W7', '0.1.1.0: the shared network core of 0.1.10; staged (owner, 2026-11-07): simulator legs on 7 x86; Vista x86 has no leg, so its line is 7 only') } }
                @{ Name = 'wxp-x64'; Arch = 'x64'
                   Files = @{ 'SR9700F.SYS' = 'x64:sr9700.wxp-x64.SR9700F.SYS'; 'SR9700F.INF' = 'out\net\sr9700\ndis51\free\SR9700F.INF' }
                   Stage = @{ 'SR9700F.INF' = @('STAGE', 'WXP64', '0.1.1.0: the shared network core of 0.1.10; staged (owner, 2026-11-07): simulator legs on XP x64 (the owner-signed f45732b image, record 2026-11-06 sha1-cert)') } }
                @{ Name = 'wv-7-x64'; Arch = 'x64'
                   Files = @{ 'SR9700F.SYS' = 'x64:sr9700.wv-7-x64.SR9700F.SYS'; 'SR9700F.INF' = 'out\net\sr9700\ndis51\free\SR9700F.INF' }
                   Stage = @{ 'SR9700F.INF' = @('STAGE', 'WVISTA64 W7X64', '0.1.1.0: the shared network core of 0.1.10; staged (owner, 2026-11-07): simulator legs on Vista x64 SP2 and 7 x64 (the owner-signed f45732b image, SHA-1 certificate); test-signed') } }
            )
        }
        @{
            Id = 'hid/xpad98'; Kind = 'DEVICE'
            Desc = 'Game pads (xpad98): for Xbox 360 and Xbox One controllers'
            # gamepad.md section 5.1: vendor-class pads, one devnode, ids listed whole.
            DeviceIds = @('*')
            Services = @('xpad98')
            Folders = @(
                @{ Name = 'w98se-me'; Arch = 'x86'
                   Files = @{ 'xpad98.sys' = 'tree:free\hid\xpad98\w98se-me\xpad98.sys'; 'xp98me.inf' = 'tree:src\hid\xpad98\xp98me.inf' }
                   Stage = @{ 'xp98me.inf' = @('STAGE', 'W98SE WME', '0.1.1.0: SurpriseRemovalOK (2000); xHCI98, UHCI and NUSB EHCI legs passed (simulated pads)') } }
                @{ Name = 'w2k'; Arch = 'x86'
                   Files = @{ 'xpad98.sys' = 'tree:free\hid\xpad98\w2k\xpad98.sys'; 'xp2k.inf' = 'tree:src\hid\xpad98\xp2k.inf' }
                   Stage = @{ 'xp2k.inf' = @('COPY', 'W2K', '0.1.1.0: install and JOYTEST under xHCI98 3.4.1.0, no unsafe-removal dialog at unplug (simulated pad); EHCI legs passed; copy only while it shares 045E:028F with the non-free x360wc staged on 2000') } }
                @{ Name = 'wxp'; Arch = 'x86'
                   Files = @{ 'xpad98.sys' = 'tree:free\hid\xpad98\wxp\xpad98.sys'; 'xpxpv.inf' = 'tree:src\hid\xpad98\xpxpv.inf' }
                   Stage = @{ 'xpxpv.inf' = @('STAGE', 'WXP', 'EHCI Xbox One leg passed') } }
                @{ Name = 'wv-x86'; Arch = 'x86'
                   Files = @{ 'xpad98.sys' = 'tree:free\hid\xpad98\wv-x86\xpad98.sys'; 'xpxpv.inf' = 'tree:src\hid\xpad98\xpxpv.inf' }
                   Stage = @{ 'xpxpv.inf' = @('STAGE', 'WVISTA', 'EHCI Xbox One leg passed') } }
                @{ Name = 'wxp-x64'; Arch = 'x64'
                   Files = @{ 'xpad98.sys' = 'x64:xpad98.wxp-x64.xpad98.sys'; 'xpxpv.inf' = 'tree:src\hid\xpad98\xpxpv.inf' }
                   Stage = @{ 'xpxpv.inf' = @('STAGE', 'WXP64', 'EHCI Xbox One leg passed') } }
                @{ Name = 'wv-x64'; Arch = 'x64'
                   Files = @{ 'xpad98.sys' = 'x64:xpad98.wv-x64.xpad98.sys'; 'xpxpv.inf' = 'tree:src\hid\xpad98\xpxpv.inf' }
                   Stage = @{ 'xpxpv.inf' = @('STAGE', 'WVISTA64', 'EHCI Xbox One leg passed with test signing (owner, 2026-10-24); test-signed') } }
            )
        }
        @{
            # New in 0.1.9. Folders: absptr.md section 9; the x64 folder's name is the
            # owner's (2026-11-07). absptr2k.inf and absptr64.inf include Windows'
            # input.inf (Needs=HID_Inst.NT).
            Id = 'hid/absptr'; Kind = 'DEVICE'
            Desc = 'Pointing (absptr): pen tablets, HID touch screens and absolute mice'
            DeviceIds = @('*')
            Services = @('absptr')
            Folders = @(
                @{ Name = 'w98se-me'; Arch = 'x86'
                   Files = @{ 'absptr.sys' = 'tree:free\hid\absptr\w98se-me\absptr.sys'; 'absptr.inf' = 'tree:src\hid\absptr\absptr.inf' }
                   Stage = @{ 'absptr.inf' = @('STAGE', 'W98SE WME', 'staged (owner, 2026-11-07): on 98 SE under xHCI98 3.4.1.0 and NUSB EHCI the PenPartner, touch, pen and absolute-mouse simulators and the QEMU tablet passed (record 2026-11-06 absptr); ME: an earlier build (final2)') } }
                @{ Name = 'w2k-xp'; Arch = 'x86'
                   Files = @{ 'absptr.sys' = 'tree:free\hid\absptr\w2k-xp\absptr.sys'; 'absptr2k.inf' = 'tree:src\hid\absptr\absptr2k.inf' }
                   Stage = @{ 'absptr2k.inf' = @('STAGE', 'W2K WXP', 'staged (owner, 2026-11-07): 2000 under xHCI98 3.4.1.0 (QEMU tablet, touch simulator) passed; 32-bit XP: an earlier build (final2)') } }
                @{ Name = 'xp-7-x64'; Arch = 'x64'
                   Files = @{ 'absptr.sys' = 'x64:absptr.xp-7-x64.absptr.sys'; 'absptr64.inf' = 'tree:src\hid\absptr\absptr64.inf' }
                   Stage = @{ 'absptr64.inf' = @('STAGE', 'WXP64 WVISTA64 W7X64', 'staged (owner, 2026-11-07): XP x64, Vista x64 and 7 x64 legs with unsigned builds (record 2026-11-06 absptr); test-signed') } }
            )
        }
        @{
            Id = 'storage/usbflop'; Kind = 'CLASS'
            Desc = 'USB floppy (usbflop): USB Mass Storage UFI floppy drives'
            DeviceIds = @()
            Services = @()
            # floppy.md 7: 98 SE's from NUSB 3.3, ME's from its own cabs (LayoutFile).
            WindowsFiles = @('USBSTOR.SYS', 'USBNTMAP.SYS')
            Folders = @(
                @{ Name = 'w98se'; Arch = 'inf'
                   Files = @{ 'USBFLOP.INF' = 'tree:free\storage\usbflop\w98se\USBFLOP.INF'; 'README.TXT' = 'tree:free\storage\usbflop\w98se\README.TXT' }
                   Stage = @{ 'USBFLOP.INF' = @('STAGE', 'W98SE', 'real drive via VM, UHCI and xHCI98 reads passed; needs NUSB 3.3 USB storage installed') } }
                @{ Name = 'wme'; Arch = 'inf'
                   Files = @{ 'USBFLOP.INF' = 'tree:free\storage\usbflop\wme\USBFLOP.INF'; 'README.TXT' = 'tree:free\storage\usbflop\wme\README.TXT' }
                   Stage = @{ 'USBFLOP.INF' = @('STAGE', 'WME', 'real drive via VM, xHCI98 read passed') } }
                @{ Name = 'w2k-8'; Arch = 'inf'
                   Files = @{ 'USBFLOP.INF' = 'tree:free\storage\usbflop\w2k-8\USBFLOP.INF' }
                   Stage = @{ 'USBFLOP.INF' = @('STAGE', 'W2K WXP WXP64 W7X64', 'binds Windows USBSTOR; simulator reads passed (XP x64, 2026-11-04: CBI and CB under UHCI, CBI under xHCI98 3.4.0.0), real drive bound on 2000') } }
            )
        }
        @{
            # New in 0.1.7 (owner, 2026-11-01). Folders: printing.md section 10.
            Id = 'print/usbprn'; Kind = 'CLASS'
            Desc = 'Printers (usbprn): USB printers and USB-to-parallel printer cables'
            DeviceIds = @()
            Services = @()
            # USBPRNME.INF binds ME's own usbprint.sys (printing.md section 2).
            WindowsFiles = @('usbprint.sys')
            Folders = @(
                @{ Name = 'w98se'; Arch = 'x86'
                   Files = @{ 'USBPRN.SYS' = 'tree:free\print\usbprn\w98se\USBPRN.SYS'; 'USBPRNMN.DLL' = 'tree:free\print\usbprn\w98se\USBPRNMN.DLL'; 'USBPRN.INF' = 'tree:free\print\usbprn\w98se\USBPRN.INF' }
                   Stage = @{ 'USBPRN.INF' = @('STAGE', 'W98SE', 'install, USB001, test and Notepad pages, paper-empty and error with Retry, unplug idle and mid-job, a second printer on USB002: passed under xHCI98, NUSB EHCI and Windows UHCI (simulator)') } }
                @{ Name = 'wme'; Arch = 'inf'
                   Files = @{ 'USBPRNME.INF' = 'tree:free\print\usbprn\wme\USBPRNME.INF' }
                   Stage = @{ 'USBPRNME.INF' = @('STAGE', 'WME', 'binds Windows usbprint.sys; install, USB001, test and Notepad pages, replug: passed under Windows UHCI (simulator)') } }
            )
        }
        @{
            Id = 'generic/libusb0'; Kind = 'DEVICE'
            Desc = 'libusb (libusb0): libusb-win32 0.1 for listed programmer devices'
            # libusb.md section 6.2: listed devices, whole-device ids (owner).
            DeviceIds = @('*')
            Services = @()
            Folders = @(
                @{ Name = 'w98se-me'; Arch = 'x86'
                   Files = @{ 'LIBUSB0.SYS' = 'tree:free\generic\libusb0\w98se-me\LIBUSB0.SYS'; 'LIBUSB0.DLL' = 'tree:free\generic\libusb0\w98se-me\LIBUSB0.DLL'; 'LIBUSB0.INF' = 'tree:free\generic\libusb0\w98se-me\LIBUSB0.INF'
                              'TESTLIBU.EXE' = 'tree:free\generic\libusb0\w98se-me\TESTLIBU.EXE'; 'USB.H' = 'tree:free\generic\libusb0\w98se-me\USB.H'
                              'LIBUSB0.LIB' = 'tree:free\generic\libusb0\w98se-me\LIBUSB0.LIB'; 'LIBUSB0.A' = 'tree:free\generic\libusb0\w98se-me\LIBUSB0.A'; 'README.TXT' = 'tree:free\generic\libusb0\w98se-me\README.TXT' }
                   Extra = @('TESTLIBU.EXE', 'USB.H', 'LIBUSB0.LIB', 'LIBUSB0.A')
                   Stage = @{ 'LIBUSB0.INF' = @('STAGE', 'W98SE', 'xHCI98, NUSB EHCI and UHCI install and listing passed; ME has no leg') } }
            )
        }
        @{
            Id = 'audio/noaud'; Kind = 'DEVICE'
            Desc = 'No driver (noaud): leaves a webcam microphone driverless on 98 SE'
            DeviceIds = @('*')
            Services = @()
            Folders = @(
                @{ Name = 'w98se'; Arch = 'inf'
                   Files = @{ 'NOAUD98.INF' = 'tree:free\audio\noaud\w98se\NOAUD98.INF'; 'README.TXT' = 'tree:free\audio\noaud\w98se\README.TXT' }
                   Stage = @{ 'NOAUD98.INF' = @('COPY', 'W98SE', 'opt-in (owner, 2026-10-23): install by hand only if plugging a webcam freezes 98 SE; staged, it would take the microphone from usbaudio.sys for everyone') } }
            )
        }
    )
    # Tools (owner, 2026-10-24): free\tools\<Name>\, not driver packages (no INF, no
    # STAGE.TXT line). Files as for a folder; an image is built in this checkout by its
    # own script (CamCheck: src\video\uvc98\preview.cmd) and may import only Imports.
    Tools = @(
        @{ Name = 'camcheck'
           Files = @{ 'CAMCHECK.EXE' = 'tree:free\tools\camcheck\CAMCHECK.EXE'
                      'README.TXT' = 'tree:free\tools\camcheck\README.TXT'; 'NOTICE.TXT' = 'tree:free\tools\camcheck\NOTICE.TXT' }
           Imports = @('AVICAP32.DLL', 'USER32.DLL', 'KERNEL32.DLL', 'MSVCRT.DLL') }
    )
    Relations = @(
        'EXCLUDES | free:audio/uac1 | free:audio/usbmidi | WME W2K WXP WXP64 WVISTA WVISTA64 W7 W7X64 W8 W8X64 W10 W10X64 | uac1 (not in 0.1.0) serves MIDI-only functions by class; usbmidi by its ids'
        'COEXISTS | free:audio/uac1 | free:audio/usbmidi | W98SE | uac1 serves no MIDI (a MIDI-only function gets a null start); usbmidi''s hardware ids outrank uac1''s class line (uac2.md 26.10)'
        'COEXISTS | free:audio/uac1 | free:audio/noaud | W98SE | noaud''s hardware ids (a webcam microphone, opt-in) outrank uac1''s class line: the microphone stays driverless where the user chose it'
        'EXCLUDES | free:net/rtl815x | nonfree:ethernet/rtl815x | W98SE WME W2K W8 W8X64 W10 W10X64 | the same chips'
        'COEXISTS | free:net/rtl815x | nonfree:ethernet/rtl815x | WXP WXP64 WVISTA WVISTA64 W7 W7X64 | disjoint ids: RTLXP7.INF lists only chips the nonfree INFs leave out on each system (owner, 2026-10-29: split again with the xHCI98 packager change, xHCI98 task 45.6)'
        'EXCLUDES | free:net/ax88179 | nonfree:ethernet/ax88179 | W98SE WME W2K WXP WXP64 WVISTA WVISTA64 W7 W7X64 W8 W8X64 W10 W10X64 | the same chips'
        'COEXISTS | free:net/ax88772 | nonfree:ethernet/ax88772b | W2K WXP WXP64 WVISTA WVISTA64 W7 W7X64 | disjoint ids: AX772F.INF lists only the SR9800 (0FE6:9800) on 2000 and later, where ASIX driver serves the AX88772B/C'
        'EXCLUDES | free:hid/xpad98 | nonfree:hid/x360wc | W98SE WME W2K WXP WXP64 WVISTA WVISTA64 W7 W7X64 W8 W8X64 W10 W10X64 | both bind the Play and Charge Kit 045E:028F'
        'COEXISTS | free:hid/xpad98 | nonfree:hid/x360c | W98SE WME W2K WXP WXP64 WVISTA WVISTA64 W7 W7X64 | disjoint ids: xpad98 takes the pads outside x360c list and the Xbox One pads'
        'COEXISTS | free:video/uvc98 | free:audio/noaud | W98SE | one webcam, different interfaces: uvc98 the video function, noaud the microphone'
    )
}
