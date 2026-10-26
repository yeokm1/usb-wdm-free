# Release definition for usb-wdm-free 0.1.3 (docs/design/build.md section 7),
# read by release\release.ps1: release 0.1.2 plus the class matches of build.md
# section 7.1 (owner, 2026-10-26): usbcom's CDC-ACM class line on 98 SE, ME and
# 2000, and cdcnet's CDCRNDIS.INF without EF/04/01 on 7. Those INFs (0.1.3.0)
# come from the tree; every other file is 0.1.2's ('tree:free\...'), the
# signed x64 images among them.
#
# Per package: Class/Name, Kind (CLASS or DEVICE: bound by class ids or by
# VID/PID), Desc (the STAGE.TXT PACKAGE words), DeviceIds (whole-device
# hardware ids the package design records as allowed), and Folders. Per
# folder: Name, Arch (x86, x64 or inf), Files (published name = source under
# out\, or 'tree:<path>' for a file already in the tree), Extra (declared
# support files no INF copies), and per INF its STAGE.TXT line (Action,
# Systems, Note). Sources are relative to the repository root.
@{
    Version = '0.1.3'
    # INF copies kept in the tree that must equal their original but for the
    # Signature line and comments (uac2's x64 INF: 64-bit Windows refuses $CHICAGO$).
    Agree = @{ 'src\audio\uac2\x64\uac2wdm.inf' = 'src\audio\uac2\uac2wdm.inf' }
    # No 'x64:' source in 0.1.3: the signed x64 images are 0.1.0's, from the tree.
    X64Builds = @{}
    Packages = @(
        @{
            Id = 'audio/uac2'; Kind = 'CLASS'
            Desc = 'USB audio (uac2): USB Audio Class 2.0 sound cards and DACs'
            # uac2.md section 3: the X4's whole-device id, owner's request 2026-10-22.
            DeviceIds = @('USB\VID_041E&PID_3278', 'USB\VID_041E&PID_3278&MI_03')
            Services = @('uac2wdm')
            Folders = @(
                @{ Name = 'w98se-me'; Arch = 'x86'
                   Files = @{ 'uac2wdm.sys' = 'tree:free\audio\uac2\w98se-me\uac2wdm.sys'; 'uac2wdm.inf' = 'tree:free\audio\uac2\w98se-me\uac2wdm.inf' }
                   Stage = @{ 'uac2wdm.inf' = @('STAGE', 'W98SE WME', 'every simulator profile passed under xHCI98 (build 16); ME: x4full playback and capture passed under xHCI98 (0.1.1)') } }
                @{ Name = 'w2k-xp'; Arch = 'x86'
                   Files = @{ 'uac2wdm.sys' = 'tree:free\audio\uac2\w2k-xp\uac2wdm.sys'; 'uac2wdm.inf' = 'tree:free\audio\uac2\w2k-xp\uac2wdm.inf' }
                   Stage = @{ 'uac2wdm.inf' = @('STAGE', 'WXP', 'playback passed (simulator, real X4 via VM); capture fails on XP; 2000 has no leg') } }
                @{ Name = 'wv-8-x86'; Arch = 'x86'
                   Files = @{ 'uac2wdm.sys' = 'tree:free\audio\uac2\wv-8-x86\uac2wdm.sys'; 'uac2wdm.inf' = 'tree:free\audio\uac2\wv-8-x86\uac2wdm.inf' }
                   Stage = @{ 'uac2wdm.inf' = @('STAGE', 'W7', 'playback and capture passed on 7 x86 (simulator, real X4 via VM); Vista and 8 have no leg') } }
                @{ Name = 'wxp-x64'; Arch = 'x64'
                   Files = @{ 'uac2wdm.sys' = 'tree:free\audio\uac2\wxp-x64\uac2wdm.sys'; 'uac2wdm.inf' = 'tree:free\audio\uac2\wxp-x64\uac2wdm.inf' }
                   Stage = @{ 'uac2wdm.inf' = @('COPY', 'WXP64', 'copy only (owner, 2026-10-24): no XP x64 leg') } }
                @{ Name = 'wv-8-x64'; Arch = 'x64'
                   Files = @{ 'uac2wdm.sys' = 'tree:free\audio\uac2\wv-8-x64\uac2wdm.sys'; 'uac2wdm.inf' = 'tree:free\audio\uac2\wv-8-x64\uac2wdm.inf' }
                   Stage = @{ 'uac2wdm.inf' = @('COPY', 'WVISTA64 W7X64 W8X64', 'copy only (owner, 2026-10-24); test-signed: loads and plays on 7 x64 with test signing on and the certificate trusted') } }
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
                   Stage = @{ 'usbmidi.inf' = @('STAGE', 'W98SE', 'UHCI, xHCI98 and NUSB EHCI legs passed') } }
                @{ Name = 'wme'; Arch = 'x86'
                   Files = @{ 'usbmidi.sys' = 'tree:free\audio\usbmidi\wme\usbmidi.sys'; 'usbmidi.inf' = 'tree:free\audio\usbmidi\wme\usbmidi.inf' }
                   Stage = @{ 'usbmidi.inf' = @('COPY', 'WME', 'by-hand INF (USBMIDI_BY_HAND only); Windows usbaudio serves the rest') } }
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
                   Files = @{ 'usbmidi.sys' = 'tree:free\audio\usbmidi\wxp-x64\usbmidi.sys'; 'usbmidi.inf' = 'tree:free\audio\usbmidi\wxp-x64\usbmidi.inf' }
                   Stage = @{ 'usbmidi.inf' = @('COPY', 'WXP64', 'by-hand INF (USBMIDI_BY_HAND only); no x64 leg') } }
                @{ Name = 'wv-8-x64'; Arch = 'x64'
                   Files = @{ 'usbmidi.sys' = 'tree:free\audio\usbmidi\wv-8-x64\usbmidi.sys'; 'usbmidi.inf' = 'tree:free\audio\usbmidi\wv-8-x64\usbmidi.inf' }
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
                   Files = @{ 'uvcm2.sys' = 'tree:free\video\uvc98\w98se-me\uvcm2.sys'; 'UVCM298.INF' = 'tree:free\video\uvc98\w98se-me\UVCM298.INF' }
                   Stage = @{ 'UVCM298.INF' = @('STAGE', 'W98SE', 'xHCI98 legs passed (simulator to 640x480, real camera via VM at 160x120); ME has no leg') } }
                @{ Name = 'w2k'; Arch = 'x86'
                   Files = @{ 'uvcm2.sys' = 'tree:free\video\uvc98\w2k\uvcm2.sys'; 'UVCM2.INF' = 'tree:free\video\uvc98\w2k\UVCM2.INF' }
                   Stage = @{ 'UVCM2.INF' = @('STAGE', 'W2K', 'Windows EHCI legs passed (simulator, real camera via VM to 320x240)') } }
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
                   Stage = @{ 'USBCOM.INF' = @('STAGE', 'W98SE WME', 'the USB device; xHCI98, UHCI and NUSB EHCI legs passed (listed boards); the CDC-ACM class match from 0.1.3: ranking read from the INFs, no leg')
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
                   Files = @{ 'CDCNET.SYS' = 'tree:free\net\cdcnet\w98se-me\CDCNET.SYS'; 'CDCNET.INF' = 'tree:free\net\cdcnet\w98se-me\CDCNET.INF' }
                   Stage = @{ 'CDCNET.INF' = @('STAGE', 'W98SE WME', 'binds QEMU usb-net (0525:A4A2): install and ping passed on 98 SE under xHCI98 3.2.0.0; the UHCI, NUSB EHCI and xHCI98 data legs ran its test copy CDCT98.INF') } }
                @{ Name = 'w2k-xp'; Arch = 'x86'
                   Files = @{ 'CDCNET.SYS' = 'tree:free\net\cdcnet\w2k-xp\CDCNET.SYS'; 'CDCNET.INF' = 'tree:free\net\cdcnet\w2k-xp\CDCNET.INF'; 'CDCRNDIS.INF' = 'tree:src\net\cdcnet\CDCRNDIS.INF' }
                   Stage = @{ 'CDCNET.INF' = @('STAGE', 'W2K WXP', 'binds QEMU usb-net (0525:A4A2): install and ping passed on XP under UHCI; the UHCI and EHCI ECM data legs ran its test copy CDCTEST.INF')
                              'CDCRNDIS.INF' = @('COPY', 'WXP', 'binds Windows RNDIS driver; no passing data leg') } }
                @{ Name = 'wv-7-x86'; Arch = 'x86'
                   Files = @{ 'CDCNET.SYS' = 'tree:free\net\cdcnet\wv-7-x86\CDCNET.SYS'; 'CDCNET.INF' = 'tree:free\net\cdcnet\wv-7-x86\CDCNET.INF'; 'CDCRNDIS.INF' = 'tree:src\net\cdcnet\CDCRNDIS.INF' }
                   Stage = @{ 'CDCNET.INF' = @('COPY', 'WVISTA W7', 'no leg on Vista or 7 x86')
                              'CDCRNDIS.INF' = @('COPY', 'WVISTA W7', 'binds Windows RNDIS driver; no passing data leg') } }
                @{ Name = 'wxp-x64'; Arch = 'x64'
                   Files = @{ 'CDCNET.SYS' = 'tree:free\net\cdcnet\wxp-x64\CDCNET.SYS'; 'CDCNET.INF' = 'tree:free\net\cdcnet\wxp-x64\CDCNET.INF'; 'CDCRNDIS.INF' = 'tree:src\net\cdcnet\CDCRNDIS.INF' }
                   Stage = @{ 'CDCNET.INF' = @('COPY', 'WXP64', 'no XP x64 leg')
                              'CDCRNDIS.INF' = @('COPY', 'WXP64', 'binds Windows RNDIS driver; no passing data leg') } }
                @{ Name = 'wv-7-x64'; Arch = 'x64'
                   Files = @{ 'CDCNET.SYS' = 'tree:free\net\cdcnet\wv-7-x64\CDCNET.SYS'; 'CDCNET.INF' = 'tree:free\net\cdcnet\wv-7-x64\CDCNET.INF'; 'CDCRNDIS.INF' = 'tree:src\net\cdcnet\CDCRNDIS.INF' }
                   Stage = @{ 'CDCNET.INF' = @('STAGE', 'W7X64', 'UHCI ECM leg on 7 x64 ran its test copy CDCTEST.INF (same install sections); test-signed')
                              'CDCRNDIS.INF' = @('COPY', 'WVISTA64 W7X64', 'binds Windows RNDIS driver; no passing data leg') } }
            )
        }
        @{
            Id = 'net/rtl815x'; Kind = 'DEVICE'
            Desc = 'USB Ethernet (rtl815x): adapters with a Realtek RTL8152 or RTL8153 chip'
            DeviceIds = @('*')
            Services = @('rtl815x')
            Folders = @(
                @{ Name = 'w98se-me'; Arch = 'x86'
                   Files = @{ 'RTL815X.SYS' = 'tree:free\net\rtl815x\w98se-me\RTL815X.SYS'; 'RTL81598.INF' = 'tree:free\net\rtl815x\w98se-me\RTL81598.INF' }
                   Stage = @{ 'RTL81598.INF' = @('COPY', 'W98SE WME', 'copy only (owner, 2026-10-24): simulator legs only, no device; chip core rewritten in 0.1.2') } }
                @{ Name = 'w2k'; Arch = 'x86'
                   Files = @{ 'RTL815X.SYS' = 'tree:free\net\rtl815x\w2k\RTL815X.SYS'; 'RTL815X.INF' = 'tree:free\net\rtl815x\w2k\RTL815X.INF' }
                   Stage = @{ 'RTL815X.INF' = @('COPY', 'W2K', 'copy only (owner, 2026-10-24): no leg') } }
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
                   Files = @{ 'AX179F.SYS' = 'tree:free\net\ax88179\w98se-me\AX179F.SYS'; 'AX179F98.INF' = 'tree:free\net\ax88179\w98se-me\AX179F98.INF' }
                   Stage = @{ 'AX179F98.INF' = @('STAGE', 'W98SE WME', 'real AX88179 via VM: install, DHCP, ping and removal legs passed on 98 SE under xHCI98 and NUSB EHCI, on ME under xHCI98; the final source (settle cycle) ran again on 98 SE only') } }
                @{ Name = 'w2k'; Arch = 'x86'
                   Files = @{ 'AX179F.SYS' = 'tree:free\net\ax88179\w2k\AX179F.SYS'; 'AX179F.INF' = 'tree:free\net\ax88179\w2k\AX179F.INF' }
                   Stage = @{ 'AX179F.INF' = @('COPY', 'W2K', 'copy only: no leg on 2000') } }
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
                   Files = @{ 'xpad98.sys' = 'tree:free\hid\xpad98\w98se-me\xpad98.sys'; 'xp98me.inf' = 'tree:free\hid\xpad98\w98se-me\xp98me.inf' }
                   Stage = @{ 'xp98me.inf' = @('STAGE', 'W98SE WME', 'xHCI98, UHCI and NUSB EHCI legs passed (simulated pads)') } }
                @{ Name = 'w2k'; Arch = 'x86'
                   Files = @{ 'xpad98.sys' = 'tree:free\hid\xpad98\w2k\xpad98.sys'; 'xp2k.inf' = 'tree:free\hid\xpad98\w2k\xp2k.inf' }
                   Stage = @{ 'xp2k.inf' = @('COPY', 'W2K', 'EHCI legs passed; copy only while it shares 045E:028F with the non-free x360wc staged on 2000') } }
                @{ Name = 'wxp'; Arch = 'x86'
                   Files = @{ 'xpad98.sys' = 'tree:free\hid\xpad98\wxp\xpad98.sys'; 'xpxpv.inf' = 'tree:free\hid\xpad98\wxp\xpxpv.inf' }
                   Stage = @{ 'xpxpv.inf' = @('STAGE', 'WXP', 'EHCI Xbox One leg passed') } }
                @{ Name = 'wv-x86'; Arch = 'x86'
                   Files = @{ 'xpad98.sys' = 'tree:free\hid\xpad98\wv-x86\xpad98.sys'; 'xpxpv.inf' = 'tree:free\hid\xpad98\wv-x86\xpxpv.inf' }
                   Stage = @{ 'xpxpv.inf' = @('STAGE', 'WVISTA', 'EHCI Xbox One leg passed') } }
                @{ Name = 'wxp-x64'; Arch = 'x64'
                   Files = @{ 'xpad98.sys' = 'tree:free\hid\xpad98\wxp-x64\xpad98.sys'; 'xpxpv.inf' = 'tree:free\hid\xpad98\wxp-x64\xpxpv.inf' }
                   Stage = @{ 'xpxpv.inf' = @('STAGE', 'WXP64', 'EHCI Xbox One leg passed') } }
                @{ Name = 'wv-x64'; Arch = 'x64'
                   Files = @{ 'xpad98.sys' = 'tree:free\hid\xpad98\wv-x64\xpad98.sys'; 'xpxpv.inf' = 'tree:free\hid\xpad98\wv-x64\xpxpv.inf' }
                   Stage = @{ 'xpxpv.inf' = @('STAGE', 'WVISTA64', 'EHCI Xbox One leg passed with test signing (owner, 2026-10-24); test-signed') } }
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
                   Stage = @{ 'USBFLOP.INF' = @('STAGE', 'W2K WXP W7X64', 'binds Windows USBSTOR; simulator reads passed, real drive bound on 2000') } }
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
        'EXCLUDES | free:audio/uac1 | free:audio/usbmidi | W98SE WME W2K WXP WXP64 WVISTA WVISTA64 W7 W7X64 W8 W8X64 W10 W10X64 | uac1 (not in 0.1.0) serves MIDI-only functions by class; usbmidi by its ids'
        'EXCLUDES | free:net/rtl815x | nonfree:ethernet/rtl815x | W98SE WME W2K WXP WXP64 WVISTA WVISTA64 W7 W7X64 W8 W8X64 W10 W10X64 | the same chips'
        'EXCLUDES | free:net/ax88179 | nonfree:ethernet/ax88179 | W98SE WME W2K WXP WXP64 WVISTA WVISTA64 W7 W7X64 W8 W8X64 W10 W10X64 | the same chips'
        'EXCLUDES | free:hid/xpad98 | nonfree:hid/x360wc | W98SE WME W2K WXP WXP64 WVISTA WVISTA64 W7 W7X64 W8 W8X64 W10 W10X64 | both bind the Play and Charge Kit 045E:028F'
        'COEXISTS | free:hid/xpad98 | nonfree:hid/x360c | W98SE WME W2K WXP WXP64 WVISTA WVISTA64 W7 W7X64 | disjoint ids: xpad98 takes the pads outside x360c list and the Xbox One pads'
        'COEXISTS | free:video/uvc98 | free:audio/noaud | W98SE | one webcam, different interfaces: uvc98 the video function, noaud the microphone'
    )
}
