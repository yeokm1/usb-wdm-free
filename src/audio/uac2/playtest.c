/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Direct actual-player host regression. All audio, time, report-file and
 * event operations below are mocks; no audio device or A: is accessed.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static UINT MDevices(void);
static MMRESULT MCaps(UINT,WAVEOUTCAPS *,UINT);
static MMRESULT MOpen(HWAVEOUT *,UINT,const WAVEFORMATEX *,DWORD,DWORD,DWORD);
static MMRESULT MPrepare(HWAVEOUT,WAVEHDR *,UINT);
static MMRESULT MWrite(HWAVEOUT,WAVEHDR *,UINT);
static MMRESULT MPosition(HWAVEOUT,MMTIME *,UINT);
static MMRESULT MReset(HWAVEOUT);
static MMRESULT MUnprepare(HWAVEOUT,WAVEHDR *,UINT);
static MMRESULT MClose(HWAVEOUT);
static DWORD MTick(void),MWait(HANDLE,DWORD);
static void MSleep(DWORD);
static BOOL MFrequency(LARGE_INTEGER *),MCounter(LARGE_INTEGER *);
static HANDLE MFile(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
static BOOL MWriteFile(HANDLE,LPCVOID,DWORD,LPDWORD,LPOVERLAPPED);
static BOOL MFlush(HANDLE),MHandleClose(HANDLE);
static HANDLE MEvent(LPSECURITY_ATTRIBUTES,BOOL,BOOL,LPCSTR);
static LPVOID MAlloc(HANDLE,DWORD,SIZE_T);
static BOOL MFree(HANDLE,DWORD,LPVOID);

#undef waveOutGetDevCaps
#undef CreateFile
#undef CreateEvent
#define waveOutGetNumDevs MDevices
#define waveOutGetDevCaps MCaps
#define waveOutOpen MOpen
#define waveOutPrepareHeader MPrepare
#define waveOutWrite MWrite
#define waveOutGetPosition MPosition
#define waveOutReset MReset
#define waveOutUnprepareHeader MUnprepare
#define waveOutClose MClose
#define GetTickCount MTick
#define QueryPerformanceFrequency MFrequency
#define QueryPerformanceCounter MCounter
#define WaitForSingleObject MWait
#define Sleep MSleep
#define CreateFile MFile
#define WriteFile MWriteFile
#define FlushFileBuffers MFlush
#define CloseHandle MHandleClose
#define CreateEvent MEvent
#define HeapAlloc MAlloc
#define HeapFree MFree
#define WinMain PlayerWinMain
#include "uacplay.c"

enum { NORMAL,LATE,WARPED,STALL,POSITION_BACK,QPC_BACK,QPC_START_FAIL,
       QPC_FINAL_FAIL,FREQ_FAIL,CLOSE_FAIL,HEADER_OWNED,SAVE_FAIL,SHORT_WRITE,
       EARLY,ALLOC_FAIL,PROGRESSIVE,QPC_MID_BACK,QPC_FINAL_BACK,
       WRITE_DELAY,PHASE_CLOCK_FAIL,WRITE_FAIL,WRITE_AND_PHASE_FAIL };
static int scenario,waits,writes,prepares,unprepares,resets,closes,frees;
static int eventCloses,saveCalls,pcmChecks,qpcCalls,failures,checks,writeAttempts;
static DWORD tick,pos;
static LONGLONG qpc;
static WAVEHDR *queued[120];
static void *allocations[2];
static int allocated;
static char saved[16384];
#define CHECK(x) do { ++checks; if(!(x)) {printf("FAIL line %d: %s\n",__LINE__,#x);++failures;} } while(0)
static UINT MDevices(void) {return 1;}
static MMRESULT MCaps(UINT id,WAVEOUTCAPS *c,UINT bytes)
{(void)id;(void)bytes;ZeroMemory(c,sizeof(*c));lstrcpy(c->szPname,"Mock UAC2");c->wChannels=2;return 0;}
static MMRESULT MOpen(HWAVEOUT *w,UINT id,const WAVEFORMATEX *f,DWORD cb,DWORD instance,DWORD flags)
{
    (void)id;(void)cb;(void)instance;
    CHECK(f->nChannels==2 && f->nSamplesPerSec==48000 && f->nBlockAlign==4);
    if(flags&WAVE_FORMAT_QUERY) {CHECK(w==NULL);return 0;}
    *w=(HWAVEOUT)3;return 0;
}
static MMRESULT MPrepare(HWAVEOUT w,WAVEHDR *h,UINT n)
{(void)w;CHECK(n==sizeof(*h));CHECK(h->dwBufferLength==48000);h->dwFlags|=WHDR_PREPARED;++prepares;return 0;}
static MMRESULT MWrite(HWAVEOUT w,WAVEHDR *h,UINT n)
{
    DWORD i;short *p=(short *)h->lpData;
    ++writeAttempts;
    if((scenario==WRITE_FAIL || scenario==WRITE_AND_PHASE_FAIL) && writeAttempts==2)
        return MMSYSERR_ERROR; /* failed API call never takes the header */
    (void)w;(void)n;CHECK(writes<BLOCKS);queued[writes++]=h;
    if(scenario==WRITE_DELAY && writes==1) {tick+=50;qpc+=3000;}
    for(i=0;i<FRAMES;i++) {CHECK(p[i*2]==p[i*2+1]);CHECK(p[i*2]>=-1024 && p[i*2]<=1024);}
    CHECK(p[0]!=p[100]);++pcmChecks;return 0;
}
static MMRESULT MPosition(HWAVEOUT w,MMTIME *t,UINT n)
{(void)w;(void)n;t->wType=TIME_SAMPLES;t->u.sample=pos;return 0;}
static MMRESULT MReset(HWAVEOUT w) {(void)w;++resets;return 0;}
static MMRESULT MUnprepare(HWAVEOUT w,WAVEHDR *h,UINT n)
{(void)w;(void)n;++unprepares;if(scenario==HEADER_OWNED)return WAVERR_STILLPLAYING;h->dwFlags&=~WHDR_PREPARED;return 0;}
static MMRESULT MClose(HWAVEOUT w) {(void)w;++closes;return scenario==CLOSE_FAIL?WAVERR_STILLPLAYING:0;}
static DWORD MTick(void) {return tick;}
static void MSleep(DWORD n) {tick+=n;qpc+=n;}
static DWORD MWait(HANDLE e,DWORD n)
{
    int i;(void)e;(void)n;++waits;CHECK(waits<10);
    if(scenario==PROGRESSIVE) {
        tick=TEST_MS*waits/4;qpc=1000+tick;pos=TOTAL_FRAMES*waits/4;
        for(i=0;i<writes*waits/4;i++) queued[i]->dwFlags|=WHDR_DONE;
        return WAIT_OBJECT_0;
    }
    if(scenario==QPC_MID_BACK && waits<=2) {
        tick=TEST_MS*waits/4;pos=TOTAL_FRAMES*waits/4;
        qpc=1000+TEST_MS/(waits==1?2:4);return WAIT_OBJECT_0;
    }
    if(scenario==QPC_FINAL_BACK && waits==1) {
        tick=TEST_MS/2;pos=TOTAL_FRAMES/2;qpc=1000+TEST_MS+100;
        return WAIT_OBJECT_0;
    }
    if(scenario==STALL) {qpc=1000+TEST_MS+8001;return WAIT_TIMEOUT;}
    if(scenario==POSITION_BACK && waits==1) {pos=100;tick=100;qpc=1100;return 0;}
    if(scenario==POSITION_BACK) {pos=50;tick=200;qpc=1200;return 0;}
    if(scenario==QPC_BACK) {qpc=999;tick=100;return 0;}
    tick=scenario==EARLY?TEST_MS-501:TEST_MS;
    qpc=1000+TEST_MS;
    if(scenario==WRITE_DELAY) {tick+=50;qpc+=3000;}
    if(scenario==LATE) tick=TEST_MS+8001,qpc=1000+tick;
    if(scenario==WARPED) qpc=1000+TEST_MS*10;
    pos=TOTAL_FRAMES;
    for(i=0;i<writes;i++) queued[i]->dwFlags|=WHDR_DONE;
    return WAIT_OBJECT_0;
}
static BOOL MFrequency(LARGE_INTEGER *v) {v->QuadPart=1000;return scenario!=FREQ_FAIL;}
static BOOL MCounter(LARGE_INTEGER *v)
{++qpcCalls;v->QuadPart=qpc;return !((scenario==QPC_START_FAIL && qpcCalls==1)||(scenario==QPC_FINAL_FAIL && waits)||
    (scenario==PHASE_CLOCK_FAIL && qpcCalls==3)||
    (scenario==WRITE_AND_PHASE_FAIL && qpcCalls==5));}
static const char *expectedPath=REPORT_PATH;
static HANDLE MFile(LPCSTR p,DWORD a,DWORD s,LPSECURITY_ATTRIBUTES x,DWORD c,DWORD f,HANDLE t)
{(void)a;(void)s;(void)x;(void)c;(void)f;(void)t;CHECK(!strcmp(p,expectedPath));++saveCalls;return scenario==SAVE_FAIL?INVALID_HANDLE_VALUE:(HANDLE)5;}
static BOOL MWriteFile(HANDLE h,LPCVOID p,DWORD n,LPDWORD done,LPOVERLAPPED o)
{(void)o;CHECK(h==(HANDLE)5);CHECK(n<sizeof(saved));memcpy(saved,p,n);saved[n]=0;*done=scenario==SHORT_WRITE?n-1:n;return TRUE;}
static BOOL MFlush(HANDLE h) {CHECK(h==(HANDLE)5);return TRUE;}
static BOOL MHandleClose(HANDLE h) {CHECK(h==(HANDLE)4 || h==(HANDLE)5);if(h==(HANDLE)4)++eventCloses;return TRUE;}
static HANDLE MEvent(LPSECURITY_ATTRIBUTES a,BOOL m,BOOL i,LPCSTR s)
{(void)a;(void)m;(void)i;(void)s;return (HANDLE)4;}
static LPVOID MAlloc(HANDLE h,DWORD flags,SIZE_T n)
{void *p;(void)h;(void)flags;if(scenario==ALLOC_FAIL)return NULL;CHECK(allocated<2);p=calloc(1,n);allocations[allocated++]=p;return p;}
static BOOL MFree(HANDLE h,DWORD f,LPVOID p)
{int i;(void)h;(void)f;for(i=0;i<allocated;i++)if(allocations[i]==p)allocations[i]=NULL;free(p);++frees;return TRUE;}
static void Run(int kind,int expected,const char *why)
{
    int result,i;
    scenario=kind;waits=writes=prepares=unprepares=resets=closes=frees=0;
    eventCloses=saveCalls=pcmChecks=qpcCalls=allocated=writeAttempts=0;tick=pos=0;qpc=1000;
    saved[0]=0;output=NULL;
    result=Play();
    if(result!=expected)printf("CASE %s expected %d got %d\n%s",why,expected,result,report);
    CHECK(result==expected);CHECK(saveCalls==1);
    if(expected==1 || expected==4) {CHECK(strstr(report,"FAIL:")!=NULL);CHECK(strstr(report,"PASS:")==NULL);}
    if(kind!=ALLOC_FAIL) {CHECK(prepares==BLOCKS);CHECK(unprepares==BLOCKS);CHECK(closes==1);}
    if(expected==0) {CHECK(writes==BLOCKS);CHECK(pcmChecks==BLOCKS);CHECK(strstr(saved,"PASS:")!=NULL);}
    if(kind==CLOSE_FAIL || kind==HEADER_OWNED) {CHECK(playResourcesRetained==1);CHECK(frees==0);CHECK(eventCloses==0);}
    else {CHECK(!playResourcesRetained);CHECK(frees==allocated);CHECK(eventCloses==1);}
    for(i=0;i<allocated;i++)if(allocations[i]) {free(allocations[i]);allocations[i]=NULL;}
    printf("case %-22s exit=%d\n",why,result);
}
int main(void)
{
    CHECK(!SelectCommand(NULL));CHECK(!autoExit);CHECK(!strcmp(reportPath,REPORT_PATH));
    CHECK(SelectCommand("/playexitlocal"));CHECK(autoExit);
#ifdef UACPLAY_SOAK
    CHECK(!strcmp(reportPath,"C:\\UACTEST\\SOAKLOG.TXT"));
#else
    CHECK(!strcmp(reportPath,"C:\\UACTEST\\PLAYLOG.TXT"));
#endif
    expectedPath=LOCAL_REPORT_PATH;Run(NORMAL,0,"local report no A access");
    CHECK(SelectCommand("/playlocal"));CHECK(!autoExit);
    CHECK(!SelectCommand("/playexit"));CHECK(autoExit);CHECK(!strcmp(reportPath,REPORT_PATH));
    expectedPath=REPORT_PATH;
    Run(NORMAL,0,"all buffers complete");
#ifdef UACPLAY_PHASES
    CHECK(strstr(report,"Phase=before_write index=0 tick_ms=0 qpc_ms=0")!=NULL);
    CHECK(strstr(report,"Phase=after_write index=7 tick_ms=0 qpc_ms=0")!=NULL);
    Run(WRITE_DELAY,1,"write setup included");
    CHECK(strstr(report,"Phase=before_write index=0 tick_ms=0 qpc_ms=0")!=NULL);
    CHECK(strstr(report,"Phase=after_write index=0 tick_ms=50 qpc_ms=3000")!=NULL);
    Run(PHASE_CLOCK_FAIL,1,"phase QPC failure");
    Run(WRITE_AND_PHASE_FAIL,1,"write and phase failure");
    CHECK(writes==1 && writeAttempts==2);
    CHECK(strstr(report,"Submitted=1 completed=0")!=NULL);CHECK(resets==1);
    CHECK(strstr(report,"FAIL: phase performance counter failed")!=NULL);
    CHECK(strstr(report,"Write 1 error=1")!=NULL);
#endif
    Run(WRITE_FAIL,1,"failed write not submitted");
    CHECK(writes==1 && writeAttempts==2);
    CHECK(strstr(report,"Submitted=1 completed=0")!=NULL);CHECK(resets==1);
    Run(PROGRESSIVE,0,"four completion batches");
    Run(POSITION_BACK,1,"position backstep");
    Run(EARLY,1,"premature completion");
    Run(CLOSE_FAIL,4,"close retains storage");
    Run(HEADER_OWNED,4,"prepared header retained");
    Run(SAVE_FAIL,3,"report create fails");
    Run(SHORT_WRITE,3,"report short write");
    Run(ALLOC_FAIL,1,"allocation failure");
#ifdef UACPLAY_WALLGATE
    Run(LATE,1,"late completion");Run(WARPED,1,"stretched wall time");
    Run(STALL,1,"QPC-only deadline");Run(QPC_BACK,1,"QPC below start");
    Run(QPC_MID_BACK,1,"QPC intermediate backstep");
    Run(QPC_FINAL_BACK,1,"QPC final backstep");
    Run(QPC_START_FAIL,1,"QPC startup failure");Run(QPC_FINAL_FAIL,1,"QPC final failure");
    Run(FREQ_FAIL,1,"frequency unavailable");
#else
    /* Explicitly preserve, rather than silently strengthen, legacy criteria. */
    Run(LATE,0,"legacy late diagnostic");Run(WARPED,0,"legacy wall diagnostic");
    Run(QPC_START_FAIL,0,"legacy QPC optional");
    Run(QPC_FINAL_FAIL,0,"legacy final optional");
#endif
    printf("%s: actual player %u buffers, %d checks, %d failures; mocked audio/files/clocks\n",failures?"FAIL":"PASS",(UINT)BLOCKS,checks,failures);
    return failures?1:0;
}
