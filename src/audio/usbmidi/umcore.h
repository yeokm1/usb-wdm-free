/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original USB MIDI 1.0 codec/descriptor core; USB-IF MIDI 1.0 sections 4,6.
 */
#ifndef UMCORE_H
#define UMCORE_H
#ifdef __cplusplus
extern "C" {
#endif
typedef struct UM_ENCODER {
    unsigned char Cable, Running, Status, Need, Count, Sysex, Data[3];
} UM_ENCODER;
/* One state per cable. Feed returns 1 packet, 0 buffered, -1 invalid byte.
 * Caller supplies four output bytes. Realtime does not disturb pending data.
 * Reset drops incomplete messages, e.g. when closing a stream. No allocation.
 */
int UmEncoderInit(UM_ENCODER *s, unsigned int cable);
int UmEncodeByte(UM_ENCODER *s, unsigned char byte, unsigned char packet[4]);
/* Stateless packet decoding, including CIN F raw single-byte mode. Returns
 * payload bytes 1..3, or 0 for reserved/malformed/header/cable input. The
 * allowedCables bitmask is endpoint-specific. Padding is not delivered.
 */
unsigned int UmDecodePacket(const unsigned char *packet, unsigned int length,
                           unsigned int allowedCables, unsigned char data[3]);

#define UM_MAX_ENDPOINTS 30
typedef struct UM_ENDPOINT {
    unsigned char Address;    /* bEndpointAddress */
    unsigned char Type;       /* 2 bulk, 3 interrupt */
    unsigned char Interval;   /* bInterval as declared */
    unsigned char Cables;     /* bNumEmbMIDIJack, 1..16 */
    unsigned char Jack[16];   /* baAssocJackID per cable */
    unsigned char Name[16];   /* iJack string index per cable, 0 = none */
    unsigned short MaxPacket; /* wMaxPacketSize bits 10..0 */
} UM_ENDPOINT;
typedef struct UM_INTERFACE {
    unsigned char Number, Alternate, Count, Declared;
    UM_ENDPOINT Endpoint[UM_MAX_ENDPOINTS];
} UM_INTERFACE;
/* Parse one MIDIStreaming interface/alternate (class 01, subclass 03, MS
 * header bcdMSC 1.00) inside a bounded configuration blob. Every descriptor
 * boundary of the whole blob is validated. Inside the interface, as Linux's
 * snd_usbmidi_get_ms_info does, a data endpoint that is not bulk/interrupt
 * or lacks its class-specific MS endpoint descriptor is skipped, Element
 * and unknown class descriptors are skipped, and wTotalLength is recorded
 * but not enforced. At least one usable endpoint is required. Result is
 * zero on failure. No allocation, no Windows dependency.
 */
int UmParseInterface(const unsigned char *blob, unsigned int length,
                     unsigned int number, unsigned int alternate,
                     UM_INTERFACE *result);
/* Find the first MIDIStreaming interface in the blob and choose its
 * alternate: alternate 0 when it parses as MIDI 1.0, else the lowest one
 * that does (a MIDI 2.0 function keeps MIDI 1.0 on alternate 0 and its
 * bcdMSC 2.00 alternate fails the header check). Returns 0 when none.
 */
int UmFindInterface(const unsigned char *blob, unsigned int length,
                    UM_INTERFACE *result);

/* Ports: one per (endpoint, cable), in endpoint then cable order, up to
 * four data endpoints per direction and UM_MAX_PORTS per direction.
 */
#define UM_MAX_PORTS 32
#define UM_MAX_PIPES 4
typedef struct UM_PORT {
    unsigned char Pipe;   /* index into OutPipe/InPipe */
    unsigned char Cable;
    unsigned char Name;   /* iJack string index, 0 = none */
} UM_PORT;
typedef struct UM_PORTMAP {
    unsigned int Outputs, Inputs, OutPipes, InPipes;
    unsigned char OutPipe[UM_MAX_PIPES], InPipe[UM_MAX_PIPES]; /* endpoint index */
    UM_PORT Out[UM_MAX_PORTS], In[UM_MAX_PORTS];
} UM_PORTMAP;
/* Returns the number of filters, max(Outputs, Inputs); filter i pairs
 * output port i and input port i where each exists. 0 when no ports.
 */
unsigned int UmMapPorts(const UM_INTERFACE *iface, UM_PORTMAP *map);

/* Port name (design 5.4), UTF-16 code units, at most 31 plus NUL in out[32].
 * jack/product may be NULL or empty. With a jack name: the jack name alone
 * when it begins with the product name, else "product jack". Without one:
 * product, plus " MIDI n" (n = index+1) when several ports share it; with
 * no product string "USB MIDI" stands in for it. Returns the length.
 */
unsigned int UmPortName(const unsigned short *product, unsigned int productLength,
                        const unsigned short *jack, unsigned int jackLength,
                        int several, unsigned int index, unsigned short out[32]);
#ifdef __cplusplus
}
#endif
#endif
