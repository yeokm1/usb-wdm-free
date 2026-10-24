/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original UAC2 scalar controls, USB-IF Audio2 September2026 chapter5.
 * No Windows dependency, allocation, packed structs or unaligned loads.
 */
#ifndef UACCTL_H
#define UACCTL_H
#ifdef __cplusplus
extern "C" {
#endif
#ifdef _MSC_VER
typedef __int64 UAC_CTL_INT;
#else
typedef long long UAC_CTL_INT;
#endif
#define UAC_CTL_CLOCK_SOURCE 1
#define UAC_CTL_CLOCK_SELECTOR 2
#define UAC_CTL_CLOCK_MULTIPLIER 3
#define UAC_CTL_SELECTOR 4
#define UAC_CTL_FEATURE 5
#define UAC_CTL_PARAM_EQ 6
#define UAC_CTL_ENCODER 7
#define UAC_CTL_AS_INTERFACE 8
#define UAC_CTL_ENDPOINT 9
#define UAC_CTL_MIXER 10
#define UAC_CTL_CUR 1
#define UAC_CTL_RANGE_ATTRIBUTE 2
#define UAC_CTL_ENTITY_RECIPIENT 1
#define UAC_CTL_INTERFACE_RECIPIENT 2
#define UAC_CTL_ENDPOINT_RECIPIENT 3
typedef struct UAC_CTL_POLICY {
    unsigned int Width,Signed,Range,ReadOnly,Channels,Recipient;
} UAC_CTL_POLICY;
typedef struct UAC_CTL_TARGET {
    /* MIXER family: Channel is zero-based MCN, MaxChannel is cells-1. */
    unsigned int Interface,Entity,Endpoint,Channel,MaxChannel;
} UAC_CTL_TARGET;
typedef struct UAC_CTL_RANGE {
    const unsigned char *Bytes; /* immutable caller-owned complete response */
    unsigned int Length,Count,Width,Signed;
} UAC_CTL_RANGE;
typedef struct UAC_CTL_TRIPLE {UAC_CTL_INT Min,Max,Res;} UAC_CTL_TRIPLE;
/* 0 absent, 1 readable, 3 read/write, -1 malformed/reserved lane (10b).
 * Selector is one-based. Call on the correct descriptor channel's bitmap.
 */
int UacCtlAccess(const unsigned char *bitmap,unsigned int length,unsigned int selector);
/* Explicit partial family policy; unknown/variable-layout selectors fail.
 * Output structs/bytes stay unchanged on failure for every API below.
 */
int UacCtlPolicy(unsigned int family,unsigned int selector,UAC_CTL_POLICY *out);
/* Caller must first establish target presence/topology and descriptor access.
 * read=1 GET,0 SET. rangeWritable is an independently established capability,
 * not inferred from bmControls. GET_RANGE permits count-only length2.
 * Serializes exactly8 setup bytes; never transmits any USB request itself.
 */
int UacCtlSetup(unsigned int family,unsigned int selector,const UAC_CTL_TARGET *target,
    unsigned int access,unsigned int attribute,unsigned int read,unsigned int length,
    unsigned int rangeWritable,unsigned char setup[8]);
/* Complete scalar CUR value validation for implemented policies; pinCount
 * supplies bNrInPins for clock/ordinary selectors. Vendor encoder Param1..8
 * have standardized signed32 wire representation, opaque parameter meaning.
 */
int UacCtlValue(unsigned int family,unsigned int selector,const unsigned char *bytes,
    unsigned int length,unsigned int pinCount);
/* Full RANGE response only (not a count prefix), width1/2/4, signed flag0/1.
 * Positive signed RES, single-value RES0, ascending disjoint intervals.
 * No unsupported assumption that (MAX-MIN) must divide evenly by RES.
 */
int UacCtlRange(const unsigned char *bytes,unsigned int length,unsigned int width,
    unsigned int isSigned,UAC_CTL_RANGE *out);
int UacCtlRangeGet(const UAC_CTL_RANGE *range,unsigned int index,UAC_CTL_TRIPLE *out);
int UacCtlRangeContains(const UAC_CTL_RANGE *range,UAC_CTL_INT value);
#ifdef __cplusplus
}
#endif
#endif
