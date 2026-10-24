# Physical Logitech Brio 100 descriptor fixture

Captured 2026-10-21 from the connected 046D:094C before any Zadig rebind,
using our read-only HUBDESC helper's explicit-target mode. The Windows
native webcam and microphone bindings were left unchanged. Only standard
device/configuration descriptors were requested through the USB hub; no
strings, video frames, microphone data, class controls or vendor requests.

The host identifies Brio 100 and libusb reports high speed. The actual
configuration is 2453 bytes: UVC 1.0, VC interface0, VS interface1, YUY2 and
MJPEG with 19 frame sizes each, plus a separate UAC1 microphone function
on interfaces2/3. YUY2 includes 160x120/320x240/640x480 at up to30fps and
1920x1080 at5fps; MJPEG advertises up to1920x1080 at30fps. There are11
nonzero isochronous video alternates, including high-bandwidth transactions.
The advertised format list is not a measured capture/performance result.

The actual compiled `UvcDescParse` accepted the immutable config with
VC anchor0/high speed: result0, Valid1, Revision0100, StreamCount1,
FrameCount38, AltCount11. Its UVC1.0 probe size is26 bytes. This makes the
camera a useful hardware candidate; it does not establish project driver
loading, negotiation, transport or visible frames on a target OS.

SHA-256:

- DEVICE.BIN: `89609e342a2db7ad8525ccc2db5ca03cab139b719480353ecb0978e52ac20289`
- CONFIG.BIN: `4c6975d2262f3267ccd639c23a0843c4c101f71ea99d6c3768d6eb8d14d7eeb7`

HUBDESC.TXT records hub observations and descriptor fields. No actual
device serial string was acquired; the device descriptor retains only its
string descriptor index. These are USB data fixtures, not driver binaries.
