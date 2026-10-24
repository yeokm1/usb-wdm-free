# Physical UM-ONE mk2 descriptor capture

Captured 2026-10-21 using the original native-hub standard descriptor
helper while the host Roland driver remained bound. Owner confirms the
physical switch is TAB. Descriptors alone do not establish switch state.
No strings, MIDI payload, vendor controls, configuration changes or resets
were requested. HUBDESC.TXT records the bounded capture.

- VID/PID 0582:012A, USB1.1, one configuration, one interface.
- Alt0: OUT02 bulk64 and IN81 bulk64.
- Alt1: OUT02 bulk64 and IN81 interrupt64, interval1.
- Roland F1/02 descriptor advertises one input and one output cable.
- DEVICE.BIN SHA256: 1F287C05BB7CE5D9A3D0DA1AB4FA3880434900D4822C9DF2A6AD933363F0322C.
- CONFIG.BIN SHA256: D1FB603C9EBEE334383FE0C848E96E7D886F92E0331EF4B1307976B815095CFF.

The strict standard MIDI parser deliberately rejects this vendor profile.
The separate UmMatchUmOne2 recognizer accepts only the captured descriptor
shape and exact identity, and does not issue I/O or weaken class matching.
Its output contains cable counts rather than invented standard jack IDs.
No physical MIDI transfer or project kernel transport claim is made.
