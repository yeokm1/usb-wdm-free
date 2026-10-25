#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Yeo Kheng Meng
"""Compile actual QEMU ISO functions against bounded host-only test doubles.

Usage: python -B qemu-iso-test.py SOURCE_C GCC_EXE OUTPUT_DIR
The separately obtained QEMU source retains its own MIT copyright/licence.
No source is fetched, USB opened, or guest started by this test.
"""
import os
from pathlib import Path
import subprocess
import sys

source_path, compiler, output_dir = map(Path, sys.argv[1:4])
source = source_path.read_text(encoding="utf-8")
poll_mode = "static void usb_host_poll_notify(" in source
output_dir.mkdir(parents=True, exist_ok=True)


def function(name):
    marker = name + "("
    pos = source.index(marker)
    start = source.rfind("static ", 0, pos)
    opening = source.index("{", pos)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CONFIG_WIN32 1
#define LIBUSB_CALL
#define LIBUSB_TRANSFER_COMPLETED 0
#define USB_TOKEN_OUT 0
#define USB_TOKEN_IN 1
#define NANOSECONDS_PER_SECOND 1000000000LL
#define QEMU_CLOCK_REALTIME 0
#define QEMU_CLOCK_VIRTUAL 1
typedef struct { int length, actual_length, status; } IsoPacket;
typedef struct libusb_transfer { int num_iso_packets, length,status; unsigned char *buffer; void *user_data;
    IsoPacket iso_packet_desc[32]; } Transfer;
typedef struct { unsigned int max_packet_size; int pid, nr; } Endpoint;
typedef struct { int bus_num,addr; } USBHostDevice;
typedef struct { Endpoint *ep; int inflight,copy,unused; USBHostDevice *host; } Ring;
typedef struct { Transfer *xfer; Ring *ring; int packet; bool copy_complete; } USBHostIsoXfer;
typedef struct { int pid; struct { unsigned int size; } iov;
    unsigned char bytes[112]; } USBPacket;
static void libusb_set_iso_packet_lengths(Transfer *t, int size) {
    int i; for(i=0;i<t->num_iso_packets;i++) t->iso_packet_desc[i].length=size;
}
static unsigned char *libusb_get_iso_packet_buffer_simple(Transfer *t, int packet) {
    return t->buffer+t->iso_packet_desc[0].length*packet;
}
static unsigned char *libusb_get_iso_packet_buffer(Transfer *t, int packet) {
    int i,offset=0; for(i=0;i<packet;i++) offset+=t->iso_packet_desc[i].length;
    return t->buffer+offset;
}
static void usb_packet_copy(USBPacket *p, unsigned char *bytes, unsigned int size) {
    if(p->pid==USB_TOKEN_OUT) memcpy(bytes,p->bytes,size);
    else memcpy(p->bytes,bytes,size);
}
static uint32_t request_count,iso_request_count;
static void *poll_timer;
static int last_clock;
static int64_t now=123456789,deadline;
static int64_t qemu_clock_get_ns(int clock) { assert(clock==QEMU_CLOCK_VIRTUAL || clock==QEMU_CLOCK_REALTIME); last_clock=clock; return now; }
static void timer_mod(void *timer,int64_t when) { (void)timer; assert(last_clock==QEMU_CLOCK_VIRTUAL); deadline=when; }
static void timer_mod_anticipate_ns(void *timer,int64_t when) {
    (void)timer; assert(last_clock==QEMU_CLOCK_VIRTUAL); if(!deadline || when<deadline) deadline=when;
}
#define QTAILQ_REMOVE(head,item,field) do { (void)(item); --*(head); } while(0)
#define QTAILQ_EMPTY(head) (*(head)==0)
#define QTAILQ_INSERT_TAIL(head,item,field) do { (void)(item); ++*(head); } while(0)
#define trace_usb_host_iso_stop(bus,addr,ep) do { (void)(bus); (void)(addr); (void)(ep); } while(0)
static void fake_trace(int bus,int addr,int ep,void *transfer,int64_t ns,...) {
    (void)bus; (void)addr; (void)ep; (void)transfer; (void)ns;
}
#define trace_usb_host_iso_submit(...) fake_trace(__VA_ARGS__)
#define trace_usb_host_iso_callback(...) fake_trace(__VA_ARGS__)
static int orphan_traces __attribute__((unused)); static uintptr_t orphan_last __attribute__((unused));
static int orphan_vals[5] __attribute__((unused));
#define trace_usb_host_iso_orphan_callback(t,ns,st,pk,f,fi,a) do { orphan_traces++;     orphan_last=(uintptr_t)(t); (void)(ns); orphan_vals[0]=(st); orphan_vals[1]=(pk);     orphan_vals[2]=(f); orphan_vals[3]=(fi); orphan_vals[4]=(a); } while(0)
static int submit_result,freed_buffers,freed_transfers;
static int libusb_submit_transfer(Transfer *transfer) { (void)transfer; return submit_result; }
static void g_free(void *pointer) { freed_buffers++; free(pointer); }
static void libusb_free_transfer(Transfer *transfer) { freed_transfers++; free(transfer); }
static void usb_wakeup(Endpoint *ep,int stream) { (void)ep; (void)stream; }
'''

tests = r'''
static void packets(void) {
    unsigned char guarded[32*112+64],expected[32*112];
    Endpoint ep={112,USB_TOKEN_OUT,1}; Ring ring={0}; Transfer transfer={0};
    USBHostIsoXfer xfer={&transfer,&ring,0,false};
    unsigned int round,i,j,total,value=173;
    ring.ep=&ep; transfer.num_iso_packets=32; transfer.buffer=guarded+32;
    for(round=0;round<2000;round++) {
        memset(guarded,0xcc,sizeof(guarded));
        transfer.length=32*112;
        usb_host_iso_reset_xfer(&xfer); total=0;
        for(i=0;i<32;i++) {
            USBPacket p={0};
            value=value*1664525u+1013904223u;
            p.pid=USB_TOKEN_OUT;
            p.iov.size=round==1999 ? 0 : round%3==0 ? 24 : round%3==1 ? 4*(5+value%3) : 4*(value%29);
            for(j=0;j<p.iov.size;j++) p.bytes[j]=(unsigned char)(i*17+j+round);
            memcpy(expected+total,p.bytes,p.iov.size); total+=p.iov.size;
            assert(usb_host_iso_data_copy(&xfer,&p)==(i==31));
        }
        usb_host_iso_set_out_length(&xfer);
        assert(transfer.length==(int)total);
        assert(!memcmp(transfer.buffer,expected,total));
        for(i=0;i<32;i++) assert(guarded[i]==0xcc && guarded[sizeof(guarded)-1-i]==0xcc);
    }
    /* IN buffers keep requested MPS strides despite varying actual lengths. */
    usb_host_iso_reset_xfer(&xfer);
    for(i=0;i<32;i++) {
        USBPacket p={0}; p.pid=USB_TOKEN_IN; p.iov.size=112;
        transfer.iso_packet_desc[i].actual_length=i%5;
        memset(transfer.buffer+i*112,(int)i,112);
        assert(usb_host_iso_data_copy(&xfer,&p)==(i==31));
        for(j=0;j<i%5;j++) assert(p.bytes[j]==i);
    }
}
static void feedback(void) {
    unsigned char bytes[128]; Endpoint ep={4,USB_TOKEN_IN,3}; Ring ring={0};
    Transfer transfer={0}; USBHostIsoXfer xfer={&transfer,&ring,0,false}; unsigned int i,j;
    ring.ep=&ep; transfer.buffer=bytes; transfer.num_iso_packets=32; transfer.length=128;
    usb_host_iso_reset_xfer(&xfer);
    for(i=0;i<32;i++) {
        USBPacket p={0}; p.pid=USB_TOKEN_IN; p.iov.size=4;
        transfer.iso_packet_desc[i].actual_length=i%5;
        memset(bytes+i*4,(int)i,4); usb_host_iso_data_copy(&xfer,&p);
        for(j=0;j<i%5;j++) assert(p.bytes[j]==i);
    }
    assert(transfer.length==128);
}
#ifdef HAVE_COUNTED_SUBMIT
static void lifetime(void) {
    unsigned int i; USBHostDevice host={1,7}; Endpoint ep={4,USB_TOKEN_IN,3};
    Ring ring={0}; Transfer transfer={0}; USBHostIsoXfer xfer={&transfer,&ring,0,false};
    ring.ep=&ep; ring.host=&host; transfer.user_data=&xfer;
    iso_request_count=0; submit_result=-1;
#ifdef HAVE_HOST_POLL
    { unsigned int before=kicks;
    assert(usb_host_iso_submit(&xfer)==-1 && iso_request_count==0 && kicks==before); }
#else
    assert(usb_host_iso_submit(&xfer)==-1 && iso_request_count==0);
#endif
    submit_result=0; assert(usb_host_iso_submit(&xfer)==0 && iso_request_count==1);
    ring.inflight=1; usb_host_req_complete_iso(&transfer);
    assert(!iso_request_count && !ring.inflight && ring.copy==1 && !ring.unused);
    ep.pid=USB_TOKEN_OUT;
    assert(usb_host_iso_submit(&xfer)==0 && iso_request_count==1);
    ring.inflight=1; usb_host_req_complete_iso(&transfer);
    assert(!iso_request_count && !ring.inflight && ring.unused==1);
    /* Free-ring path clears user_data but transfers retain completion ownership. */
    for(i=0;i<17;i++) assert(usb_host_iso_submit(&xfer)==0);
    assert(iso_request_count==17);
    for(i=0;i<17;i++) {
        Transfer *orphan=calloc(1,sizeof(*orphan)); assert(orphan);
        orphan->buffer=malloc(128); assert(orphan->buffer);
        orphan->status=(int)i%2; orphan->num_iso_packets=3;
        orphan->iso_packet_desc[0].actual_length=4; orphan->iso_packet_desc[2].actual_length=(int)i;
        orphan->iso_packet_desc[1].status=(int)i%3 ? 5 : 0;
        { uintptr_t expect=(uintptr_t)orphan; int before=orphan_traces;
        usb_host_req_complete_iso(orphan);
        assert(iso_request_count==16-i && freed_buffers==(int)i+1 && freed_transfers==(int)i+1);
#ifdef HAVE_ORPHAN_TRACE
        /* Observation fix: every released-ring completion is traced once,
         * with its transfer pointer and packet summary, before it is freed. */
        assert(orphan_traces==before+1 && orphan_last==expect);
        assert(orphan_vals[0]==(int)i%2 && orphan_vals[1]==3 && orphan_vals[4]==4+(int)i);
        assert(orphan_vals[2]==((int)i%3 ? 1 : 0) && orphan_vals[3]==((int)i%3 ? 5 : 0));
#else
        (void)expect; (void)orphan_last; (void)orphan_vals; assert(orphan_traces==before);
#endif
        }
    }
}
#endif
static void timers(void) {
    deadline=0; request_count=0; iso_request_count=0;
    usb_host_timer_kick(); assert(deadline==now+1000000000LL);
    request_count=1; usb_host_timer_kick(); assert(deadline==now+10000000);
    request_count=0; iso_request_count=1;
    usb_host_timer_kick(); assert(deadline==now+1000000);
    now+=500000; usb_host_timer_kick(); assert(deadline==123456789+1000000);
}
int main(void) { packets(); feedback(); timers();
#ifdef HAVE_COUNTED_SUBMIT
    lifetime();
#endif
    puts("PASS actual QEMU functions: 2000 compact OUT/reuse patterns, IN strides/length, guards, VM-clock/deadline/ISO-poll, submit failure/orphan completion accounting and tracing"); return 0; }
'''

if poll_mode:
    prefix = prefix.replace("static void *poll_timer;", "")
    prefix += r"""
#include <sys/time.h>
#define HAVE_HOST_POLL 1
#define LIBUSB_API_VERSION 0x01000106
#define LIBUSB_OPTION_LOG_LEVEL 3
typedef struct Notifier Notifier;
struct Notifier { void (*notify)(Notifier *,void *); };
typedef struct { int value; } VMChangeStateEntry;
typedef struct { int state; uint32_t timeout; } MainLoopPoll;
typedef int RunState;
enum { MAIN_LOOP_POLL_FILL, MAIN_LOOP_POLL_ERR, MAIN_LOOP_POLL_OK };
#define MIN(a,b) ((a)<(b)?(a):(b))
#define g_assert_not_reached() assert(0)
static bool running=true,usb_host_poll_dispatching,nested_dispatch;
static unsigned int kicks,pumps,bql_checks;
static void *ctx=(void *)0x1234;
static int loglevel=4,init_result,init_calls,notifier_registrations,vm_registrations;
static Notifier usb_host_poll_notifier;
static VMChangeStateEntry *usb_host_poll_vmstate,vm_entry;
static void (*registered_vm_change)(void *,bool,RunState);
static int libusb_init(void **context) {
    init_calls++;if(init_result)return init_result;*context=(void *)0x1234;return 0;
}
static int libusb_set_option(void *context,int option,int level) {
    assert(context==ctx && option==LIBUSB_OPTION_LOG_LEVEL && level==4);return 0;
}
static void main_loop_poll_add_notifier(Notifier *n) {
    assert(n==&usb_host_poll_notifier && n->notify);notifier_registrations++;
}
static VMChangeStateEntry *qemu_add_vm_change_state_handler(void (*cb)(void *,bool,RunState),void *opaque) {
    assert(!opaque);vm_registrations++;registered_vm_change=cb;return &vm_entry;
}
static bool bql_locked(void) { bql_checks++; return true; }
static bool runstate_is_running(void) { return running; }
static void qemu_notify_event(void) { kicks++; }
static void usb_host_poll_notify(Notifier *,void *);
static int libusb_handle_events_timeout(void *context,struct timeval *tv) {
    assert(context==ctx && tv->tv_sec==0 && tv->tv_usec==0 && running);
    pumps++;
    if(nested_dispatch) {
        MainLoopPoll inner={MAIN_LOOP_POLL_OK,999};
        nested_dispatch=false; usb_host_poll_notify(NULL,&inner);
    }
    return 0;
}
"""
    start = tests.index("static void timers(void)")
    end = tests.index("int main(void)", start)
    tests = tests[:start] + r"""
static void timers(void) {
    unsigned int i; uint32_t seed=191;
    MainLoopPoll p;
    ctx=NULL;init_result=-1;
    assert(usb_host_init()==-1 && init_calls==1 && !ctx && !notifier_registrations && !vm_registrations);
    init_result=0;
    assert(usb_host_init()==0 && init_calls==2 && notifier_registrations==1 && vm_registrations==1);
    assert(usb_host_poll_notifier.notify==usb_host_poll_notify && registered_vm_change==usb_host_poll_vm_change && usb_host_poll_vmstate==&vm_entry);
    assert(usb_host_init()==0 && init_calls==2 && notifier_registrations==1 && vm_registrations==1);
    request_count=0;iso_request_count=0;kicks=pumps=bql_checks=0;
    usb_host_poll_kick(); assert(kicks==1);
    usb_host_poll_vm_change(NULL,false,0);assert(kicks==1);
    usb_host_poll_vm_change(NULL,true,0);assert(kicks==2);
    for(i=0;i<50000;i++) {
        uint32_t expected;
        seed=seed*1664525U+1013904223U;
        p.state=MAIN_LOOP_POLL_FILL;p.timeout=seed;
        request_count=i%3==0?0:UINT32_MAX;
        iso_request_count=i%7==0?UINT32_MAX:0;
        expected=(request_count || iso_request_count)?1:1000;
        if(expected>seed)expected=seed;
        running=true;usb_host_poll_notify(NULL,&p);assert(p.timeout==expected);
        running=false;p.timeout=seed;usb_host_poll_notify(NULL,&p);assert(p.timeout==seed);
        p.state=i%2?MAIN_LOOP_POLL_OK:MAIN_LOOP_POLL_ERR;
        usb_host_poll_notify(NULL,&p);assert(pumps==0);
    }
    running=true; request_count=iso_request_count=0;
    p.state=MAIN_LOOP_POLL_FILL;p.timeout=0;
    usb_host_poll_notify(NULL,&p);assert(p.timeout==0 && !pumps);
    p.timeout=1;usb_host_poll_notify(NULL,&p);assert(p.timeout==1);
    p.timeout=UINT32_MAX;usb_host_poll_notify(NULL,&p);assert(p.timeout==1000);
    p.state=MAIN_LOOP_POLL_OK;usb_host_poll_notify(NULL,&p);assert(pumps==1);
    p.state=MAIN_LOOP_POLL_ERR;nested_dispatch=true;
    usb_host_poll_notify(NULL,&p);assert(pumps==2 && !usb_host_poll_dispatching);
    assert(bql_checks==150006);
}
""" + tests[end:]
    tests = tests.replace("VM-clock/deadline/ISO-poll", "host-poll deadline/pause/resume/reentry")

upstream_notice = source.split("*/", 1)[0] + "*/\n" if source.startswith("/*") else ""
if "qemu_host_wait_now" in source:
    # Telemetry is tested independently by qemu-wait-test.py. Stub its
    # observation-only API here so transport/poll behavior stays isolated.
    prefix += r"""
static int64_t qemu_host_wait_now(void) { return 0; }
static void qemu_host_wait_record(const char *tag,int64_t a,int64_t b,int64_t detail) {
    (void)tag;(void)a;(void)b;(void)detail;
}
"""
parts = [upstream_notice, prefix, function("usb_host_iso_reset_xfer"),
         function("usb_host_iso_data_copy")]
if "static void usb_host_iso_set_out_length(" in source:
    parts.append(function("usb_host_iso_set_out_length"))
else:
    # The original code has no aggregate-length update; retain that baseline.
    parts.append("static void usb_host_iso_set_out_length(USBHostIsoXfer *x) {(void)x;}")
if poll_mode:
    parts.extend([function("usb_host_poll_kick"), function("usb_host_poll_notify"),
                  function("usb_host_poll_vm_change"), function("usb_host_init")])
else:
    parts.append(function("usb_host_timer_kick"))
if "static int usb_host_iso_submit(" in source:
    if "static void usb_host_iso_packet_summary(" in source:
        parts.extend(["#define HAVE_ORPHAN_TRACE 1", function("usb_host_iso_packet_summary")])
    parts.extend(["#define HAVE_COUNTED_SUBMIT 1", function("usb_host_iso_submit"),
                  function("usb_host_req_complete_iso")])
parts.append(tests)
c_path = output_dir / "iso-test.c"
c_path.write_text("\n".join(parts), encoding="utf-8")
exe_path = output_dir / "iso-test.exe"
env = dict(os.environ)
env["PATH"] = str(compiler.parent) + os.pathsep + env.get("PATH", "")
subprocess.run([str(compiler), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-function", str(c_path), "-o", str(exe_path)],
               check=True, env=env)
result = subprocess.run([str(exe_path)], env=env)
sys.exit(result.returncode)

