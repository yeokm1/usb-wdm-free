CAMCHECK.EXE - CamCheck webcam preview (usb-wdm-free 0.1.0)
===========================================================

CamCheck shows a webcam's live picture, to check that the camera and its
driver work. It works with this set's uvc98 webcam driver (in
free\video\uvc98) and with any other Video for Windows capture driver.

Systems: Windows 98 SE, ME and 2000.

Using it: run CAMCHECK.EXE; nothing is installed. Pick the capture
driver in the list and press Preview; Stop releases the camera. On these
systems every WDM webcam, uvc98's included, is under one entry,
"Microsoft WDM Image Capture (Win32)": with two or more cameras plugged
in, choose the camera in Source... (the driver's own dialog, which also
holds the picture settings). Format... sets the picture size and format.
Refresh lists the drivers again after a camera is plugged in or removed.
The status line shows the format and the frames received. CamCheck does
not record.

It uses Windows' own AVICAP32.DLL (Video for Windows) and MSVCRT.DLL.
