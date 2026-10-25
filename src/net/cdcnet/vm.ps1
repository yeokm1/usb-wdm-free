# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
#
# Start one headless QEMU guest for a CDCNET leg (docs/toolchain.md,
# "Test guests"). Everything it creates lives under -Work on E:: the
# throwaway overlay, the virtual floppy's folder, QEMU's TEMP/TMP, the
# TFTP folder, the 0xE9 debug-console trace and the network capture.
# The monitor listens on localhost only. The guest's only network is
# QEMU's user-mode network, reached through the USB device.
#
#   vm.ps1 -Guest w2k -Work E:\usb-wdm-free\cdcnet\w2k-ecm -Stage <folder>
param(
    [Parameter(Mandatory)][ValidateSet('w2k', 'w98se', 'wxp', 'w764', 'w786', 'wv86', 'wxp64', 'wme')][string]$Guest,
    [Parameter(Mandatory)][string]$Work,
    [Parameter(Mandatory)][string]$Stage,
    [int]$Monitor = 48445,
    [ValidateSet('uhci', 'ehci')][string]$Bus = 'uhci',
    [switch]$NoDevice,
    [string]$Cdrom,
    [string]$Base
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
# Owner, 2026-10-22: base images live on E:; the repository's vm\ is the fallback.
if (-not $Base) {
    $Base = "E:\usb-wdm-free\vm\$Guest.qcow2"
    if (-not (Test-Path $Base)) { $Base = Join-Path $root "vm\$Guest.qcow2" }
}
New-Item -ItemType Directory -Force $Work | Out-Null
$overlay = Join-Path $Work 'disk.qcow2'
if (-not (Test-Path $overlay)) {
    & qemu-img create -q -f qcow2 -b $Base -F qcow2 $overlay
    if ($LASTEXITCODE) { throw 'qemu-img failed' }
}
$tmp = Join-Path $Work 'tmp'
$tftp = Join-Path $Work 'tftp'
New-Item -ItemType Directory -Force $tmp, $tftp | Out-Null
python (Join-Path $PSScriptRoot 'stage.py') $tftp | Out-Null

$common = @('-display', 'none', '-nic', 'none', '-rtc', 'base=localtime',
    '-monitor', "tcp:127.0.0.1:$Monitor,server,nowait",
    '-debugcon', "file:$(Join-Path $Work 'e9.log')",
    '-drive', "file=$overlay,format=qcow2,if=ide,index=0,media=disk",
    '-drive', "file=fat:floppy:rw:$Stage,if=floppy,index=0,format=raw")
switch ($Guest) {
    'w2k'   { $exe = 'qemu-system-i386'; $m = @('-machine', 'pc,acpi=off', '-cpu', 'pentium3,-apic', '-m', '256', '-vga', 'cirrus') }
    'w98se' { $exe = 'qemu-system-i386'; $m = @('-machine', 'pc', '-cpu', 'pentium3', '-m', '256', '-vga', 'cirrus') }
    'wme'   { $exe = 'qemu-system-i386'; $m = @('-machine', 'pc', '-cpu', 'pentium3', '-m', '256', '-vga', 'cirrus') }
    'wxp'   { $exe = 'qemu-system-x86_64'; $m = @('-machine', 'pc', '-accel', 'whpx,kernel-irqchip=off', '-cpu', 'pentium3', '-smp', '2', '-m', '512', '-vga', 'std') }
    'wv86'  { $exe = 'qemu-system-x86_64'; $m = @('-machine', 'pc', '-accel', 'tcg,thread=multi', '-cpu', 'pentium3', '-smp', '2', '-m', '1024', '-vga', 'std') }
    'w786'  { $exe = 'qemu-system-x86_64'; $m = @('-machine', 'pc', '-accel', 'tcg,thread=multi', '-cpu', 'qemu64', '-smp', '2', '-m', '1024', '-vga', 'std') }
    'w764'  { $exe = 'qemu-system-x86_64'; $m = @('-machine', 'pc', '-accel', 'tcg,thread=multi', '-cpu', 'qemu64', '-smp', '2', '-m', '2048', '-vga', 'std') }
    'wxp64' { $exe = 'qemu-system-x86_64'; $m = @('-machine', 'pc', '-accel', 'tcg,thread=multi', '-cpu', 'qemu64', '-smp', '2', '-m', '1024', '-vga', 'std') }
}
if ($Cdrom) { $common += @('-drive', "file=$Cdrom,if=ide,index=2,media=cdrom,readonly=on") }
$usb = if ($Bus -eq 'ehci') { @('-device', 'usb-ehci,id=ehci') } else { @('-usb') }
$net = @()
if (-not $NoDevice) {
    $busOpt = if ($Bus -eq 'ehci') { ',bus=ehci.0' } else { '' }
    $net = @('-netdev', "user,id=usbnet,tftp=$tftp",
             '-object', "filter-dump,id=dump,netdev=usbnet,file=$(Join-Path $Work 'net.pcap')",
             '-device', "usb-net,netdev=usbnet,id=cdcnet$busOpt")
}
$qargs = $m + $common + $usb + $net
$env:TEMP = $tmp; $env:TMP = $tmp
$p = Start-Process -FilePath $exe -ArgumentList $qargs -WindowStyle Hidden -PassThru `
    -RedirectStandardError (Join-Path $Work 'qemu.err') -RedirectStandardOutput (Join-Path $Work 'qemu.out')
"$($p.Id)" | Set-Content (Join-Path $Work 'qemu.pid')
"$exe $($qargs -join ' ')" | Set-Content (Join-Path $Work 'qemu.cmd')
"PID $($p.Id) monitor 127.0.0.1:$Monitor"
