#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Yeo Kheng Meng
"""Compile the ACTUAL bounded QEMU wait-recorder source with host API mocks.

No QEMU build/run, USB, timer policy or device access. Explicit QEMU source
input is a reviewed reference artifact, not a package build dependency.
The generated translation unit retains the upstream MIT notice verbatim.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess

EXPECTED = '56E7560D32E974E1277D4C7708ECE28DE0BAE7D749D25F4D1CECE3B7C366EF09'

STUBS = r'''
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#define CONFIG_WIN32 1
#define QEMU_CLOCK_REALTIME 0
static unsigned checks, io_calls, clock_calls, registration_calls;
static bool locked, dumping;
static int registration_failure;
static const char *environment;
static int64_t clock_value;
static void (*registered_exit)(void);
static char output[2*1024*1024];
static size_t used;
static void check(int value, unsigned line, const char *text)
{
    ++checks;
    if (!value) { fprintf(stderr,"FAIL line=%u %s\n",line,text); exit(1); }
}
#define CHECK(x) check(!!(x),__LINE__,#x)
static bool bql_locked(void) { return locked; }
typedef struct MockGuard { bool acquired; } MockGuard;
static MockGuard acquire_guard(void)
{
    MockGuard g = { !locked };
    locked=true;
    return g;
}
static void release_guard(MockGuard *g) { if(g->acquired)locked=false; }
#define BQL_LOCK_GUARD() MockGuard guard __attribute__((cleanup(release_guard))) = acquire_guard()
static int64_t qemu_clock_get_ns(int type)
{ CHECK(type==QEMU_CLOCK_REALTIME);++clock_calls;return clock_value; }
static char *fake_getenv(const char *name)
{ CHECK(!strcmp(name,"QEMU_HOST_WAIT_DIAG"));return (char *)environment; }
static int fake_atexit(void (*fn)(void))
{
    ++registration_calls;
    if(registration_failure)return 1;
    CHECK(!registered_exit);registered_exit=fn;return 0;
}
static int fake_fprintf(FILE *stream,const char *format,...)
{
    int n;va_list args;
    CHECK(dumping);CHECK(stream==stderr);++io_calls;
    va_start(args,format);n=vsnprintf(output+used,sizeof(output)-used,format,args);va_end(args);
    CHECK(n>=0 && (size_t)n<sizeof(output)-used);used+=(size_t)n;return n;
}
static int fake_fflush(FILE *stream)
{ CHECK(dumping && stream==stderr);++io_calls;return 0; }
#define getenv fake_getenv
#define atexit fake_atexit
#define fprintf fake_fprintf
#define fflush fake_fflush
'''

TESTS = r'''
#undef getenv
#undef atexit
#undef fprintf
#undef fflush
static void reset(const char *env)
{
    memset(host_wait_records,0,sizeof(host_wait_records));
    host_wait_count=0;host_wait_overflow=host_wait_backsteps=0;
    host_wait_enabled=-1;host_wait_frozen=false;
    environment=env;registration_failure=0;registered_exit=NULL;
    locked=dumping=false;io_calls=clock_calls=registration_calls=0;
    used=0;output[0]=0;clock_value=INT64_C(1234567890123);
}
static void dump(void)
{ CHECK(registered_exit!=NULL);dumping=true;registered_exit();dumping=false; }
int main(void)
{
    unsigned i;const char *off[]={NULL,"0","true","01"};
    for(i=0;i<sizeof(off)/sizeof(off[0]);++i) {
        reset(off[i]);CHECK(qemu_host_wait_now()==0);CHECK(qemu_host_wait_now()==0);
        CHECK(!clock_calls && !registration_calls && !registered_exit);
        qemu_host_wait_record("poll",1,INT64_C(900000000),1);
        CHECK(!host_wait_count && !io_calls);
    }
    reset("1");registration_failure=1;
    CHECK(qemu_host_wait_now()==0 && registration_calls==1 && !registered_exit && !clock_calls);
    CHECK(qemu_host_wait_now()==0 && registration_calls==1);
    reset("1");CHECK(qemu_host_wait_now()==clock_value);
    ++clock_value;CHECK(qemu_host_wait_now()==clock_value);
    CHECK(registration_calls==1 && clock_calls==2 && !io_calls);
    locked=true;
    qemu_host_wait_record("poll",1,INT64_C(50000000),1000);
    CHECK(!host_wait_count);
    qemu_host_wait_record("poll",1,INT64_C(50000001),1000);
    CHECK(host_wait_count==1 && host_wait_records[0].duration==50000000);
    CHECK(host_wait_records[0].requested==1000 && !io_calls);
    qemu_host_wait_record("poll",2,1,0);
    CHECK(host_wait_backsteps==1 && host_wait_count==1);
    qemu_host_wait_record("poll",0,INT64_C(500000000),0);
    CHECK(host_wait_count==1);
    locked=false;dump();CHECK(host_wait_frozen && !locked);
    CHECK(strstr(output,"HOST_WAIT_BEGIN count=1 overflow=0 backsteps=1")!=NULL);
    CHECK(strstr(output,"HOST_WAIT_END count=1")!=NULL);
    locked=true;qemu_host_wait_record("poll",1,INT64_C(50000001),0);
    CHECK(host_wait_count==1 && host_wait_overflow==0);locked=false;

    reset("1");qemu_host_wait_now();locked=true;
    for(i=0;i<HOST_WAIT_CAPACITY;++i)qemu_host_wait_record("poll",1,INT64_C(50000001),1000000);
    CHECK(host_wait_count==8192 && host_wait_overflow==0 && !io_calls);
    qemu_host_wait_record("poll",1,INT64_C(50000001),0);
    CHECK(host_wait_count==8192 && host_wait_overflow==1 && !io_calls);
    locked=false;dump();
    CHECK(strstr(output,"HOST_WAIT_BEGIN count=8192 overflow=1 backsteps=0")!=NULL);
    CHECK(strstr(output,"HOST_WAIT_END count=8192")!=NULL);
    CHECK(io_calls==8195 && !locked);

    reset("1");qemu_host_wait_now();locked=true;
    qemu_host_wait_record("poll",1,INT64_C(50000001),-1);
    qemu_host_wait_record("usb_set_config",1,INT64_C(50000001),1);
    qemu_host_wait_record("usb_set_interface",1,INT64_C(50000001),INT64_C(17179869185));
    qemu_host_wait_record("libusb_pump",1,INT64_C(50000001),0);
    CHECK(!io_calls);dump();CHECK(locked); /* conditional guard preserves caller's lock */
    CHECK(strstr(output,"aux=-1 aux_unit=ns")!=NULL);
    CHECK(strstr(output,"aux=1 aux_unit=configuration")!=NULL);
    CHECK(strstr(output,"aux=17179869185 aux_unit=interface_shift32_or_alt")!=NULL);
    CHECK(strstr(output,"aux=0 aux_unit=none")!=NULL);
    qemu_host_wait_record("poll",4,1,0);CHECK(!host_wait_backsteps);
    locked=false;
    printf("PASS: %u actual telemetry core checks; no QEMU/device/process-policy execution\n",checks);
    return 0;
}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, required=True)
    ap.add_argument('--compiler', type=Path, required=True)
    ap.add_argument('--scratch', type=Path, default=Path('E:/usb-wdm-free/wave2/qemu-wait-test'))
    ap.add_argument('--expect-sha256', default=EXPECTED)
    args = ap.parse_args()
    source, scratch = args.source.resolve(), args.scratch.resolve()
    if scratch.drive.upper() != 'E:':
        raise ValueError('scratch must resolve to E:')
    scratch.mkdir(parents=True, exist_ok=True)
    marker = scratch/'PASS.TXT'
    marker.unlink(missing_ok=True)
    raw = source.read_bytes()
    digest = hashlib.sha256(raw).hexdigest().upper()
    if digest != args.expect_sha256.upper():
        raise ValueError(f'reviewed source hash mismatch {digest}')
    text = raw.decode('utf-8').replace('\r\n', '\n')
    start = text.index('#define HOST_WAIT_CAPACITY ')
    end = text.index('\n#ifndef _WIN32', start)
    notice = text[:text.index('#include')]
    unit = scratch/'waitcore.c'
    unit.write_text(notice+STUBS+'\n/* BEGIN actual reviewed source */\n'+text[start:end]+
                    '\n/* END actual reviewed source */\n'+TESTS, encoding='utf-8')
    env = os.environ.copy()
    env['PATH'] = str(args.compiler.resolve().parent)+os.pathsep+env.get('PATH','')
    env['TMP'] = env['TEMP'] = str(scratch)
    executable = scratch/'WAITCORE.EXE'
    with (scratch/'build.log').open('w') as log:
        subprocess.run([str(args.compiler), '-O2', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=undefined', '-fsanitize-undefined-trap-on-error',
                        str(unit), '-o', str(executable)], env=env, stdout=log,
                       stderr=subprocess.STDOUT, check=True)
    with (scratch/'test.log').open('w') as log:
        subprocess.run([str(executable)], env=env, stdout=log, stderr=subprocess.STDOUT,
                       timeout=30, check=True)
    output = (scratch/'test.log').read_text()
    if not output.startswith('PASS: '):
        raise RuntimeError('test completion marker missing')
    if hashlib.sha256(source.read_bytes()).hexdigest().upper() != digest:
        raise RuntimeError('source changed during test')
    manifest = dict(source=str(source),source_sha256=digest,
                    extracted_sha256=hashlib.sha256(unit.read_bytes()).hexdigest(),
                    executable_sha256=hashlib.sha256(executable.read_bytes()).hexdigest(),
                    compiler=str(args.compiler.resolve()),result=output.strip(),
                    limits='Actual recorder core only; no actual QEMU BQL, OS scheduler, USB or atexit lifecycle proof.')
    (scratch/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    marker.write_text(output)
    print(output, end='')


if __name__ == '__main__':
    main()
