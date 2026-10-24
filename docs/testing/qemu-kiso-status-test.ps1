# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
# Diagnostic only: unchanged pinned libusb mapper, original harness.
# No USB access, driver installation or downloads.
param([string]$WorkDirectory='E:/usb-wdm-free/host-qemu/kiso-status')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path "$PSScriptRoot/../..").Path
$refs=Join-Path $repo 'docs/references'
$sourcePath=Join-Path $refs 'libusb-v1.0.30/libusb/os/windows_common.c'
$headerPath=Join-Path $refs 'libusb-v1.0.30/libusb/os/windows_common.h'
$bsdPath=Join-Path $refs 'libusbk-4dd2bdd/LICENSE-bsd.txt'
foreach($path in @($sourcePath,$headerPath,$bsdPath)) {
    if(!(Test-Path -LiteralPath $path)) {throw "Missing pinned reference: $path; see qemu-iso.md"}
}
$expected=@{
    $sourcePath='B1C26832C25D71BCA4A50668F08A3728B26C6E2D398DF4D5AFFB0DA063B33E0F'
    $headerPath='01164AC769E7C90E5BFE0991CF40E0577B80EBD35ED73486994E6F8630FE052B'
    $bsdPath='0649D566561CE1A8B0F7521C4C62E046F7EF81CEFE0A993C9463A3E3F4A8A328'
}
foreach($path in $expected.Keys) {
    if((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $expected[$path]) {
        throw "Pinned reference hash changed: $path"
    }
}
$source=[IO.File]::ReadAllText($sourcePath)
$header=[IO.File]::ReadAllText($headerPath)
$start=$source.IndexOf('enum libusb_transfer_status usbd_status_to_libusb_transfer_status(')
if($start -lt 0) {throw 'Pinned mapper function not found'}
$brace=$source.IndexOf('{',$start);$depth=0;$end=-1
for($i=$brace;$i -lt $source.Length;$i++) {
    if($source[$i] -eq '{') {$depth++}
    elseif($source[$i] -eq '}') {$depth--;if($depth -eq 0) {$end=$i+1;break}}
}
if($end -lt 0) {throw 'Unbalanced mapper source'}
$mapper=$source.Substring($start,$end-$start)
$notice=$source.Substring(0,$source.IndexOf('*/')+2)
$bsd=[IO.File]::ReadAllText($bsdPath)
$success=[regex]::Match($header,'(?m)^#define USBD_SUCCESS[^\r\n]+').Value
$constants=[regex]::Matches($header,'(?m)^#define USBD_STATUS_(STALL_PID|ENDPOINT_HALTED|TIMEOUT|DEVICE_GONE|CANCELED)[^\r\n]+')
if(!$success -or $constants.Count -ne 5) {throw 'Pinned status definitions changed'}
$prefix=@'
#include <stdio.h>
#include <stdlib.h>
typedef long USBD_STATUS;
typedef char RequireLong32[(sizeof(long)==4)?1:-1];
enum libusb_transfer_status {
 LIBUSB_TRANSFER_COMPLETED,LIBUSB_TRANSFER_ERROR,LIBUSB_TRANSFER_TIMED_OUT,
 LIBUSB_TRANSFER_CANCELLED,LIBUSB_TRANSFER_STALL,LIBUSB_TRANSFER_NO_DEVICE,
 LIBUSB_TRANSFER_OVERFLOW
};
#define usbi_dbg(a,b,c) ((void)0)
'@
$harness=@'
/* Original harness. Packed-status semantics from BSD drv_xfer_iso.c;
 * no kernel implementation is compiled. */
static enum libusb_transfer_status ProposedKiso(unsigned short packed)
{return packed ? LIBUSB_TRANSFER_ERROR : LIBUSB_TRANSFER_COMPLETED;}
static void Check(int yes)
{if(!yes){puts("FAIL");exit(1);}}
int main(void)
{
 static const unsigned long errors[]={
  0xc0000001UL,0xc0000004UL,0xc0000009UL,0xc0000a00UL,
  0xc0010000UL,0xc0020000UL,0xc0030000UL,0xc0040000UL,0xc0050000UL
 };
 unsigned int i;unsigned short folded;
 Check(usbd_status_to_libusb_transfer_status(0)==LIBUSB_TRANSFER_COMPLETED);
 for(i=0;i<sizeof(errors)/sizeof(errors[0]);i++) {
  folded=(unsigned short)((errors[i]&65535UL)|(errors[i]>>16));
  Check(usbd_status_to_libusb_transfer_status((USBD_STATUS)errors[i])!=LIBUSB_TRANSFER_COMPLETED);
  Check(usbd_status_to_libusb_transfer_status(folded)==LIBUSB_TRANSFER_COMPLETED);
  Check(ProposedKiso(folded)==LIBUSB_TRANSFER_ERROR);
  printf("USBD=%08lX packed=%04X upstream=COMPLETED proposed=ERROR\n",errors[i],(unsigned int)folded);
 }
 for(i=0;i<=65535u;i++) {
  Check(usbd_status_to_libusb_transfer_status((unsigned short)i)==LIBUSB_TRANSFER_COMPLETED);
  Check((ProposedKiso((unsigned short)i)==LIBUSB_TRANSFER_COMPLETED)==(i==0));
 }
 puts("PASS: actual pinned mapper accepts all 65536 packed statuses; proposed boundary rejects all nonzero statuses");
 puts("Source-level reproduction only; no physical device or installed DLL behavior established.");
 return 0;
}
'@
New-Item -ItemType Directory -Force $WorkDirectory | Out-Null
$nl=[Environment]::NewLine
$generated=$notice+$nl+"/* libusbK BSD reference notice:"+$nl+$bsd+$nl+"*/"+$nl+$prefix+$nl+$success+$nl+
    (($constants | ForEach-Object {$_.Value}) -join $nl)+$nl+$mapper+$nl+$harness
[IO.File]::WriteAllText((Join-Path $WorkDirectory 'status.c'),$generated,[Text.UTF8Encoding]::new($false))
$vc=Join-Path $repo 'tools/MSVC600/VC98'
$saved=$env:PATH
try {
    $env:PATH="$vc/BIN;$repo/tools/MSVC600/Common/MSDev98/Bin;$saved"
    Push-Location $WorkDirectory
    try {
        & "$vc/BIN/cl.exe" /nologo /W4 /WX /O2 /MD /I"$vc/Include" /Festatus.exe status.c /link /libpath:"$vc/Lib"
        if($LASTEXITCODE) {throw 'Mapper diagnostic compile failed'}
        & ./status.exe | Tee-Object -FilePath result.txt
        if($LASTEXITCODE) {throw 'Mapper diagnostic failed'}
    } finally {Pop-Location}
} finally {$env:PATH=$saved}
