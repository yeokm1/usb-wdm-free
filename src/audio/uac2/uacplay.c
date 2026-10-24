/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original WinMM test: real queued PCM, completion events and positions.
 * The GUI never labels enumeration or elapsed time as successful playback.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <string.h>

#ifdef UACPLAY_PHASES
#ifndef UACPLAY_WALLGATE
#define UACPLAY_WALLGATE
#endif
#endif

#ifdef UACPLAY_SOAK
#ifndef UACPLAY_WALLGATE
#define UACPLAY_WALLGATE
#endif
#define BLOCKS 120
#define REPORT_PATH "A:\\SOAKLOG.TXT"
#define LOCAL_REPORT_PATH "C:\\UACTEST\\SOAKLOG.TXT"
#define PLAY_BUTTON "Play 30 s quiet tone"
#else
#define BLOCKS 8
#define REPORT_PATH "A:\\PLAYLOG.TXT"
#define LOCAL_REPORT_PATH "C:\\UACTEST\\PLAYLOG.TXT"
#define PLAY_BUTTON "Play 2 s quiet tone"
#endif
#define FRAMES 12000
#define TOTAL_FRAMES (BLOCKS*FRAMES)
#define TEST_MS (TOTAL_FRAMES/48)
static char report[16384];
static HWND output;
static int autoExit,processResult=2,playResourcesRetained;
static const char *reportPath=REPORT_PATH;
static int SelectCommand(const char *command)
{
    int local=command && (!lstrcmp(command,"/playexitlocal") || !lstrcmp(command,"/playlocal"));
    autoExit=command && (!lstrcmp(command,"/playexit") || !lstrcmp(command,"/playexitlocal"));
    reportPath=local?LOCAL_REPORT_PATH:REPORT_PATH;
    return local;
}

static void Append(const char *text)
{
    if(lstrlen(report)+lstrlen(text)<(int)sizeof(report)) lstrcat(report,text);
}
#ifdef UACPLAY_PHASES
/* Memory-only observations. Keep startup inside the existing strict gate;
 * expose it separately rather than treating setup as completed playback. */
static int PhaseStamp(const char *phase, UINT index, DWORD tickStart,
    const LARGE_INTEGER *perfStart, const LARGE_INTEGER *frequency,
    LARGE_INTEGER *previous)
{
    LARGE_INTEGER now;
    double ms;
    char text[192];
    if(!QueryPerformanceCounter(&now) || now.QuadPart<previous->QuadPart) {
        Append("FAIL: phase performance counter failed or moved backward.\r\n");
        return 0;
    }
    *previous=now;
    ms=(double)(now.QuadPart-perfStart->QuadPart)*1000.0/(double)frequency->QuadPart;
    if(ms<0 || ms>=4294967295.0) {
        Append("FAIL: phase elapsed time out of range.\r\n"); return 0;
    }
    wsprintf(text,"Phase=%s index=%u tick_ms=%lu qpc_ms=%lu\r\n",
        phase,index,GetTickCount()-tickStart,(DWORD)ms);
    Append(text); return 1;
}
#endif
static BOOL Save(void)
{
    DWORD n=0,length=(DWORD)lstrlen(report);
    BOOL ok=FALSE;
    char errorText[192];
    HANDLE file=CreateFile(reportPath,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);
    if(file!=INVALID_HANDLE_VALUE) {
        ok=WriteFile(file,report,length,&n,NULL) && n==length;
        if(ok) ok=FlushFileBuffers(file);
        if(!CloseHandle(file)) ok=FALSE;
    }
    if(!ok) {
        wsprintf(errorText,"FAIL: test report could not be persisted to %s.\r\n",reportPath);
        Append(errorText);
    }
    if(output) SetWindowText(output,report);
    return ok;
}
static UINT Devices(void)
{
    UINT count=waveOutGetNumDevs(),i,chosen=(UINT)-1;
    char text[256],name[MAXPNAMELEN];
    WAVEOUTCAPS caps;
    wsprintf(text,"waveOut devices=%u\r\n",count); Append(text);
    for(i=0;i<count;i++) {
        MMRESULT result=waveOutGetDevCaps(i,&caps,sizeof(caps));
        if(result) { wsprintf(text,"Device %u caps error=%u\r\n",i,result); Append(text); continue; }
        wsprintf(text,"Device %u: %s channels=%u\r\n",i,caps.szPname,caps.wChannels); Append(text);
        lstrcpyn(name,caps.szPname,sizeof(name)); CharUpperBuff(name,lstrlen(name));
        if(strstr(name,"UAC2") && chosen==(UINT)-1) chosen=i;
    }
    return chosen;
}
static DWORD Position(HWAVEOUT wave)
{
    MMTIME time;
    MMRESULT status;
    char text[160];
    ZeroMemory(&time,sizeof(time)); time.wType=TIME_SAMPLES;
    status=waveOutGetPosition(wave,&time,sizeof(time));
    if(status) { wsprintf(text,"Position error=%u\r\n",status); Append(text); return 0; }
    if(time.wType==TIME_SAMPLES) return time.u.sample;
    if(time.wType==TIME_BYTES) return time.u.cb/4;
    if(time.wType==TIME_MS) return time.u.ms*48;
    wsprintf(text,"Position unsupported type=%u\r\n",time.wType); Append(text); return 0;
}
static int Play(void)
{
    UINT id,i,j,attempt,prepared=0,submitted=0,complete=0;
    MMRESULT status;
    WAVEFORMATEX format;
    WAVEHDR *headers=NULL;
    short *data=NULL;
    HANDLE event=NULL;
    HWAVEOUT wave=NULL;
    DWORD phase=0,start,position=0,previousPosition=0,elapsed=0;
    LARGE_INTEGER perfStart,perfEnd,perfFrequency;
    BOOL perfValid;
#ifdef UACPLAY_WALLGATE
    double wallMs=0;
    LARGE_INTEGER perfPrevious;
#endif
    int failed=0,retained=0;
    char text[256];
    report[0]=0;
    wsprintf(text,"Real PCM test: %u seconds, stereo16/48000, quiet440Hztriangle.\r\n",
        (UINT)(TEST_MS/1000));Append(text);
    Append("Requires UAC2-labelled device; hardware USB results are separate evidence.\r\n");
    id=Devices();
    playResourcesRetained=0;
    if(id==(UINT)-1) { Append("NO TEST: no UAC2-labelled endpoint.\r\n"); return Save() ? 2:3; }
    ZeroMemory(&format,sizeof(format));
    format.wFormatTag=WAVE_FORMAT_PCM; format.nChannels=2;
    format.nSamplesPerSec=48000; format.wBitsPerSample=16;
    format.nBlockAlign=4; format.nAvgBytesPerSec=192000;
    status=waveOutOpen(NULL,id,&format,0,0,WAVE_FORMAT_QUERY);
    wsprintf(text,"Format query=%u\r\n",status); Append(text);
    if(status) { Append("FAIL: format query.\r\n"); return Save() ? 1:3; }
    data=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,BLOCKS*FRAMES*4);
    headers=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,BLOCKS*sizeof(*headers));
    event=CreateEvent(NULL,FALSE,FALSE,NULL);
    if(!data || !headers || !event) { Append("FAIL: allocation.\r\n"); failed=1; goto cleanup; }
    for(i=0;i<BLOCKS*FRAMES;i++) {
        DWORD triangle=phase<24000 ? phase : 48000-phase;
        short value=(short)((int)(triangle*2048/24000)-1024);
        data[i*2]=data[i*2+1]=value;
        phase+=440; if(phase>=48000) phase-=48000;
    }
    /* Endpoint format probing may release a physical pin asynchronously.
     * Retry only the explicit allocated/busy result for a bounded interval;
     * all other failures remain immediate and every attempt is recorded. */
    for(attempt=0;attempt<20;attempt++) {
        wave=NULL;
        status=waveOutOpen(&wave,id,&format,(DWORD)event,0,CALLBACK_EVENT);
        wsprintf(text,"Open attempt=%u result=%u\r\n",attempt+1,status); Append(text);
        if(status!=MMSYSERR_ALLOCATED) break;
        if(attempt+1<20) Sleep(100);
    }
    wsprintf(text,"Open=%u\r\n",status); Append(text);
    if(status) { wave=NULL; failed=1; goto cleanup; }
    for(i=0;i<BLOCKS;i++) {
        headers[i].lpData=(LPSTR)(data+i*FRAMES*2);
        headers[i].dwBufferLength=FRAMES*4;
        status=waveOutPrepareHeader(wave,&headers[i],sizeof(headers[i]));
        if(status) { wsprintf(text,"Prepare %u error=%u\r\n",i,status); Append(text); failed=1; break; }
        prepared++;
    }
    perfValid=QueryPerformanceFrequency(&perfFrequency) && perfFrequency.QuadPart>0 &&
        QueryPerformanceCounter(&perfStart);
#ifdef UACPLAY_WALLGATE
    if(!perfValid) {Append("FAIL: required independent performance counter unavailable.\r\n");failed=1;}
    else perfPrevious=perfStart;
#endif
    start=GetTickCount();
    if(!failed) for(i=0;i<BLOCKS;i++) {
#ifdef UACPLAY_PHASES
        if(!PhaseStamp("before_write",i,start,&perfStart,&perfFrequency,&perfPrevious)) {
            failed=1; break;
        }
#endif
        status=waveOutWrite(wave,&headers[i],sizeof(headers[i]));
#ifdef UACPLAY_PHASES
        if(!PhaseStamp("after_write",i,start,&perfStart,&perfFrequency,&perfPrevious)) failed=1;
#endif
        if(status) { wsprintf(text,"Write %u error=%u\r\n",i,status); Append(text); failed=1; break; }
        submitted++;
#ifdef UACPLAY_PHASES
        if(failed) break;
#endif
    }
    while(!failed && complete<submitted) {
        complete=0;
        for(j=0;j<submitted;j++) if(headers[j].dwFlags&WHDR_DONE) complete++;
        position=Position(wave);
        if(position<previousPosition) {
            Append("FAIL: playback position moved backward.\r\n"); failed=1; break;
        }
        previousPosition=position;
        if(complete==submitted) break;
        if(GetTickCount()-start>TEST_MS+8000) { Append("FAIL: completion deadline.\r\n"); failed=1; break; }
#ifdef UACPLAY_WALLGATE
        if(!QueryPerformanceCounter(&perfEnd) || perfEnd.QuadPart<perfPrevious.QuadPart) {
            Append("FAIL: required performance counter failed or moved backward.\r\n");failed=1;break;
        }
        wallMs=(double)(perfEnd.QuadPart-perfStart.QuadPart)*1000.0/(double)perfFrequency.QuadPart;
        perfPrevious=perfEnd;
        if(wallMs>TEST_MS+8000) {Append("FAIL: independent wall-clock completion deadline.\r\n");failed=1;break;}
#endif
        WaitForSingleObject(event,100);
    }
    elapsed=GetTickCount()-start;
    wsprintf(text,"Submitted=%u completed=%u samples=%lu elapsed_ms=%lu\r\n",
        submitted,complete,position,elapsed); Append(text);
    if(perfValid && QueryPerformanceCounter(&perfEnd)) {
        double perfMs=(double)(perfEnd.QuadPart-perfStart.QuadPart)*1000.0/(double)perfFrequency.QuadPart;
#ifdef UACPLAY_WALLGATE
        if(perfEnd.QuadPart<perfPrevious.QuadPart) {
            Append("FAIL: independent counter moved backward between observations.\r\n");failed=1;
        }
#endif
        if(perfMs>=0 && perfMs<4294967295.0) {
#ifdef UACPLAY_WALLGATE
            wsprintf(text,"Performance elapsed_ms=%lu (required wall-clock check)\r\n",(DWORD)perfMs);Append(text);
            if(perfMs<TEST_MS-500 || perfMs>TEST_MS+500) {
                Append("FAIL: independent elapsed time outside expected duration +/-500ms.\r\n");failed=1;
            }
#else
            wsprintf(text,"Performance elapsed_ms=%lu (diagnostic only)\r\n",(DWORD)perfMs); Append(text);
#endif
        }
#ifdef UACPLAY_WALLGATE
        else {Append("FAIL: invalid independent elapsed time.\r\n");failed=1;}
#endif
    }
#ifdef UACPLAY_WALLGATE
    else {Append("FAIL: required final performance-counter observation failed.\r\n");failed=1;}
#endif
    /* Allow100ms of pipeline/cursor rounding around the client frame target.
     * A positive cursor alone must not turn premature completions intoPASS. */
    if(position<TOTAL_FRAMES-4800 || position>TOTAL_FRAMES+4800 || elapsed<TEST_MS-500 ||
        submitted!=BLOCKS || complete!=BLOCKS) {
        wsprintf(text,"FAIL unless all%ubuffers complete near%lusamples after real duration.\r\n",
            (UINT)BLOCKS,(ULONG)TOTAL_FRAMES);Append(text);
        failed=1;
    }
cleanup:
    if(wave) {
        if(failed) { status=waveOutReset(wave); wsprintf(text,"Reset=%u\r\n",status); Append(text); }
        for(i=0;i<prepared;i++) {
            status=waveOutUnprepareHeader(wave,&headers[i],sizeof(headers[i]));
            if(status) { wsprintf(text,"Unprepare %u error=%u\r\n",i,status); Append(text); failed=1; }
        }
        status=waveOutClose(wave);
        wsprintf(text,"Close=%u\r\n",status); Append(text);
        if(status) { failed=1; retained=1; }
        /* If a broken driver still owns a header after Reset, keep its
         * backing storage until process exit rather than freeing it here. */
        for(i=0;i<prepared;i++) if(headers[i].dwFlags&WHDR_PREPARED) retained=1;
    }
    if(retained) {
        playResourcesRetained=1;
        Append("Resources retained after failed release; further playback disabled.\r\n");
        EnableWindow(GetDlgItem(GetParent(output),1),FALSE);
    } else {
        if(event) CloseHandle(event);
        if(data) HeapFree(GetProcessHeap(),0,data);
        if(headers) HeapFree(GetProcessHeap(),0,headers);
    }
    Append(failed ? "FAIL: WinMM queued playback test.\r\n" : "PASS: WinMM real-buffer completion/position/close.\r\n");
    if(!Save()) return 3;
    return retained ? 4 : failed ? 1:0;
}
static LRESULT CALLBACK Proc(HWND window,UINT message,WPARAM w,LPARAM l)
{
    if(message==WM_COMMAND && LOWORD(w)==1) {
        processResult=Play();
        if(autoExit && !playResourcesRetained) DestroyWindow(window);
        return 0;
    }
    if(message==WM_COMMAND && LOWORD(w)==2) { report[0]=0; Devices(); Save(); return 0; }
    if(message==WM_SIZE && output) MoveWindow(output,0,34,LOWORD(l),HIWORD(l)-34,TRUE);
    if(message==WM_DESTROY) { PostQuitMessage(processResult); return 0; }
    return DefWindowProc(window,message,w,l);
}
int WINAPI WinMain(HINSTANCE instance,HINSTANCE previous,LPSTR command,int show)
{
    WNDCLASS wc;
    HWND window,run;
    MSG message;
    char title[192];
    (void)previous;
    if(SelectCommand(command) && !CreateDirectory("C:\\UACTEST",NULL) &&
       GetLastError()!=ERROR_ALREADY_EXISTS) return 3;
    ZeroMemory(&wc,sizeof(wc)); wc.hInstance=instance; wc.lpfnWndProc=Proc;
    wc.lpszClassName="UacPlay"; wc.hCursor=LoadCursor(NULL,IDC_ARROW);
    if(!RegisterClass(&wc)) return 1;
    Devices();
    wsprintf(title,"UAC2 actual PCM playback test - %s",reportPath);
    window=CreateWindow("UacPlay",title,WS_OVERLAPPEDWINDOW,
        10,10,780,530,NULL,NULL,instance,NULL);
    run=CreateWindow("BUTTON",PLAY_BUTTON,WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,
        0,0,260,30,window,(HMENU)1,instance,NULL);
    CreateWindow("BUTTON","Refresh devices",WS_CHILD|WS_VISIBLE|WS_TABSTOP,265,0,220,30,
        window,(HMENU)2,instance,NULL);
    output=CreateWindow("EDIT",report,WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_READONLY,
        0,34,760,456,window,NULL,instance,NULL);
    SendMessage(output,WM_SETFONT,(WPARAM)GetStockObject(ANSI_FIXED_FONT),TRUE);
    ShowWindow(window,show); UpdateWindow(window); SetFocus(run); Save();
    /* Explicit test automation only; default launch remains manual.
     * Queue after controls exist so Play sees the same initialized GUI. */
    if(autoExit || (command && (!lstrcmp(command,"/play") || !lstrcmp(command,"/playlocal"))))
        PostMessage(window,WM_COMMAND,1,0);
    while(GetMessage(&message,NULL,0,0)>0) {
        if(!IsDialogMessage(window,&message)) { TranslateMessage(&message); DispatchMessage(&message); }
    }
    return (int)message.wParam;
}
