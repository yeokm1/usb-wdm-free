NOAUD98.INF - Windows 98 SE: a webcam's microphone with no driver
==================================================================

On Windows 98 SE, if plugging a webcam freezes Windows, install
NOAUD98.INF for its microphone; the camera keeps working and the
microphone is ignored.

Why: 98 SE loads its own USB audio driver (USBAUDIO.SYS) for a camera's
microphone, and that load can freeze Windows at the plug ("Msgsrv32 not
responding" in the Ctrl+Alt+Del list). NOAUD98.INF gives the microphone
no driver at all, so USBAUDIO.SYS never loads for it. The camera's video
is a different function and keeps its own driver.

Devices it lists (only these; it never matches by class):
  Logitech Brio 100 microphone   USB\VID_046D&PID_094C&MI_02
  QEMU emulated usb-audio        USB\VID_46F4&PID_0002&MI_00

Installing (Windows 98 SE):
  1. Unplug the camera if Windows froze, and restart.
  2. With the camera plugged in, open Device Manager, find the camera's
     "USB Audio Device" (under "Sound, video and game controllers"),
     Properties, Driver, Update Driver.
  3. Choose "Display a list of all the drivers in a specific location",
     Have Disk, and point it at this folder. Pick the "(no driver)" line.
  A device that was never installed: point the Add New Hardware Wizard
  at this folder instead.

Undoing it: remove the "(no driver)" device in Device Manager and unplug
and replug the camera; Windows then offers its own driver again.

The installer never stages this file by itself (STAGE.TXT: COPY): it is
copied with the other drivers and used only when you install it.
