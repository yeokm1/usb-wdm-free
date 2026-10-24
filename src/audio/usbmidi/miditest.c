/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original WinMM loopback verifier with an explicit simulator install mode.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <setupapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static CRITICAL_SECTION lock;
static unsigned char received[4096];
static unsigned int count,errors,shortCount,longCount;
static int counting;   /* Stress: count bytes only */
static void Append(const unsigned char *p,unsigned int n)
{
    if(counting) {count+=n;return;}
    if(n>sizeof(received)-count) { errors++;return; }
    memcpy(received+count,p,n);count+=n;
}
static void CALLBACK Input(HMIDIIN in,UINT message,DWORD context,DWORD a,DWORD b)
{
    unsigned char bytes[3]; unsigned int n;
    (void)in;(void)context;(void)b;
    EnterCriticalSection(&lock);
    if(message==MIM_DATA) {
        bytes[0]=(unsigned char)a; bytes[1]=(unsigned char)(a>>8);bytes[2]=(unsigned char)(a>>16);
        if(bytes[0]<0x80) errors++;
        else {
            if(bytes[0]<0xf0) n=(bytes[0]<0xc0 || bytes[0]>=0xe0)?3:2;
            else n=(bytes[0]==0xf1 || bytes[0]==0xf3)?2:(bytes[0]==0xf2)?3:1;
            Append(bytes,n);shortCount++;
        }
    } else if(message==MIM_LONGDATA) {
        MIDIHDR *h=(MIDIHDR*)a;
        if(!h || h->dwBytesRecorded>h->dwBufferLength ||
            (h->dwBytesRecorded && !h->lpData)) errors++;
        else if(h->dwBytesRecorded) {
            Append((const unsigned char*)h->lpData,h->dwBytesRecorded);longCount++;
        }
    } else if(message==MIM_ERROR || message==MIM_LONGERROR) errors++;
    LeaveCriticalSection(&lock);
}
static void Ports(void)
{
    UINT i; MIDIINCAPS input; MIDIOUTCAPS output;
    for(i=0;i<midiInGetNumDevs();i++) if(!midiInGetDevCaps(i,&input,sizeof(input)))
        printf("IN %u: %s\n",i,input.szPname);
    for(i=0;i<midiOutGetNumDevs();i++) if(!midiOutGetDevCaps(i,&output,sizeof(output)))
        printf("OUT %u: %s\n",i,output.szPname);
    printf("WinMM: %u in, %u out\n",midiInGetNumDevs(),midiOutGetNumDevs());
}
/* KS audio/render/capture interfaces with their FriendlyName and CLSID, and
 * the WinMM driver registrations, to see why a filter is (not) in WinMM. */
static void Interfaces(void)
{
    static const GUID cats[3]={
        {0x6994AD04,0x93EF,0x11D0,{0xA3,0xCC,0x00,0xA0,0xC9,0x22,0x31,0x96}},
        {0x65E8773E,0x8F56,0x11D0,{0xA3,0xB9,0x00,0xA0,0xC9,0x22,0x31,0x96}},
        {0x65E8773D,0x8F56,0x11D0,{0xA3,0xB9,0x00,0xA0,0xC9,0x22,0x31,0x96}}};
    static const char *catName[3]={"AUDIO","RENDER","CAPTURE"};
    unsigned int c; DWORD i,type,size; HKEY key; char value[512];
    for(c=0;c<3;c++) {
        HDEVINFO set=SetupDiGetClassDevs((GUID*)&cats[c],NULL,NULL,DIGCF_DEVICEINTERFACE|DIGCF_PRESENT);
        SP_DEVICE_INTERFACE_DATA data;
        if(set==INVALID_HANDLE_VALUE) {printf("%s: SetupAPI error=%lu\n",catName[c],GetLastError());continue;}
        for(i=0;;i++) {
            static char buffer[1024];
            SP_DEVICE_INTERFACE_DETAIL_DATA_A *detail=(SP_DEVICE_INTERFACE_DETAIL_DATA_A*)buffer;
            ZeroMemory(&data,sizeof(data));data.cbSize=sizeof(data);
            if(!SetupDiEnumDeviceInterfaces(set,NULL,(GUID*)&cats[c],i,&data)) break;
            detail->cbSize=sizeof(*detail);
            if(!SetupDiGetDeviceInterfaceDetailA(set,&data,detail,sizeof(buffer),NULL,NULL)) continue;
            printf("%s %lu: %s flags=%lX\n",catName[c],i,detail->DevicePath,data.Flags);
            key=SetupDiOpenDeviceInterfaceRegKey(set,&data,0,KEY_QUERY_VALUE);
            if(key!=INVALID_HANDLE_VALUE) {
                size=sizeof(value)-1;ZeroMemory(value,sizeof(value));
                if(RegQueryValueEx(key,"FriendlyName",NULL,&type,(PBYTE)value,&size)==ERROR_SUCCESS) printf("  FriendlyName=%s\n",value);
                size=sizeof(value)-1;ZeroMemory(value,sizeof(value));
                if(RegQueryValueEx(key,"CLSID",NULL,&type,(PBYTE)value,&size)==ERROR_SUCCESS) printf("  CLSID=%s\n",value);
                RegCloseKey(key);
            }
        }
        SetupDiDestroyDeviceInfoList(set);
    }
    if(RegOpenKeyEx(HKEY_LOCAL_MACHINE,"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32",0,KEY_QUERY_VALUE,&key)==ERROR_SUCCESS) {
        char name[128];DWORD n;
        for(i=0;;i++) {
            n=sizeof(name);size=sizeof(value)-1;ZeroMemory(value,sizeof(value));
            if(RegEnumValue(key,i,name,&n,NULL,&type,(PBYTE)value,&size)!=ERROR_SUCCESS) break;
            if(!_strnicmp(name,"midi",4) || !_strnicmp(name,"wave",4)) printf("Drivers32 %s=%s\n",name,value);
        }
        RegCloseKey(key);
    }
}
static void Diagnostic(void)
{
    typedef DWORD (WINAPI *CMSTATUS)(PULONG,PULONG,DWORD,ULONG);
    HMODULE cm=LoadLibrary("cfgmgr32.dll");
    CMSTATUS status=cm?(CMSTATUS)GetProcAddress(cm,"CM_Get_DevNode_Status"):NULL;
    HDEVINFO set=SetupDiGetClassDevs(NULL,NULL,NULL,DIGCF_ALLCLASSES|DIGCF_PRESENT);
    SP_DEVINFO_DATA info;DWORD index,type,size;char ids[2048],value[256];HKEY key;
    if(set==INVALID_HANDLE_VALUE) {printf("SetupAPI error=%lu\n",GetLastError());if(cm) FreeLibrary(cm);return;}
    for(index=0;;index++) {
        ZeroMemory(&info,sizeof(info));info.cbSize=sizeof(info);
        if(!SetupDiEnumDeviceInfo(set,index,&info)) break;
        ZeroMemory(ids,sizeof(ids));
        if(!SetupDiGetDeviceRegistryProperty(set,&info,SPDRP_HARDWAREID,&type,(PBYTE)ids,sizeof(ids)-2,&size)) continue;
        if(!strstr(ids,"VID_FFFF") && !strstr(ids,"Vid_ffff") &&
           _strnicmp(ids,"USB\\VID_0582&PID_0168",21) &&
           _strnicmp(ids,"USB\\VID_0582&PID_012A",21)) continue;
        {const char *id=ids;while(*id) {printf("Hardware: %s\n",id);id+=strlen(id)+1;}}
        if(status) {ULONG flags=0,problem=0;DWORD cr=status(&flags,&problem,info.DevInst,0);
            printf("  CM return=%08lX flags=%08lX problem=%08lX (%lu)\n",cr,flags,problem,problem);}
        else printf("  CM API unavailable\n");
        {
            DWORD property[3]={SPDRP_DEVICEDESC,SPDRP_SERVICE,SPDRP_DRIVER};unsigned int i;
            for(i=0;i<3;i++) {
                ZeroMemory(value,sizeof(value));
                if(SetupDiGetDeviceRegistryProperty(set,&info,property[i],&type,(PBYTE)value,sizeof(value)-1,&size))
                    printf("  property%lu=%s\n",property[i],value);
            }
        }
        key=SetupDiOpenDevRegKey(set,&info,DICS_FLAG_GLOBAL,0,DIREG_DRV,KEY_QUERY_VALUE);
        if(key!=INVALID_HANDLE_VALUE) {
            static const char *names[]={"MidiStage","MidiStatus","MidiDevice","MidiPorts","MidiFilters","MidiInterface","MidiMaxPacket","TxBytes","RxBytes","TxUrbs","RxUrbs","BadEvents","Overruns","Timeouts","UsbLastStatus","Stalls","UsbErrors"};
            unsigned int i;
            for(i=0;i<sizeof(names)/sizeof(names[0]);i++) {
                DWORD data=0,bytes=sizeof(data);
                if(RegQueryValueEx(key,names[i],NULL,&type,(PBYTE)&data,&bytes)==ERROR_SUCCESS && type==REG_DWORD && bytes==4)
                    printf("  %s=%08lX (%lu)\n",names[i],data,data);
            }
            RegCloseKey(key);
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    if(cm) FreeLibrary(cm);
    if(RegOpenKeyEx(HKEY_LOCAL_MACHINE,"SYSTEM\\CurrentControlSet\\Services\\usbmidi",0,KEY_QUERY_VALUE,&key)==ERROR_SUCCESS) {
        static const char *names[]={"MidiDiagBuild","MidiAddCount","MidiRemoveCount","MidiDestroyCount","MidiElevatedCount","MidiUnloadPresent"};
        unsigned int i;
        for(i=0;i<sizeof(names)/sizeof(names[0]);i++) {DWORD data=0,bytes=4;
            if(RegQueryValueEx(key,names[i],NULL,&type,(PBYTE)&data,&bytes)==ERROR_SUCCESS && type==REG_DWORD && bytes==4)
                printf("Lifecycle %s=%08lX (%lu)\n",names[i],data,data);}
        RegCloseKey(key);
    }
}
static int Install(const char *id,const char *inf)
{
    /* Windows 2000 DDK newdev.h ABI: install INF for exactly this hardware
     * ID. Runtime lookup keeps list/loopback/diagnostics free of Newdev. */
    typedef BOOL (WINAPI *UPDATE)(HWND,LPCSTR,LPCSTR,DWORD,PBOOL);
    HMODULE module=LoadLibrary("newdev.dll");UPDATE update;BOOL reboot=FALSE,ok;
    DWORD error;
    if(!module) {printf("Newdev load error=%lu\n",GetLastError());return 1;}
    update=(UPDATE)GetProcAddress(module,"UpdateDriverForPlugAndPlayDevicesA");
    if(!update) {printf("Newdev API unavailable\n");FreeLibrary(module);return 1;}
    printf("Install %s from %s\n",id,inf);
    ok=update(NULL,id,inf,1,&reboot);
    error=ok?0:GetLastError();
    printf("Install success=%u error=%lu reboot=%u\n",ok,error,reboot);
    FreeLibrary(module);return ok?0:1;
}
static int PortNumber(const char *s,UINT *out)
{
    unsigned long n=0;
    if(!s || !*s) return 0;
    while(*s) { if(*s<'0' || *s>'9' || n>6553) return 0;n=n*10+(*s++-'0'); }
    if(n>65535) return 0;*out=(UINT)n;return 1;
}
/* Control for the 98 SE replug check: list WinMM wave outputs and open and
 * close each at 44.1 kHz 16-bit stereo PCM, so a WDM audio device that is not
 * ours (Windows' own usbaudio) can be put through the same unplug/replug. */
static int WaveOpen(void)
{
    UINT i,n=waveOutGetNumDevs(),bad=0; WAVEOUTCAPS caps; WAVEFORMATEX f; HWAVEOUT h; MMRESULT a,b;
    ZeroMemory(&f,sizeof(f));f.wFormatTag=WAVE_FORMAT_PCM;f.nChannels=2;f.nSamplesPerSec=44100;
    f.wBitsPerSample=16;f.nBlockAlign=4;f.nAvgBytesPerSec=44100*4;
    for(i=0;i<n;i++) {
        if(waveOutGetDevCaps(i,&caps,sizeof(caps))) caps.szPname[0]=0;
        h=NULL;a=waveOutOpen(&h,i,&f,0,0,CALLBACK_NULL);b=h?waveOutClose(h):0;
        printf("WAVE OUT %u: %s open=%u close=%u\n",i,caps.szPname,a,b);
        if(a || b) bad++;
    }
    printf("WinMM: %u wave out; %s\n",n,bad?"FAIL":"PASS");
    return bad?1:0;
}
static int OpenOnly(UINT inId,UINT outId,const char *log)
{
    HMIDIIN in=NULL;HMIDIOUT out=NULL;MMRESULT a,b=0,c=0,d=0;
    if(log && !freopen(log,"w",stdout)) return 2;
    setvbuf(stdout,NULL,_IONBF,0);
    /* Both are tried, so a failing input still shows whether output opens. */
    a=midiInOpen(&in,inId,0,0,CALLBACK_NULL);
    b=midiOutOpen(&out,outId,0,0,CALLBACK_NULL);
    if(out) c=midiOutClose(out);
    if(in) d=midiInClose(in);
    printf("Open-only in=%u out=%u: input=%u output=%u closeOut=%u closeIn=%u\n",inId,outId,a,b,c,d);
    printf("%s; no MIDI events sent\n",(a || b || c || d)?"FAIL":"PASS");
    if(fflush(stdout)==EOF || ferror(stdout)) return 1;
    return (a || b || c || d)?1:0;
}
/* Sustained traffic for a set time, for unplug-with-open-ports and soak
 * runs: note pairs every millisecond tick plus a 1 KiB SysEx each half
 * second. Prints a line per second (the log survives an unplug), then
 * closes everything and reports first errors and cleanup results. */
static int Stress(UINT inId,UINT outId,unsigned int seconds,const char *log)
{
    static MIDIHDR ih[8],oh; static unsigned char ib[8][4096],ob[1024];
    HMIDIIN in=NULL;HMIDIOUT out=NULL;MMRESULT a,b,firstOut=0,firstIn=0,r;
    unsigned int i,sent=0,tick=0,secs=0,sysexes=0,cleanup=1;DWORD start,last;
    if(log && !freopen(log,"w",stdout)) return 2;
    setvbuf(stdout,NULL,_IONBF,0);
    InitializeCriticalSection(&lock);counting=1;
    a=midiInOpen(&in,inId,(DWORD)Input,0,CALLBACK_FUNCTION);
    b=a?0:midiOutOpen(&out,outId,0,0,CALLBACK_NULL);
    printf("Stress in=%u out=%u %us: open in=%u out=%u\n",inId,outId,seconds,a,b);
    if(a || b) {if(in) midiInClose(in);return 1;}
    for(i=0;i<8;i++) {ih[i].lpData=(LPSTR)ib[i];ih[i].dwBufferLength=sizeof(ib[i]);
        midiInPrepareHeader(in,&ih[i],sizeof(ih[i]));midiInAddBuffer(in,&ih[i],sizeof(ih[i]));}
    ob[0]=0xf0;ob[1]=0x7d;for(i=2;i<sizeof(ob)-1;i++) ob[i]=(unsigned char)(i&127);ob[sizeof(ob)-1]=0xf7;
    oh.lpData=(LPSTR)ob;oh.dwBufferLength=sizeof(ob);midiOutPrepareHeader(out,&oh,sizeof(oh));
    oh.dwFlags|=MHDR_DONE;
    midiInStart(in);start=last=GetTickCount();
    while(GetTickCount()-start<seconds*1000) {
        DWORD note=0x90|((tick&127)<<8)|(64<<16);
        r=midiOutShortMsg(out,note);if(r && !firstOut) firstOut=r;if(!r) sent+=3;
        r=midiOutShortMsg(out,note&0xffff);if(r && !firstOut) firstOut=r;if(!r) sent+=3;
        if(!(tick%250) && (oh.dwFlags&MHDR_DONE)) {
            r=midiOutLongMsg(out,&oh,sizeof(oh));if(r && !firstOut) firstOut=r;if(!r) {sent+=sizeof(ob);sysexes++;}
        }
        for(i=0;i<8;i++) if(ih[i].dwFlags&MHDR_DONE) {ih[i].dwBytesRecorded=0;
            r=midiInAddBuffer(in,&ih[i],sizeof(ih[i]));if(r && !firstIn) firstIn=r;}
        tick++;Sleep(2);
        if(GetTickCount()-last>=1000) {last+=1000;secs++;
            EnterCriticalSection(&lock);
            printf("t=%u sent=%u received=%u errors=%u firstOut=%u firstIn=%u\n",secs,sent,count,errors,firstOut,firstIn);
            LeaveCriticalSection(&lock);}
    }
    Sleep(1000);
    if(midiInStop(in)) cleanup=0;
    if(midiInReset(in)) cleanup=0;
    for(i=0;i<8;i++) if(midiInUnprepareHeader(in,&ih[i],sizeof(ih[i]))) cleanup=0;
    if((r=midiInClose(in))!=0) {cleanup=0;printf("midiInClose=%u\n",r);}
    if(midiOutReset(out)) cleanup=0;
    if(midiOutUnprepareHeader(out,&oh,sizeof(oh))) cleanup=0;
    if((r=midiOutClose(out))!=0) {cleanup=0;printf("midiOutClose=%u\n",r);}
    printf("End sent=%u received=%u sysex=%u firstOut=%u firstIn=%u cleanup=%s\n",sent,count,sysexes,firstOut,firstIn,cleanup?"complete":"failed");
    return 0;
}
/* The newest System event log entries (source, id, inserted strings): the
 * OS's own record of a bugcheck ("Save Dump") or a driver load failure. */
static void Events(unsigned int max)
{
    HANDLE log=OpenEventLog(NULL,"System");static BYTE buffer[65536];
    DWORD read,need;unsigned int shown=0;
    if(!log) {printf("OpenEventLog error=%lu\n",GetLastError());return;}
    while(shown<max && ReadEventLog(log,EVENTLOG_SEQUENTIAL_READ|EVENTLOG_BACKWARDS_READ,0,buffer,sizeof(buffer),&read,&need)) {
        BYTE *p=buffer;
        while(p<buffer+read && shown<max) {
            EVENTLOGRECORD *r=(EVENTLOGRECORD*)p;const char *src=(const char*)(r+1),*s;WORD i;
            printf("#%lu t=%lu %s id=%lu type=%u:",r->RecordNumber,r->TimeGenerated,src,r->EventID&0xffff,r->EventType);
            for(i=0,s=(const char*)p+r->StringOffset;i<r->NumStrings;i++,s+=strlen(s)+1) printf(" [%s]",s);
            printf("\n");shown++;p+=r->Length;
        }
    }
    CloseEventLog(log);
}
int main(int argc,char **argv)
{
    HMIDIIN in=NULL; HMIDIOUT out=NULL;
    static MIDIHDR input[2],output;
    static unsigned char inData[2][1024],sysex[259];
    unsigned char expected[512];
    unsigned int i,n=0,got=0,bad=0,shorts=0,longs=0;
    UINT inId,outId;DWORD start; MMRESULT mm=0;
    int preparedIn[2]={0,0},preparedOut=0,started=0,ok=0,cleanup=1;
    setvbuf(stdout,NULL,_IONBF,0); /* retain diagnostics even if exit flushing fails */
    if((argc==2 || argc==3) && !strcmp(argv[1],"diag")) {
        if(argc==3 && !freopen(argv[2],"w",stdout)) return 1;
        Diagnostic();return fflush(stdout) || ferror(stdout)?1:0;
    }
    if(argc==2 && !strcmp(argv[1],"install")) return Install("USB\\VID_FFFF&PID_0001&MI_00","A:\\USBMIDI.INF");
    if(argc==4 && !strcmp(argv[1],"install")) return Install(argv[2],argv[3]);
    if(argc==3 && !strcmp(argv[1],"stage")) {
        /* Pre-install the INF (SetupCopyOEMInf) so a device plugged later is
         * matched to it by PnP directly. */
        char dest[MAX_PATH];BOOL ok=SetupCopyOEMInf(argv[2],NULL,SPOST_PATH,0,dest,sizeof(dest),NULL,NULL);
        printf("Stage %s success=%u error=%lu as %s\n",argv[2],ok,ok?0:GetLastError(),ok?dest:"-");
        return ok?0:1;
    }
    if((argc==4 || argc==5) && !strcmp(argv[1],"open") &&
        PortNumber(argv[2],&inId) && PortNumber(argv[3],&outId) &&
        inId<midiInGetNumDevs() && outId<midiOutGetNumDevs())
        return OpenOnly(inId,outId,argc==5?argv[4]:NULL);
    if(argc==1 || (argc==2 && !strcmp(argv[1],"list"))) { Ports();return 0; }
    if(argc==2 && !strcmp(argv[1],"wave")) return WaveOpen();
    if(argc==3 && !strcmp(argv[1],"eject")) {
        /* Orderly removal of one devnode (query remove, then remove), as a
         * user's "stop the device" would do, for the 98 SE replug
         * investigation: does a removal that is queried first change what
         * WinMM does on the next arrival? */
        typedef DWORD (WINAPI *LOCATE)(PULONG,LPSTR,ULONG);
        typedef DWORD (WINAPI *NODE)(ULONG,ULONG);
        HMODULE cm=LoadLibrary("cfgmgr32.dll");
        LOCATE locate=cm?(LOCATE)GetProcAddress(cm,"CM_Locate_DevNodeA"):NULL;
        NODE query=cm?(NODE)GetProcAddress(cm,"CM_Query_Remove_SubTree"):NULL,rem=cm?(NODE)GetProcAddress(cm,"CM_Remove_SubTree"):NULL;
        ULONG dn=0;DWORD r=0,a=0,b=0;
        if(!locate || !query || !rem) {printf("CM API unavailable\n");return 1;}
        r=locate(&dn,argv[2],0);
        if(!r) a=query(dn,1);
        if(!r && !a) b=rem(dn,1);
        printf("eject %s: locate=%lX query=%lX remove=%lX\n",argv[2],r,a,b);
        return (r||a||b)?1:0;
    }
    if(argc==4 && (!strcmp(argv[1],"disable") || !strcmp(argv[1],"enable") || !strcmp(argv[1],"cycle"))) {
        /* Disable and/or enable one devnode by device instance id (cfgmgr32),
         * for the 98 SE replug investigation. argv[3] is CM flags (decimal). */
        typedef DWORD (WINAPI *LOCATE)(PULONG,LPSTR,ULONG);
        typedef DWORD (WINAPI *NODE)(ULONG,ULONG);
        HMODULE cm=LoadLibrary("cfgmgr32.dll");
        LOCATE locate=cm?(LOCATE)GetProcAddress(cm,"CM_Locate_DevNodeA"):NULL;
        NODE off=cm?(NODE)GetProcAddress(cm,"CM_Disable_DevNode"):NULL,on=cm?(NODE)GetProcAddress(cm,"CM_Enable_DevNode"):NULL;
        ULONG dn=0,flags=(ULONG)atoi(argv[3]);DWORD r=0,a=0,b=0;
        if(!locate || !off || !on) {printf("CM API unavailable\n");return 1;}
        r=locate(&dn,argv[2],0);
        if(!r && strcmp(argv[1],"enable")) a=off(dn,flags);
        if(!r && strcmp(argv[1],"enable") && strcmp(argv[1],"disable")) Sleep(5000);
        if(!r && strcmp(argv[1],"disable")) b=on(dn,0);
        printf("%s %s: locate=%lX disable=%lX enable=%lX\n",argv[1],argv[2],r,a,b);
        return (r||a||b)?1:0;
    }
    if(argc==3 && !strcmp(argv[1],"outopen") && PortNumber(argv[2],&outId) && outId<midiOutGetNumDevs()) {
        /* Output alone, for devices with no MIDI input (the SW synth). */
        HMIDIOUT h=NULL;MMRESULT a=midiOutOpen(&h,outId,0,0,CALLBACK_NULL),b=h?midiOutClose(h):0;
        printf("Out-only %u: open=%u close=%u %s\n",outId,a,b,(a||b)?"FAIL":"PASS");
        return (a||b)?1:0;
    }
    if((argc==2 || argc==3) && !strcmp(argv[1],"events")) {
        if(argc==3 && !freopen(argv[2],"w",stdout)) return 1;
        Events(40);return fflush(stdout) || ferror(stdout)?1:0;
    }
    if((argc==5 || argc==6) && !strcmp(argv[1],"stress") && PortNumber(argv[2],&inId) &&
        PortNumber(argv[3],&outId) && inId<midiInGetNumDevs() && outId<midiOutGetNumDevs())
        return Stress(inId,outId,(unsigned int)atoi(argv[4]),argc==6?argv[5]:NULL);
    if((argc==2 || argc==3) && !strcmp(argv[1],"ifaces")) {
        if(argc==3 && !freopen(argv[2],"w",stdout)) return 1;
        Ports();Interfaces();return fflush(stdout) || ferror(stdout)?1:0;
    }
    if((argc!=3 && argc!=4) || !PortNumber(argv[1],&inId) || !PortNumber(argv[2],&outId) ||
        inId>=midiInGetNumDevs() || outId>=midiOutGetNumDevs()) {
        puts("Usage: MIDITEST list | diag [log] | install [hwid inf] | open in out [log] | in out [log]\n"
             "       | outopen out | wave | disable|enable|cycle devid flags | eject devid");return 2;
    }
    if(argc==4) {
        if(!freopen(argv[3],"w",stdout)) return 2;
        setvbuf(stdout,NULL,_IONBF,0);
    }
    printf("WinMM MIDI loopback input=%u output=%u; requires physical/simulated echo\n",inId,outId);
    InitializeCriticalSection(&lock);
    ZeroMemory(input,sizeof(input));ZeroMemory(&output,sizeof(output));
    for(i=0;i<32;i++) { expected[n++]=0x90;expected[n++]=(unsigned char)i;expected[n++]=(unsigned char)(i+1); }
    sysex[0]=0xf0;sysex[1]=0x7d;
    for(i=0;i<256;i++) sysex[i+2]=(unsigned char)(i&127);
    sysex[258]=0xf7;memcpy(expected+n,sysex,sizeof(sysex));n+=sizeof(sysex);
    mm=midiInOpen(&in,inId,(DWORD)Input,0,CALLBACK_FUNCTION);if(mm) goto done;
    mm=midiOutOpen(&out,outId,0,0,CALLBACK_NULL);if(mm) goto done;
    for(i=0;i<2;i++) {
        input[i].lpData=(LPSTR)inData[i];input[i].dwBufferLength=sizeof(inData[i]);
        mm=midiInPrepareHeader(in,&input[i],sizeof(input[i]));if(mm) goto done;
        preparedIn[i]=1;mm=midiInAddBuffer(in,&input[i],sizeof(input[i]));if(mm) goto done;
    }
    output.lpData=(LPSTR)sysex;output.dwBufferLength=sizeof(sysex);
    mm=midiOutPrepareHeader(out,&output,sizeof(output));if(mm) goto done;preparedOut=1;
    mm=midiInStart(in);if(mm) goto done;started=1;
    for(i=0;i<32;i++) {
        DWORD event=0x90 | (i<<8) | ((i+1)<<16);
        mm=midiOutShortMsg(out,event);if(mm) goto done;
    }
    mm=midiOutLongMsg(out,&output,sizeof(output));if(mm) goto done;
    start=GetTickCount();
    do {
        EnterCriticalSection(&lock);got=count;bad=errors;shorts=shortCount;longs=longCount;
        ok=(got==n && !bad && !memcmp(received,expected,n));LeaveCriticalSection(&lock);
        if(bad || got>n || (ok && (output.dwFlags&MHDR_DONE))) break;
        Sleep(10);
    } while(GetTickCount()-start<5000);
    ok=ok && !!(output.dwFlags&MHDR_DONE) && shorts==32 && longs>=1;
    printf("Received=%u expected=%u short=%u long=%u errors=%u outputDone=%u\n",
        got,n,shorts,longs,bad,!!(output.dwFlags&MHDR_DONE));
done:
    if(mm) { printf("WinMM failure=%u\n",mm);ok=0; }
    if(in) {
        if(started && midiInStop(in)) cleanup=0;
        if(midiInReset(in)) cleanup=0;
        for(i=0;i<2;i++) if(preparedIn[i] && midiInUnprepareHeader(in,&input[i],sizeof(input[i]))) cleanup=0;
        if(midiInClose(in)) cleanup=0;
    }
    if(out) {
        if(midiOutReset(out)) cleanup=0;
        if(preparedOut && midiOutUnprepareHeader(out,&output,sizeof(output))) cleanup=0;
        if(midiOutClose(out)) cleanup=0;
    }
    /* If a driver retained a buffer/callback, leave storage and the lock
     * live until process exit rather than freeing them underneath it. */
    EnterCriticalSection(&lock);
    ok=ok && count==n && !errors && !memcmp(received,expected,n) && shortCount==32 && longCount>=1;
    printf("Final received=%u errors=%u\n",count,errors);
    LeaveCriticalSection(&lock);
    if(cleanup) DeleteCriticalSection(&lock);
    printf("%s; cleanup=%s\n",ok&&cleanup?"PASS":"FAIL",cleanup?"complete":"failed");
    if(fflush(stdout)==EOF || ferror(stdout)) return 1;
    return ok&&cleanup?0:1;
}
