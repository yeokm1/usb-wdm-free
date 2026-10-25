/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * Host-test stand-in for the NDIS 5 declarations netcore.c uses (see
 * shim/wdm.h). Names follow the documented NDIS 5 interface; OID and
 * status values are the documented ones; packet and buffer layouts are
 * this test's own. Identical copy in both packages.
 */
#ifndef SHIM_NDIS_H
#define SHIM_NDIS_H

#include "wdm.h"

typedef int NDIS_STATUS, *PNDIS_STATUS;
typedef PVOID NDIS_HANDLE, *PNDIS_HANDLE;
typedef ULONG NDIS_OID, *PNDIS_OID;
typedef LARGE_INTEGER NDIS_PHYSICAL_ADDRESS;
typedef UNICODE_STRING NDIS_STRING, *PNDIS_STRING;
typedef enum { NdisMedium802_3, NdisMedium802_5 } NDIS_MEDIUM, *PNDIS_MEDIUM;
typedef enum { NdisInterfaceInternal = 0 } NDIS_INTERFACE_TYPE;
typedef enum { NdisHardwareStatusReady } NDIS_HARDWARE_STATUS;
typedef enum { NdisMediaStateConnected, NdisMediaStateDisconnected } NDIS_MEDIA_STATE;
typedef enum { NdisDeviceStateUnspecified, NdisDeviceStateD0, NdisDeviceStateD1,
               NdisDeviceStateD2, NdisDeviceStateD3 } NDIS_DEVICE_POWER_STATE;
typedef enum { NdisParameterInteger, NdisParameterHexInteger, NdisParameterString,
               NdisParameterMultiString, NdisParameterBinary } NDIS_PARAMETER_TYPE;

#define NDIS_PHYSICAL_ADDRESS_CONST(l, h) { (ULONG)(l), (LONG)(h) }
#define NDIS_STRING_CONST(x) { sizeof(L##x) - 2, sizeof(L##x), (PWCHAR)L##x }

typedef struct _NDIS_CONFIGURATION_PARAMETER {
    NDIS_PARAMETER_TYPE ParameterType;
    union { ULONG IntegerData; NDIS_STRING StringData; } ParameterData;
} NDIS_CONFIGURATION_PARAMETER, *PNDIS_CONFIGURATION_PARAMETER;

typedef struct _NDIS_SPIN_LOCK { KSPIN_LOCK SpinLock; KIRQL OldIrql; } NDIS_SPIN_LOCK, *PNDIS_SPIN_LOCK;

typedef struct _NDIS_BUFFER {
    struct _NDIS_BUFFER *Next;
    PVOID Va;
    UINT Length;
    PVOID Pool;
} NDIS_BUFFER, *PNDIS_BUFFER;

typedef struct _NDIS_PACKET {
    UCHAR MiniportReserved[16];
    PNDIS_BUFFER Head;
    NDIS_STATUS Status;
    UINT HeaderSize;
    PVOID Pool;
    ULONG FakeMagic;
} NDIS_PACKET, *PNDIS_PACKET, **PPNDIS_PACKET;

typedef struct _NDIS_PNP_CAPABILITIES {
    ULONG Flags;
    struct { NDIS_DEVICE_POWER_STATE MinMagicPacketWakeUp, MinPatternWakeUp, MinLinkChangeWakeUp; } WakeUpCapabilities;
} NDIS_PNP_CAPABILITIES;

typedef struct _NDIS_MINIPORT_CHARACTERISTICS {
    UCHAR MajorNdisVersion, MinorNdisVersion;
    NDIS_STATUS (*InitializeHandler)(PNDIS_STATUS, PUINT, PNDIS_MEDIUM, UINT, NDIS_HANDLE, NDIS_HANDLE);
    VOID (*HaltHandler)(NDIS_HANDLE);
    NDIS_STATUS (*QueryInformationHandler)(NDIS_HANDLE, NDIS_OID, PVOID, ULONG, PULONG, PULONG);
    NDIS_STATUS (*SetInformationHandler)(NDIS_HANDLE, NDIS_OID, PVOID, ULONG, PULONG, PULONG);
    NDIS_STATUS (*ResetHandler)(PBOOLEAN, NDIS_HANDLE);
    VOID (*ReturnPacketHandler)(NDIS_HANDLE, PNDIS_PACKET);
    VOID (*SendPacketsHandler)(NDIS_HANDLE, PPNDIS_PACKET, UINT);
} NDIS_MINIPORT_CHARACTERISTICS;

#define NDIS_STATUS_SUCCESS           ((NDIS_STATUS)0x00000000L)
#define NDIS_STATUS_PENDING           ((NDIS_STATUS)0x00000103L)
#define NDIS_STATUS_MEDIA_CONNECT     ((NDIS_STATUS)0x4001000BL)
#define NDIS_STATUS_MEDIA_DISCONNECT  ((NDIS_STATUS)0x4001000CL)
#define NDIS_STATUS_FAILURE           ((NDIS_STATUS)0xC0000001L)
#define NDIS_STATUS_RESOURCES         ((NDIS_STATUS)0xC000009AL)
#define NDIS_STATUS_NOT_SUPPORTED     ((NDIS_STATUS)0xC00000BBL)
#define NDIS_STATUS_ADAPTER_NOT_FOUND ((NDIS_STATUS)0xC0010006L)
#define NDIS_STATUS_MULTICAST_FULL    ((NDIS_STATUS)0xC0010009L)
#define NDIS_STATUS_INVALID_LENGTH    ((NDIS_STATUS)0xC0010014L)
#define NDIS_STATUS_INVALID_DATA      ((NDIS_STATUS)0xC0010015L)
#define NDIS_STATUS_UNSUPPORTED_MEDIA ((NDIS_STATUS)0xC0010019L)
#define NDIS_ERROR_CODE_HARDWARE_FAILURE 0xC000138AL   /* netevent.h values */
#define NDIS_ERROR_CODE_ADAPTER_NOT_FOUND 0xC000138BL
#define NDIS_ATTRIBUTE_DESERIALIZE 0x00000020

#define NDIS_PACKET_TYPE_DIRECTED      0x00000001
#define NDIS_PACKET_TYPE_MULTICAST     0x00000002
#define NDIS_PACKET_TYPE_ALL_MULTICAST 0x00000004
#define NDIS_PACKET_TYPE_BROADCAST     0x00000008
#define NDIS_PACKET_TYPE_PROMISCUOUS   0x00000020
#define NDIS_MAC_OPTION_COPY_LOOKAHEAD_DATA 0x00000001
#define NDIS_MAC_OPTION_TRANSFERS_NOT_PEND  0x00000004
#define NDIS_MAC_OPTION_NO_LOOPBACK         0x00000008

#define OID_GEN_SUPPORTED_LIST          0x00010101
#define OID_GEN_HARDWARE_STATUS         0x00010102
#define OID_GEN_MEDIA_SUPPORTED         0x00010103
#define OID_GEN_MEDIA_IN_USE            0x00010104
#define OID_GEN_MAXIMUM_LOOKAHEAD       0x00010105
#define OID_GEN_MAXIMUM_FRAME_SIZE      0x00010106
#define OID_GEN_LINK_SPEED              0x00010107
#define OID_GEN_TRANSMIT_BUFFER_SPACE   0x00010108
#define OID_GEN_RECEIVE_BUFFER_SPACE    0x00010109
#define OID_GEN_TRANSMIT_BLOCK_SIZE     0x0001010A
#define OID_GEN_RECEIVE_BLOCK_SIZE      0x0001010B
#define OID_GEN_VENDOR_ID               0x0001010C
#define OID_GEN_VENDOR_DESCRIPTION      0x0001010D
#define OID_GEN_CURRENT_PACKET_FILTER   0x0001010E
#define OID_GEN_CURRENT_LOOKAHEAD       0x0001010F
#define OID_GEN_DRIVER_VERSION          0x00010110
#define OID_GEN_MAXIMUM_TOTAL_SIZE      0x00010111
#define OID_GEN_PROTOCOL_OPTIONS        0x00010112
#define OID_GEN_MAC_OPTIONS             0x00010113
#define OID_GEN_MEDIA_CONNECT_STATUS    0x00010114
#define OID_GEN_MAXIMUM_SEND_PACKETS    0x00010115
#define OID_GEN_VENDOR_DRIVER_VERSION   0x00010116
#define OID_GEN_NETWORK_LAYER_ADDRESSES 0x00010118
#define OID_GEN_TRANSPORT_HEADER_OFFSET 0x00010119
#define OID_GEN_XMIT_OK                 0x00020101
#define OID_GEN_RCV_OK                  0x00020102
#define OID_GEN_XMIT_ERROR              0x00020103
#define OID_GEN_RCV_ERROR               0x00020104
#define OID_GEN_RCV_NO_BUFFER           0x00020105
#define OID_GEN_DIRECTED_BYTES_XMIT     0x00020201
#define OID_GEN_DIRECTED_BYTES_RCV      0x00020207
#define OID_802_3_PERMANENT_ADDRESS     0x01010101
#define OID_802_3_CURRENT_ADDRESS       0x01010102
#define OID_802_3_MULTICAST_LIST        0x01010103
#define OID_802_3_MAXIMUM_LIST_SIZE     0x01010104
#define OID_802_3_RCV_ERROR_ALIGNMENT   0x01020101
#define OID_802_3_XMIT_ONE_COLLISION    0x01020102
#define OID_802_3_XMIT_MORE_COLLISIONS  0x01020103
#define OID_PNP_CAPABILITIES            0xFD010100
#define OID_PNP_SET_POWER               0xFD010101
#define OID_PNP_QUERY_POWER             0xFD010102

#define NdisMoveMemory(d, s, n) memcpy((d), (s), (n))
#define NdisZeroMemory(d, n) memset((d), 0, (n))
#define NdisEqualMemory(a, b, n) (memcmp((a), (b), (n)) == 0)
#define NDIS_SET_PACKET_STATUS(p, s) ((p)->Status = (s))
#define NDIS_SET_PACKET_HEADER_SIZE(p, s) ((p)->HeaderSize = (s))
#define NdisGetNextBuffer(b, n) (*(n) = (b)->Next)
#define NdisQueryBuffer(b, va, len) (*(va) = (b)->Va, *(len) = (b)->Length)
#define NdisAdjustBufferLength(b, len) ((b)->Length = (len))
#define NdisRecalculatePacketCounts(p) ((void)(p))

void NdisQueryPacket(PNDIS_PACKET p, PUINT physical, PUINT bufferCount, PNDIS_BUFFER *first, PUINT total);
void NdisChainBufferAtFront(PNDIS_PACKET p, PNDIS_BUFFER b);
void NdisUnchainBufferAtFront(PNDIS_PACKET p, PNDIS_BUFFER *b);
NDIS_STATUS NdisAllocateMemory(PVOID *p, UINT length, UINT flags, NDIS_PHYSICAL_ADDRESS highest);
void NdisFreeMemory(PVOID p, UINT length, UINT flags);
void NdisAllocatePacketPool(PNDIS_STATUS s, PNDIS_HANDLE pool, UINT count, UINT reserved);
void NdisFreePacketPool(NDIS_HANDLE pool);
void NdisAllocateBufferPool(PNDIS_STATUS s, PNDIS_HANDLE pool, UINT count);
void NdisFreeBufferPool(NDIS_HANDLE pool);
void NdisAllocatePacket(PNDIS_STATUS s, PNDIS_PACKET *p, NDIS_HANDLE pool);
void NdisFreePacket(PNDIS_PACKET p);
void NdisAllocateBuffer(PNDIS_STATUS s, PNDIS_BUFFER *b, NDIS_HANDLE pool, PVOID va, UINT length);
void NdisFreeBuffer(PNDIS_BUFFER b);
void NdisAllocateSpinLock(PNDIS_SPIN_LOCK l);
void NdisFreeSpinLock(PNDIS_SPIN_LOCK l);
void NdisAcquireSpinLock(PNDIS_SPIN_LOCK l);
void NdisReleaseSpinLock(PNDIS_SPIN_LOCK l);
void NdisOpenConfiguration(PNDIS_STATUS s, PNDIS_HANDLE h, NDIS_HANDLE config);
void NdisCloseConfiguration(NDIS_HANDLE h);
void NdisReadConfiguration(PNDIS_STATUS s, PNDIS_CONFIGURATION_PARAMETER *p, NDIS_HANDLE h,
                           PNDIS_STRING name, NDIS_PARAMETER_TYPE type);
void NdisWriteConfiguration(PNDIS_STATUS s, NDIS_HANDLE h, PNDIS_STRING name,
                            PNDIS_CONFIGURATION_PARAMETER p);
void NdisReadNetworkAddress(PNDIS_STATUS s, PVOID *address, PUINT length, NDIS_HANDLE h);
void NdisGetCurrentSystemTime(PLARGE_INTEGER t);
void NdisMSleep(ULONG microseconds);
void NdisWriteErrorLogEntry(NDIS_HANDLE h, ULONG code, ULONG count, ...);
void NdisMSetAttributesEx(NDIS_HANDLE h, NDIS_HANDLE context, UINT timeout, ULONG attributes,
                          NDIS_INTERFACE_TYPE type);
void NdisMGetDeviceProperty(NDIS_HANDLE h, PDEVICE_OBJECT *pdo, PDEVICE_OBJECT *fdo,
                            PDEVICE_OBJECT *next, PVOID raw, PVOID translated);
void NdisMIndicateReceivePacket(NDIS_HANDLE h, PPNDIS_PACKET packets, UINT count);
void NdisMIndicateStatus(NDIS_HANDLE h, NDIS_STATUS status, PVOID buffer, UINT length);
void NdisMIndicateStatusComplete(NDIS_HANDLE h);
void NdisMSendComplete(NDIS_HANDLE h, PNDIS_PACKET p, NDIS_STATUS status);
void NdisMSetInformationComplete(NDIS_HANDLE h, NDIS_STATUS status);
void NdisMResetComplete(NDIS_HANDLE h, NDIS_STATUS status, BOOLEAN addressing);
void NdisMInitializeWrapper(PNDIS_HANDLE wrapper, PVOID driver, PVOID path, PVOID unused);
NDIS_STATUS NdisMRegisterMiniport(NDIS_HANDLE wrapper, NDIS_MINIPORT_CHARACTERISTICS *chars, UINT size);
void NdisTerminateWrapper(NDIS_HANDLE wrapper, PVOID unused);

#endif
