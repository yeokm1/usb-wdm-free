/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * uac1 only (docs/design/uac2.md 26.12): the USB Audio 1.0 Type I wire
 * formats PCM8, IEEE_FLOAT, ALAW and MULAW against the 16-bit PCM the
 * pin offers (Formats 1.0 2.2.5, 2.2.6, ITU-T G.711), the MaxPacketsOnly
 * IN frame count (USB Audio 1.0 4.6.1.2, Formats 1.0 2.2.1) and an
 * adaptive source's synch value (USB Audio 1.0 3.7.2.2). Original code,
 * integer only. No Windows types, allocation, packed structures or
 * unaligned reads.
 */
#ifndef UACCONV_H
#define UACCONV_H
#ifdef __cplusplus
extern "C" {
#endif

/* USB Audio 1.0 Formats A.1.1 wFormatTag, Type I. */
#define UAC_TAG_PCM 1
#define UAC_TAG_PCM8 2
#define UAC_TAG_FLOAT 3
#define UAC_TAG_ALAW 4
#define UAC_TAG_MULAW 5
/* USB Audio 1.0 Table 4-21: the class-specific endpoint's bmAttributes D7,
 * MaxPacketsOnly (UAC_ALT.EpControls). */
#define UAC_CONV_MPO 0x80

/* Whether the driver runs the tag with this subframe and bit resolution:
 * PCM 2..4 bytes; PCM8, ALAW and MULAW 8 bits in 1..4 bytes (left-
 * justified: the subframe's last, most significant byte); IEEE_FLOAT 32
 * bits in 4 bytes. */
int UacConvKnown(unsigned int tag, unsigned int subframe, unsigned int bits);

/* The rank among wire formats with the same pin view (26.12): PCM 0,
 * IEEE_FLOAT 1, ALAW and MULAW 2, PCM8 3; 255 for anything else. */
unsigned int UacConvRank(unsigned int tag);

/* n 16-bit samples (little-endian) to n wire samples of subframe bytes
 * (a converted tag: UacConvKnown's layouts). */
void UacConvOut(unsigned int tag, unsigned int subframe, const unsigned char *pcm, unsigned int n,
                unsigned char *wire);
/* n wire samples of subframe bytes to 16-bit PCM (little-endian). */
void UacConvIn(unsigned int tag, unsigned int subframe, const unsigned char *wire, unsigned int n,
               unsigned char *pcm);

/* One sample each way, for the tests. */
unsigned int UacConvAlaw(int s);
int UacConvAlawDecode(unsigned int a);
unsigned int UacConvMulaw(int s);
int UacConvMulawDecode(unsigned int u);
unsigned long UacConvFloat(int s);
int UacConvFloatDecode(unsigned long bits);

/* A MaxPacketsOnly IN stream of a synchronous or adaptive source: the
 * nominal cadence (the rate, or for an adaptive source the Ff it is sent)
 * and the frames owed (the cadence's sum since the stream started or a
 * packet was lost, less the frames taken), and Formats 1.0 2.2.1's bounds
 * a packet's samples keep to: INT(nav) to INT(nav) + 1, or nav - 1 to
 * nav + 1 for a whole nav. */
typedef struct UAC_CONV_CADENCE {
    unsigned long Step, Base, Acc;
    long Owed;
    unsigned int Low, High;
} UAC_CONV_CADENCE;
/* rate 8000..384000; interval 1 (Full Speed) or 1..4 (High Speed); ff:
 * the Ff an adaptive source is sent (UacConvFf), 0 for the rate itself.
 * Returns 0 for arguments outside those limits. */
int UacConvCadence(UAC_CONV_CADENCE *c, unsigned long rate, int highSpeed, unsigned int interval,
                   unsigned long ff);
/* One service interval passes: its nominal frames are owed. Returns them. */
unsigned int UacConvNominal(UAC_CONV_CADENCE *c);
/* A packet lost, failed or null: the frames it held are unknown, so the
 * balance starts afresh (nothing owed either way). */
void UacConvLost(UAC_CONV_CADENCE *c);
/* packets packets lost together (a failed transfer): each one's interval
 * still passes (UacConvNominal), so the cadence's phase holds, and the
 * balance starts afresh after each (UacConvLost). */
void UacConvLostPackets(UAC_CONV_CADENCE *c, unsigned int packets);
/* The audio frames of one packet of bytes bytes (after UacConvNominal for
 * its interval): the count nearest the frames owed within Low..High, at
 * least up to its last frame with a nonzero byte, at most the whole
 * frames received; taken off the frames owed. *last (may be NULL): the
 * frames up to the last nonzero one (those after it are zero bytes). */
unsigned int UacConvMpoFrames(UAC_CONV_CADENCE *c, const unsigned char *packet,
                              unsigned int bytes, unsigned int frameBytes, unsigned int *last);

/* An adaptive source's Ff: Full Speed 10.14 samples a frame (3 bytes),
 * High Speed 16.16 samples a microframe (4 bytes); 0 outside 8000..384000. */
unsigned long UacConvFf(unsigned long rate, int highSpeed);

#ifdef __cplusplus
}
#endif
#endif
