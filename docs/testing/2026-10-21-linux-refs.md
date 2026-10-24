# Pinned Linux USB audio/video references, 2026-10-21

These are downloaded source references, not code incorporated into this
repository's drivers and not a build/runtime dependency. The owner
explicitly requested Linux UAC2/UVC reference downloads. No driver source
was copied or adapted by this acquisition task.

## Official origin and exact revision

- Repository: [torvalds/linux](https://github.com/torvalds/linux).
- Requested release tag: [`v6.12`](https://github.com/torvalds/linux/tree/v6.12).
- Annotated tag object: `06090c9b622a7e1f797e775db4c035e0d779b76e`.
- Resolved commit: [`adc218676eef25575469234709c2d87185ca223a`](https://github.com/torvalds/linux/commit/adc218676eef25575469234709c2d87185ca223a).
- Tag/ref resolution used the official GitHub repository API:
  [tag ref](https://api.github.com/repos/torvalds/linux/git/ref/tags/v6.12)
  and [annotated tag object](https://api.github.com/repos/torvalds/linux/git/tags/06090c9b622a7e1f797e775db4c035e0d779b76e).
- Files were fetched from
  `https://raw.githubusercontent.com/torvalds/linux/adc218676eef25575469234709c2d87185ca223a/<path>`.
  No mutable branch-head URLs were used for file contents.

The bounded set contains 58 files, totaling 1114704 bytes,
under ignored `docs/references/linux-v6.12/`, preserving upstream paths and
original bytes. `manifest.json` there records raw URLs, byte counts,
SHA-256, Git blob SHA-1, exact SPDX lines and acquisition time.
`tag-origin.json` preserves API provenance. All local SHA-256 values were
recomputed, and all Git blob identifiers matched the pinned commit's
GitHub contents API records. Tag signature verification was not performed.

## Scope and use

The audio selection includes clock/format/endpoint/PCM/mixer/card/helper/
stream/implicit handling, power, descriptor validation, supporting quirks
and proc diagnostics, plus top-level local headers. MIDI and vendor mixer
implementation bodies were not broadly downloaded; their small local
headers remain where referenced. The video selection contains all C/H
files at `drivers/media/usb/uvc/`, plus the relevant shared and UAPI headers.
This is not a complete kernel checkout or a self-contained kernel build.

Useful starting points for review, without treating Linux behavior as a
substitute for the USB specifications or Windows contracts:

- `sound/usb/clock.c` and `format.c`: clock path, rate negotiation and format
  validation; compare device quirks against observed descriptors/responses.
- `sound/usb/endpoint.c`, `pcm.c` and `implicit.c`: packet cadence, feedback,
  transfer completion and explicitly paired implicit feedback. Linux URB
  APIs are not the Windows WDM URB ABI.
- `sound/usb/mixer.c`, `mixer_maps.c` and `mixer_quirks.c`: control topology
  and device-specific policy. Do not infer a required vendor write without
  a matching device and evidence.
- `drivers/media/usb/uvc/uvc_driver.c`, `uvc_video.c`, `uvc_queue.c` and
  `uvc_ctrl.c`: descriptors, negotiation, frame assembly, buffering and
  controls; preserve the repository design's narrower initial scope.
- `include/uapi/linux/usb/audio.h`, `include/linux/usb/audio-v2.h` and
  `include/uapi/linux/usb/video.h`: class constants and descriptor layouts.
  `audio-v2.h` is an internal kernel header, not in the UAPI directory.

## License preservation

Every downloaded C/H file retains its original SPDX and copyright text.
The exact SPDX expression found in each source is indexed below. The
unmodified upstream `COPYING`, `LICENSES/preferred/GPL-2.0` and
`LICENSES/exceptions/Linux-syscall-note` are retained too. The syscall
exception belongs to the specifically marked UAPI headers; it does not
relicense the audio/video implementation files.

If a future implementation adapts code, preserve its notices/SPDX and add
a `NOTICE.md` row for each originating file as `AGENTS.md` requires. A
reference download itself is not an adaptation, so no source-origin row
was added for this task. Keep references separate from released packages;
all drivers must continue to build and run without this folder.

## Per-file SHA-256 and upstream SPDX

Each linked path opens the official source at the resolved commit.
For the two license-text files, the SPDX strings inside the license text
are accepted identifiers/examples rather than a source-file license tag.

| Original path at pinned commit | SHA-256 | SPDX expression / role |
|---|---|---|
| [COPYING](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/COPYING) | `fb5a425bd3b3cd6071a3a9aff9909a859e7c1158d54d32e07658398cd67eb6a0` | `GPL-2.0 WITH Linux-syscall-note` |
| [LICENSES/exceptions/Linux-syscall-note](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/LICENSES/exceptions/Linux-syscall-note) | `8e378ab93586eb55135d3bc119cce787f7324f48394777d00c34fa3d0be3303f` | `License/exception text; original identifier declarations preserved` |
| [LICENSES/preferred/GPL-2.0](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/LICENSES/preferred/GPL-2.0) | `f6b78c087c3ebdf0f3c13415070dd480a3f35d8fc76f3d02180a407c1c812f79` | `License/exception text; original identifier declarations preserved` |
| [drivers/media/usb/uvc/uvc_ctrl.c](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/media/usb/uvc/uvc_ctrl.c) | `0fa6f43b40a46b771ffc7443d234429ae80653dd26b3fcbc8595434c584c2a4f` | `GPL-2.0-or-later` |
| [drivers/media/usb/uvc/uvc_debugfs.c](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/media/usb/uvc/uvc_debugfs.c) | `ea533bb7df627a64f436b9d9cdff9d3b73539ace5945790239f9f0615c44846c` | `GPL-2.0-or-later` |
| [drivers/media/usb/uvc/uvc_driver.c](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/media/usb/uvc/uvc_driver.c) | `d4016513ffd68f3cf79b4d8044fc52807d08274bd32d27ca78016e10061bb87b` | `GPL-2.0-or-later` |
| [drivers/media/usb/uvc/uvc_entity.c](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/media/usb/uvc/uvc_entity.c) | `60e5c133f3f601055f70f389caa855d1934ce4d7ea76e1c7c53708cea0b6b45c` | `GPL-2.0-or-later` |
| [drivers/media/usb/uvc/uvc_isight.c](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/media/usb/uvc/uvc_isight.c) | `410052c97e113b08279af9b7b851e7ba034948646b3c17262c04b3762941d8b2` | `GPL-2.0-or-later` |
| [drivers/media/usb/uvc/uvc_metadata.c](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/media/usb/uvc/uvc_metadata.c) | `2ca4b9a8f0ac800cdefd38d4b037e4550dc49a05369f3588662ad6acda204174` | `GPL-2.0-or-later` |
| [drivers/media/usb/uvc/uvc_queue.c](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/media/usb/uvc/uvc_queue.c) | `c83d81dcb98cc4fede74fb93a29293f341d39bab09e78b1fcd21663717cd7cb4` | `GPL-2.0-or-later` |
| [drivers/media/usb/uvc/uvc_status.c](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/media/usb/uvc/uvc_status.c) | `943c44a4ea9379233f66ebe2a2f321e568cbee32910207c210188d578663b72e` | `GPL-2.0-or-later` |
| [drivers/media/usb/uvc/uvc_v4l2.c](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/media/usb/uvc/uvc_v4l2.c) | `5b104387ae04e58b12f7e4c5f741658ee572ca0faf2f8e1f6b6d90da1d3f7d62` | `GPL-2.0-or-later` |
| [drivers/media/usb/uvc/uvc_video.c](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/media/usb/uvc/uvc_video.c) | `d5c1f227b0423736a4acfeb9da8ff57ff5f44c9fd8df33af56280cb76a20b2a9` | `GPL-2.0-or-later` |
| [drivers/media/usb/uvc/uvcvideo.h](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/drivers/media/usb/uvc/uvcvideo.h) | `73712008b5bc21967246b49c8499ecdb80dc006324d507395a6560a9dfb01390` | `GPL-2.0` |
| [include/linux/usb/audio-v2.h](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/include/linux/usb/audio-v2.h) | `9e5608aa0d1117ce48c0ad573f080a81a57fef767ada6b72606beed30c1612ba` | `GPL-2.0` |
| [include/linux/usb/audio-v3.h](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/include/linux/usb/audio-v3.h) | `ca4967a7d8ad7494fbe9d5237c445018dba1194ac057ad451585799bbb521b95` | `GPL-2.0+` |
| [include/linux/usb/audio.h](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/include/linux/usb/audio.h) | `c87cd03af9f41ea4f8d06c452bb16a27970b50d675cebcdd6dbe37fe3a8fadb8` | `GPL-2.0` |
| [include/linux/usb/uvc.h](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/include/linux/usb/uvc.h) | `ee2cf76dbedc43ba6409c6fa753c360034de6803141fef592cc294bb603af276` | `GPL-2.0-or-later` |
| [include/uapi/linux/usb/audio.h](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/include/uapi/linux/usb/audio.h) | `d00a53ba5f0e75e485d7642bfc9a6a3d355429878804d6818aff217fe9508a2e` | `GPL-2.0 WITH Linux-syscall-note` |
| [include/uapi/linux/usb/video.h](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/include/uapi/linux/usb/video.h) | `9e779f3bc9d913330cca54252002469da499c31e4966d714712444aaca3d7b85` | `GPL-2.0 WITH Linux-syscall-note` |
| [include/uapi/linux/uvcvideo.h](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/include/uapi/linux/uvcvideo.h) | `81b07ef000578247799e229c65b329ab2faa7b0527138956747d898574fb04c7` | `GPL-2.0 WITH Linux-syscall-note` |
| [sound/usb/card.c](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/card.c) | `13edebb0dcca58399629496519e8776f372be20295cc33f9039f6e8a9640b9e6` | `GPL-2.0-or-later` |
| [sound/usb/card.h](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/card.h) | `c7f412baf52f148a328af76890132a6f06cd9ad5a34c72c6d3cda3f381f37ca9` | `GPL-2.0` |
| [sound/usb/clock.c](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/clock.c) | `78052101dac6673e0c15b0b13847c502f5499c97c549ff95c3b4301402523997` | `GPL-2.0-or-later` |
| [sound/usb/clock.h](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/clock.h) | `c787481e024edd5b873a101fc0ac9434d02706c262e13e4abe293928ca636740` | `GPL-2.0` |
| [sound/usb/endpoint.c](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/endpoint.c) | `e66abf14e2991c5aece47b05ae9f6e2963123bf12b5244423bff9d64667af117` | `GPL-2.0-or-later` |
| [sound/usb/endpoint.h](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/endpoint.h) | `4b4d5236e893c4745581779917f3fcc3852483d7494fee672ceb8a086088d7da` | `GPL-2.0` |
| [sound/usb/format.c](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/format.c) | `5be75845aed00f601532c3f4b797eb434c5d6fa1547afdc5bcaea5a344574b27` | `GPL-2.0-or-later` |
| [sound/usb/format.h](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/format.h) | `071e9bcd00fe46a984e82371bf4aa774cb1cc89c641527c4d0fccad89a93d267` | `GPL-2.0` |
| [sound/usb/helper.c](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/helper.c) | `1b97ec0634156b0dc300252c372a7adcf40bc37c0504b12dccdd9d22e3556b85` | `GPL-2.0-or-later` |
| [sound/usb/helper.h](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/helper.h) | `04edf41a5e43fac6cacf8570b53751eb56050fd603672bcade3f3e36a47e0578` | `GPL-2.0` |
| [sound/usb/implicit.c](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/implicit.c) | `3f49db7c2cd8614448ecf292465dfec84a89c8f2833ce5b1515c1cfc33e7302d` | `GPL-2.0-or-later` |
| [sound/usb/implicit.h](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/implicit.h) | `e8ee2a16c661973d6d286e8c4eb3bb3a63cd4f721c62507339e28fefa19927e5` | `GPL-2.0` |
| [sound/usb/media.h](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/media.h) | `012f18c5801c67a34f1e4ba25a2273cda2b75f1f3e15e441a02c9fd59339c877` | `GPL-2.0+` |
| [sound/usb/midi.h](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/midi.h) | `e792a2368b86db26a905af06f68594ffaadd29ba8af213281c64a872412144cf` | `GPL-2.0` |
| [sound/usb/midi2.h](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/midi2.h) | `cb929ef1a37d52885b9c45c87092f2c5f332224cb39f1eabfb156a1352fccedd` | `GPL-2.0-or-later` |
| [sound/usb/mixer.c](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/mixer.c) | `05d42ff40ea061dc7a0e050d25f6d7a65a3ae4a20f25605e1d7791262165a347` | `GPL-2.0-or-later` |
| [sound/usb/mixer.h](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/mixer.h) | `66a3ac81c97e1fa4c3a5c796b33d0c23587b4f8e6a722dd5ba86a644a7d0ea1d` | `GPL-2.0` |
| [sound/usb/mixer_maps.c](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/mixer_maps.c) | `b7e6be11a24dc6b82cb07771fa3afb42e5c3a6ca397c07bb995a0d5633c1edf6` | `GPL-2.0-or-later` |
| [sound/usb/mixer_quirks.c](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/mixer_quirks.c) | `308518d3ca13bc4a9052b37c1b1cd4a308a18c6641412df0182023dbe3de1242` | `GPL-2.0-or-later` |
| [sound/usb/mixer_quirks.h](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/mixer_quirks.h) | `53bcf65b0e95eec0f2d5a265e1610ce914d205c1ebe9ae5521895dd71e6236ed` | `GPL-2.0` |
| [sound/usb/mixer_s1810c.h](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/mixer_s1810c.h) | `edb29231e2cb9d3b899e697e62d0de1ca1b1b22a60cfad4ce451c2db63da7b59` | `GPL-2.0` |
| [sound/usb/mixer_scarlett.h](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/mixer_scarlett.h) | `812cfc10353696978964b7d251b4cedcf72c66ca8173e738bda8c2d83c6bf4a4` | `GPL-2.0` |
| [sound/usb/mixer_scarlett2.h](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/mixer_scarlett2.h) | `7079ced0c7113fa35ca3d7cb0bd4393286de962564ba1e8153cf28dd62b9f969` | `GPL-2.0` |
| [sound/usb/mixer_us16x08.h](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/mixer_us16x08.h) | `f2dc734848666adf9bcd1f2d5670415bb072f7d0d882569c7bb4a9553acb2f1d` | `GPL-2.0` |
| [sound/usb/pcm.c](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/pcm.c) | `ae2aa23f60badff890359852c856215f26f1a796dec1d6a4a2c5c0fa216395e7` | `GPL-2.0-or-later` |
| [sound/usb/pcm.h](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/pcm.h) | `469f371995c08550970bf435feb0f9787d05d71ac026e50cf475a87be2108dd8` | `GPL-2.0` |
| [sound/usb/power.c](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/power.c) | `6d4cbdbb7d4d40ebddf9b87da54a1727e2b902b0e8bc07ab8910db50278eaa64` | `GPL-2.0` |
| [sound/usb/power.h](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/power.h) | `d0dad30f209c6498701d7bde2d4a198c529fc3e86f95993e8e5317f608ab961c` | `GPL-2.0` |
| [sound/usb/proc.c](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/proc.c) | `2b26e28a5435da29c5af916d4ede6a9f4455c08fe70393da18d760413bc0fcce` | `GPL-2.0-or-later` |
| [sound/usb/proc.h](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/proc.h) | `8a4a31b9b90ed7bde0a5d498b13469802409bf9033768e59e21a672d9b8bf3e8` | `GPL-2.0` |
| [sound/usb/quirks-table.h](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/quirks-table.h) | `874c8a0ef0fcd796e40385671893591b238e2ee4095fcf9e5673bb6de690ad99` | `GPL-2.0-or-later` |
| [sound/usb/quirks.c](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/quirks.c) | `26cfb822c1836ffd3ae1fbb051d5cff2816dedbefe1c537d6eb89668dbb29b3b` | `GPL-2.0-or-later` |
| [sound/usb/quirks.h](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/quirks.h) | `d7907c00421239959b7fdcc1d768a4e9617cb6b0a16be3e5c808bf6288a441d3` | `GPL-2.0` |
| [sound/usb/stream.c](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/stream.c) | `87d1037ca004432026e0a9e6f20542ec04035087d518b66f46c411880f66e806` | `GPL-2.0-or-later` |
| [sound/usb/stream.h](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/stream.h) | `ace2f8f1132da511d97cf50f2c75210d250651502f252679f5a68b8a8bd00570` | `GPL-2.0` |
| [sound/usb/usbaudio.h](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/usbaudio.h) | `b44b6e0c2186e053e9224928e1a7de775f09692a45362982783470677ec43d52` | `GPL-2.0-or-later` |
| [sound/usb/validate.c](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/validate.c) | `75d9ad1be26dfb56f36a116b3910963205f28527592af33b6a99dbaf6d211bbb` | `GPL-2.0-or-later` |

This record establishes the pinned reference set is locally available and
its notices and byte identities are preserved. It does not establish
conformance, successful playback/capture or target-OS support for any
Windows driver in this repository.

## Original narrow audio crosscheck

Read-only comparison of the pinned `sound/usb/endpoint.c` feedback handler
and `clock.c` clock-selection/rate-setting paths found these differences;
no Linux implementation was copied into the Windows transport.

- Linux ignores short/zero feedback and zero numerical values, retaining
  its prior rate. Our transport accepts a successful zero-length packet as
  unchanged only after an initial valid value, separately tracking packet
  freshness and value updates. This follows USB 2.0 section 5.12.4.2; the
  200 ms freshness policy is ours, not a requirement inferred from Linux.
- Linux normalizes several historical feedback formats and applies named
  device quirks. Our first X4 path deliberately requires normative HS
  four-byte feedback, with a nominal +/-12.5% and endpoint-capacity bound.
  This is a narrower compatibility policy, not evidence that devices
  needing a Linux format quirk violate the specification. No such quirk
  was inferred merely from a silent or unsuccessful playback test.
- Linux reads the leaf clock's current rate, writes a different supported
  rate, reads back and checks validity. That ordering agrees with the
  original X4 transport. Linux also reselects a writable clock selector
  even when it has one input. Our path resolves the single-input X4
  selector 34 to source 33 without that idempotent request. Captured
  selector controls advertise read/write; this sequencing difference was
  reported for coordinated investigation. It does not prove X4 requires
  the write, and no speculative selector change was made here.
- Named clock validity/settling exceptions in the inspected path concern
  other devices. No exact `041e:3278` match was found in the selected audio
  files; the bounded selection cannot establish that no relevant quirk
  exists elsewhere in Linux.

Sources are the immutable [feedback handler](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/endpoint.c)
and [clock handling](https://github.com/torvalds/linux/blob/adc218676eef25575469234709c2d87185ca223a/sound/usb/clock.c).
These are design comparisons, not hardware or Windows conformance results.

## Subsequent bounded reuse

The acquisition statements above describe the initial reference download.
Later, `src/audio/uac2/uacmix.c`/`.h` adapted the pinned `mixer.c` matrix-bit
algorithm and UAC2 tail-bounds rule into a portable checked helper. Upstream
notices/SPDX and per-file NOTICE rows are preserved. The
[adaptation and test record](2026-10-21-uac2-mixer.md) states the exact copied
semantics, new checks and remaining full-spec requirements. No build depends
on the ignored reference tree or fetches source from another repository.
