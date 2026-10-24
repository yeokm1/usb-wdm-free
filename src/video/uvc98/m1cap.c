/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original ROOT\UVCSIM1-only user-mode KS read diagnostic. No camera/USB.
 * Public ks.h/ksmedia.h, SetupAPI, KsCreatePin and KS IOCTL documentation.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <winioctl.h>
#include <stdlib.h>
#include <string.h>
#include <ks.h>
#include <ksmedia.h>
typedef DWORD (WINAPI *CREATEPIN)(HANDLE,PKSPIN_CONNECT,ACCESS_MASK,PHANDLE);
static GUID captureCategory={STATIC_KSCATEGORY_CAPTURE};
static const GUID connectionSet={STATIC_KSPROPSETID_Connection};
static const GUID streamInterface={STATIC_KSINTERFACESETID_Standard};
static const GUID mediumSet={STATIC_KSMEDIUMSETID_Standard};
static const GUID typeVideo={STATIC_KSDATAFORMAT_TYPE_VIDEO};
static const GUID videoInfo={STATIC_KSDATAFORMAT_SPECIFIER_VIDEOINFO};
static const GUID yuy2={0x32595559,0,0x10,{0x80,0,0,0xaa,0,0x38,0x9b,0x71}};
static char report[8192];
/* Exact hardware ID of the one device opened; default the synthetic M1.
 * An optional command line names another (M2: USB\VID_046D&PID_094C). */
static char target[128]="ROOT\\UVCSIM1";
/* Optional "WxH" after the ID: the advertised frame size to open (M2). */
static DWORD wantWidth,wantHeight;
static HWND output;
static void Log(const char *text)
{
    HANDLE f; DWORD n=0,off=0,len,error=0; char bad[96];
    if(lstrlen(report)+lstrlen(text)<sizeof(report)) lstrcat(report,text);
    SetWindowText(output,report); UpdateWindow(output);
    len=(DWORD)lstrlen(report);
    f=CreateFile("A:\\M1CAP.TXT",GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(f==INVALID_HANDLE_VALUE) error=GetLastError();
    else {
        while(off<len) {
            if(!WriteFile(f,report+off,len-off,&n,NULL)) {error=GetLastError();break;}
            if(!n || n>len-off) {error=ERROR_WRITE_FAULT;break;}
            off+=n;
        }
        if(!FlushFileBuffers(f) && !error) error=GetLastError();
        if(!CloseHandle(f) && !error) error=GetLastError();
    }
    if(error) {wsprintf(bad,"REPORT SAVE FAILED error=%lu\r\n",error);SetWindowText(output,bad);}
}
/* Why no filter opened (98 SE diagnosis): counts, error codes and the first
 * hardware ID of a non-matching interface only; never paths or instance IDs. */
static DWORD seenInterfaces,detailFailed,idMismatch,idError,openError;
static char firstOther[128];
static int Exact(HDEVINFO set,SP_DEVINFO_DATA *d)
{
    char ids[2048]; DWORD type,size=0,off=0;
    if(!SetupDiGetDeviceRegistryProperty(set,d,SPDRP_HARDWAREID,&type,(PBYTE)ids,sizeof(ids),&size)) {
        if(!idError) idError=GetLastError()?GetLastError():1;
        return 0;
    }
    if(type==REG_SZ) {
        /* Windows 98 SE answers REG_SZ here, with the buffer size as the
         * required size (seen 2026-10-22), a single or comma-separated
         * list: rebuild the MULTI_SZ form below from the string itself. */
        DWORD k,n;
        if(size>sizeof(ids)-2) size=sizeof(ids)-2;
        ids[size]=0; n=(DWORD)lstrlen(ids);
        for(k=0;k<n;++k) if(ids[k]==',') ids[k]=0;
        ids[n]=0; ids[n+1]=0; size=n+2; type=REG_MULTI_SZ;
    }
    if(type!=REG_MULTI_SZ || size<2 || size>sizeof(ids) || ids[size-1] || ids[size-2]) {
        if(!idError) idError=0x80000000|type|(size<<8);
        return 0;
    }
    if(!firstOther[0]) lstrcpyn(firstOther,ids,sizeof(firstOther));
    while(off<size-1 && ids[off]) {
        if(!lstrcmpi(ids+off,target)) return 1;
        off+=(DWORD)lstrlen(ids+off)+1;
    }
    return 0;
}
static HANDLE FilterOpen(DWORD flags)
{
    HDEVINFO set=SetupDiGetClassDevs(&captureCategory,NULL,NULL,DIGCF_DEVICEINTERFACE|DIGCF_PRESENT);
    SP_DEVICE_INTERFACE_DATA i; SP_DEVINFO_DATA d;
    BYTE space[4096]; PSP_DEVICE_INTERFACE_DETAIL_DATA detail=(PSP_DEVICE_INTERFACE_DETAIL_DATA)space;
    DWORD index,needed; HANDLE h=INVALID_HANDLE_VALUE;
    if(set==INVALID_HANDLE_VALUE) return h;
    for(index=0;index<128;++index) {
        ZeroMemory(&i,sizeof(i));i.cbSize=sizeof(i);
        if(!SetupDiEnumDeviceInterfaces(set,NULL,&captureCategory,index,&i)) break;
        ZeroMemory(&d,sizeof(d));d.cbSize=sizeof(d);detail->cbSize=sizeof(*detail);
        ++seenInterfaces;
        if(!SetupDiGetDeviceInterfaceDetail(set,&i,detail,sizeof(space),&needed,&d)) { ++detailFailed; continue; }
        if(!Exact(set,&d)) { ++idMismatch; continue; }
        h=CreateFile(detail->DevicePath,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
            NULL,OPEN_EXISTING,flags,NULL);
        if(h==INVALID_HANDLE_VALUE) openError=GetLastError();
        break; /* no other device is opened, paths/instance IDs never logged */
    }
    SetupDiDestroyDeviceInfoList(set); return h;
}
static HANDLE Filter(void){return FilterOpen(FILE_FLAG_OVERLAPPED);}
static int Io(HANDLE pin,DWORD code,PVOID in,DWORD ins,PVOID out,DWORD outs,DWORD *done)
{
    OVERLAPPED ov; DWORD error,wait; BOOL ok;
    ZeroMemory(&ov,sizeof(ov));ov.hEvent=CreateEvent(NULL,TRUE,FALSE,NULL);
    if(!ov.hEvent) return 0;
    ok=DeviceIoControl(pin,code,in,ins,out,outs,done,&ov);
    error=ok?0:GetLastError();
    if(!ok && error==ERROR_IO_PENDING) {
        wait=WaitForSingleObject(ov.hEvent,5000);
        if(wait!=WAIT_OBJECT_0) {
            Log("IO deadline reached; cancelling this thread's request.\r\n");
            CancelIo(pin); wait=WaitForSingleObject(ov.hEvent,3000);
            if(wait!=WAIT_OBJECT_0) {
                Log("FAIL: cancellation did not retire; retaining all buffers/handles. Guest cleanup required.\r\n");
                Sleep(INFINITE); /* never free memory referenced by a pending IRP */
            }
            GetOverlappedResult(pin,&ov,done,FALSE); CloseHandle(ov.hEvent);
            SetLastError(ERROR_TIMEOUT); return 0;
        }
        ok=GetOverlappedResult(pin,&ov,done,FALSE);error=ok?0:GetLastError();
    }
    CloseHandle(ov.hEvent);SetLastError(error);return ok!=FALSE;
}
static int State(HANDLE pin,KSSTATE state)
{
    KSPROPERTY p;DWORD done=0;char line[96];int ok;
    ZeroMemory(&p,sizeof(p));p.Set=connectionSet;p.Id=KSPROPERTY_CONNECTION_STATE;p.Flags=KSPROPERTY_TYPE_SET;
    ok=Io(pin,IOCTL_KS_PROPERTY,&p,sizeof(p),&state,sizeof(state),&done);
    wsprintf(line,"State %u ok=%u error=%lu\r\n",state,ok,ok?0:GetLastError());Log(line);return ok;
}
/* Diagnostic only: what the filter itself publishes for pin 0, through the
 * documented KSPROPSETID_Pin properties (ks.h; IOCTL_KS_PROPERTY with a
 * KSP_PIN request). Values are logged, never used to loosen a check. */
static const GUID pinSet={STATIC_KSPROPSETID_Pin};
/* 64 KB: room for every range uvc98 advertises (UVFMT_MAX 64 x 296 bytes). */
static ULONG pinBuffer[16384];
static int PinQuery(HANDLE filter,ULONG id,DWORD *done)
{
    KSP_PIN p;char line[96];int ok;
    ZeroMemory(&p,sizeof(p));p.Property.Set=pinSet;p.Property.Id=id;p.Property.Flags=KSPROPERTY_TYPE_GET;p.PinId=0;
    ZeroMemory(pinBuffer,sizeof(pinBuffer));*done=0;
    ok=Io(filter,IOCTL_KS_PROPERTY,&p,sizeof(p),pinBuffer,sizeof(pinBuffer),done);
    wsprintf(line,"PIN0 property %lu ok=%u bytes=%lu error=%lu\r\n",id,ok,*done,ok?0:GetLastError());Log(line);
    return ok;
}
static void LogIdentifiers(const char *what,DWORD done)
{
    KSMULTIPLE_ITEM *m=(KSMULTIPLE_ITEM *)pinBuffer;KSIDENTIFIER *k=(KSIDENTIFIER *)(m+1);ULONG i;char line[144];
    if(done<sizeof(*m) || m->Size>done || m->Size<sizeof(*m)) {Log("  malformed KSMULTIPLE_ITEM\r\n");return;}
    for(i=0;i<m->Count && (BYTE *)(k+i+1)<=(BYTE *)pinBuffer+m->Size;++i) {
        wsprintf(line,"  %s[%lu] set=%08lX-%04X id=%lu flags=%lu\r\n",what,i,
            k[i].Set.Data1,k[i].Set.Data2,k[i].Id,k[i].Flags);Log(line);
    }
}
static int Describe(HANDLE filter,KS_DATAFORMAT_VIDEOINFOHEADER *built,int adopt)
{
    KSPROPERTY q;DWORD done=0,i;char line[160];int ok;
    struct {KSP_PIN Pin;KSMULTIPLE_ITEM Ranges;KS_DATARANGE_VIDEO Range;} x;
    KSMULTIPLE_ITEM *m=(KSMULTIPLE_ITEM *)pinBuffer;KSDATARANGE *r;KS_DATAFORMAT_VIDEOINFOHEADER got;
    ZeroMemory(&q,sizeof(q));q.Set=pinSet;q.Id=KSPROPERTY_PIN_CTYPES;q.Flags=KSPROPERTY_TYPE_GET;
    ok=Io(filter,IOCTL_KS_PROPERTY,&q,sizeof(q),pinBuffer,sizeof(ULONG),&done);
    wsprintf(line,"PIN_CTYPES ok=%u value=%lu error=%lu\r\n",ok,ok?pinBuffer[0]:0,ok?0:GetLastError());Log(line);
    if(PinQuery(filter,KSPROPERTY_PIN_DATAFLOW,&done)){wsprintf(line,"  dataflow=%lu\r\n",pinBuffer[0]);Log(line);}
    if(PinQuery(filter,KSPROPERTY_PIN_COMMUNICATION,&done)){wsprintf(line,"  communication=%lu\r\n",pinBuffer[0]);Log(line);}
    if(PinQuery(filter,KSPROPERTY_PIN_CINSTANCES,&done)){wsprintf(line,"  possible=%lu current=%lu\r\n",pinBuffer[0],pinBuffer[1]);Log(line);}
    if(PinQuery(filter,KSPROPERTY_PIN_INTERFACES,&done)) LogIdentifiers("interface",done);
    if(PinQuery(filter,KSPROPERTY_PIN_MEDIUMS,&done)) LogIdentifiers("medium",done);
    if(!PinQuery(filter,KSPROPERTY_PIN_DATARANGES,&done) || done<sizeof(*m)+sizeof(KSDATARANGE) ||
       m->Size>done || !m->Count) {Log("  no usable data range\r\n");return 0;}
    r=(KSDATARANGE *)(m+1);
    wsprintf(line,"  ranges=%lu first size=%lu flags=%lu sample=%lu major=%08lX sub=%08lX spec=%08lX\r\n",
        m->Count,r->FormatSize,r->Flags,r->SampleSize,r->MajorFormat.Data1,r->SubFormat.Data1,r->Specifier.Data1);Log(line);
    {   /* Every advertised range (quad-aligned KSMULTIPLE_ITEM entries); the
         * requested size, or the first, is the one intersected. */
        KSDATARANGE *pick=NULL,*it=r;ULONG k;
        for(k=0;k<m->Count;++k) {
            if(it->FormatSize<sizeof(KSDATARANGE) || (BYTE *)it+it->FormatSize>(BYTE *)pinBuffer+done) break;
            if(it->FormatSize==sizeof(KS_DATARANGE_VIDEO)) {
                KS_DATARANGE_VIDEO *v=(KS_DATARANGE_VIDEO *)it;
                wsprintf(line,"  range %lu: %ldx%ld sample=%lu interval=%lu\r\n",k,
                    v->VideoInfoHeader.bmiHeader.biWidth,v->VideoInfoHeader.bmiHeader.biHeight,
                    it->SampleSize,(DWORD)v->VideoInfoHeader.AvgTimePerFrame);Log(line);
                /* YUY2 only: the frames are saved and compared as raw YUY2. */
                if(!pick && it->SubFormat.Data1==0x32595559 &&
                   (!wantWidth || ((DWORD)v->VideoInfoHeader.bmiHeader.biWidth==wantWidth &&
                   (DWORD)v->VideoInfoHeader.bmiHeader.biHeight==wantHeight))) pick=it;
            }
            it=(KSDATARANGE *)((BYTE *)it+((it->FormatSize+7)&~7UL));
        }
        if(!pick) {Log("  requested size not advertised\r\n");return 0;}
        r=pick;
    }
    /* DATAINTERSECTION reaches the minidriver's SRB_GET_DATA_INTERSECTION. */
    ZeroMemory(&x,sizeof(x));x.Pin.Property.Set=pinSet;x.Pin.Property.Id=KSPROPERTY_PIN_DATAINTERSECTION;
    x.Pin.Property.Flags=KSPROPERTY_TYPE_GET;x.Pin.PinId=0;
    x.Ranges.Size=sizeof(x.Ranges)+sizeof(x.Range);x.Ranges.Count=1;CopyMemory(&x.Range,r,sizeof(x.Range));
    ZeroMemory(&got,sizeof(got));done=0;
    ok=Io(filter,IOCTL_KS_PROPERTY,&x,sizeof(x),&got,sizeof(got),&done);
    wsprintf(line,"PIN_DATAINTERSECTION ok=%u bytes=%lu error=%lu\r\n",ok,done,ok?0:GetLastError());Log(line);
    if(!ok || done!=sizeof(got)) return 0;
    if(adopt) {
        /* M2: connect with the format the driver's own intersection chose. */
        CopyMemory(built,&got,sizeof(got));
        wsprintf(line,"  connect format %ldx%ld sample=%lu\r\n",got.VideoInfoHeader.bmiHeader.biWidth,
            got.VideoInfoHeader.bmiHeader.biHeight,got.DataFormat.SampleSize);Log(line);
        return got.DataFormat.SampleSize!=0;
    }
    for(i=0;i<sizeof(got) && ((BYTE *)&got)[i]==((BYTE *)built)[i];++i);
    if(i==sizeof(got)) Log("  intersection format equals the hand-built connect format\r\n");
    else {wsprintf(line,"  intersection format differs first at byte %lu of %lu\r\n",i,(DWORD)sizeof(got));Log(line);}
    return 1;
}
static void Run(void)
{
    struct {KSPIN_CONNECT Pin;KS_DATAFORMAT_VIDEOINFOHEADER Format;} c;
    HMODULE lib;CREATEPIN create;HANDLE filter,pin=NULL;DWORD result,done,n,crc,previous=0,bytes;int pass=1,real;
    /* A video capture pin's stream header carries a KS_FRAME_INFO (2000 DDK,
     * "Video Stream Extended Headers"); the driver declares it. */
    struct {KSSTREAM_HEADER h;KS_FRAME_INFO f;} rx;PUCHAR data;char line[144];
    Log("KS read checker for exact hardware ID ");Log(target);
    Log(lstrcmpi(target,"ROOT\\UVCSIM1") ? " (M2: real camera frames).\r\n" : " (synthetic, no camera).\r\n");
    filter=Filter();if(filter==INVALID_HANDLE_VALUE){
        Log("FAIL: exact synthetic filter absent/open failed.\r\n");
        wsprintf(line,"  capture interfaces=%lu detailFailed=%lu idMismatch=%lu idError=%lx openError=%lu\r\n",
            seenInterfaces,detailFailed,idMismatch,idError,openError);Log(line);
        if(firstOther[0]){Log("  first hardware ID seen: ");Log(firstOther);Log("\r\n");}
        return;}
    lib=LoadLibrary("ksuser.dll");create=lib?(CREATEPIN)GetProcAddress(lib,"KsCreatePin"):NULL;
    if(!create){Log("FAIL: published KsCreatePin unavailable.\r\n");CloseHandle(filter);if(lib)FreeLibrary(lib);return;}
    ZeroMemory(&c,sizeof(c));c.Pin.Interface.Set=streamInterface;c.Pin.Interface.Id=KSINTERFACE_STANDARD_STREAMING;
    c.Pin.Medium.Set=mediumSet;c.Pin.Medium.Id=KSMEDIUM_STANDARD_DEVIO;c.Pin.Priority.PriorityClass=KSPRIORITY_NORMAL;
    c.Format.DataFormat.FormatSize=sizeof(c.Format);c.Format.DataFormat.SampleSize=153600;
    c.Format.DataFormat.MajorFormat=typeVideo;c.Format.DataFormat.SubFormat=yuy2;c.Format.DataFormat.Specifier=videoInfo;
    c.Format.VideoInfoHeader.rcSource.right=320;c.Format.VideoInfoHeader.rcSource.bottom=240;
    c.Format.VideoInfoHeader.rcTarget=c.Format.VideoInfoHeader.rcSource;
    c.Format.VideoInfoHeader.AvgTimePerFrame=1000000;c.Format.VideoInfoHeader.dwBitRate=153600*8*10;
    c.Format.VideoInfoHeader.bmiHeader.biSize=sizeof(KS_BITMAPINFOHEADER);
    c.Format.VideoInfoHeader.bmiHeader.biWidth=320;c.Format.VideoInfoHeader.bmiHeader.biHeight=240;
    c.Format.VideoInfoHeader.bmiHeader.biPlanes=1;c.Format.VideoInfoHeader.bmiHeader.biBitCount=16;
    c.Format.VideoInfoHeader.bmiHeader.biCompression=0x32595559;c.Format.VideoInfoHeader.bmiHeader.biSizeImage=153600;
    real=lstrcmpi(target,"ROOT\\UVCSIM1")!=0;
    if(!Describe(filter,&c.Format,real) && real) {
        Log("FAIL: no usable advertised format.\r\n");CloseHandle(filter);FreeLibrary(lib);return;
    }
    bytes=c.Format.DataFormat.SampleSize;
    wsprintf(line,"Connect bytes=%lu format=%lu\r\n",(DWORD)sizeof(c),c.Format.DataFormat.FormatSize);Log(line);
    /* KSPRIORITY: KSPRIORITY_NORMAL with PrioritySubClass 1. On Windows 2000
     * a zero subclass made KsCreatePin fail with 87 before any SRB reached the
     * minidriver (2026-10-21 run); with 1 the same connect created the pin.
     * The zero-subclass create is kept as a logged negative control only. */
    pin=NULL;result=create(filter,&c.Pin,GENERIC_READ,&pin);
    wsprintf(line,"Control: KsCreatePin priority subclass 0 result=%lu\r\n",result);Log(line);
    if(!result && pin) {Log("  control unexpectedly created a pin; closing it\r\n");CloseHandle(pin);}
    c.Pin.Priority.PrioritySubClass=1;
    pin=NULL;result=create(filter,&c.Pin,GENERIC_READ,&pin);
    wsprintf(line,"KsCreatePin priority subclass 1 result=%lu\r\n",result);Log(line);
    if(result || !pin) {CloseHandle(filter);FreeLibrary(lib);return;}
    data=HeapAlloc(GetProcessHeap(),0,bytes);
    if(!data) pass=0;
    if(pass) pass=State(pin,KSSTATE_ACQUIRE) && State(pin,KSSTATE_PAUSE) && State(pin,KSSTATE_RUN);
    for(n=0;pass && n<3;++n) {
        DWORD j,error;int ioOk;ZeroMemory(&rx,sizeof(rx));ZeroMemory(data,bytes);
        rx.h.Size=sizeof(rx);rx.h.FrameExtent=bytes;rx.h.Data=data;
        rx.h.PresentationTime.Numerator=rx.h.PresentationTime.Denominator=1;
        /* Same header array supplied both ways: METHOD_NEITHER probe/read
         * receives a valid writable header and its separate payload buffer. */
        ioOk=Io(pin,IOCTL_KS_READ_STREAM,&rx,sizeof(rx),&rx,sizeof(rx),&done);
        error=ioOk?0:GetLastError();pass=ioOk;
        crc=2166136261UL;
        if(pass && rx.h.DataUsed==bytes) for(j=0;j<bytes;++j) crc=(crc^data[j])*16777619UL;
        else pass=0;
        wsprintf(line,"READ %lu ok=%u data=%lu headerBytes=%lu FNV1a=%08lX error=%lu picture=%lu drops=%lu\r\n",
            n+1,pass,rx.h.DataUsed,done,crc,error,(DWORD)rx.f.PictureNumber,(DWORD)rx.f.DropCount);Log(line);
        if(n && pass && previous==crc){Log("FAIL: successive frame did not change.\r\n");pass=0;}
        if(pass)previous=crc;
        /* A real camera's frames are kept as raw YUY2 for viewing on the host. */
        if(pass && real) {
            char name[32];HANDLE f;DWORD w=0;
            wsprintf(name,"A:\\FRAME%lu.YUY",n+1);
            f=CreateFile(name,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
            if(f==INVALID_HANDLE_VALUE || !WriteFile(f,data,bytes,&w,NULL) || w!=bytes ||
               !FlushFileBuffers(f)) Log("frame save failed\r\n");
            else {wsprintf(line,"saved %s\r\n",name);Log(line);}
            if(f!=INVALID_HANDLE_VALUE) CloseHandle(f);
        }
    }
    if(!State(pin,KSSTATE_STOP))pass=0;
    if(!CloseHandle(pin))pass=0;
    if(data)HeapFree(GetProcessHeap(),0,data);
    if(!CloseHandle(filter))pass=0;FreeLibrary(lib);
    Log(pass?"PASS: three complete changing frames; STOP and pin/filter close returned.\r\n":
        "FAIL: capture/state/close check incomplete.\r\n");
}
static LRESULT CALLBACK Window(HWND w,UINT m,WPARAM a,LPARAM b)
{if(m==WM_DESTROY){PostQuitMessage(0);return 0;}return DefWindowProc(w,m,a,b);}
int WINAPI WinMain(HINSTANCE instance,HINSTANCE previous,LPSTR command,int show)
{
    WNDCLASS cls;HWND w;MSG msg;(void)previous;
    if(command && *command) {
        char *size;
        lstrcpyn(target,command,sizeof(target));
        size=strchr(target,' ');
        if(size) { *size++=0; wantWidth=strtoul(size,&size,10); if(*size=='x') wantHeight=strtoul(size+1,NULL,10); }
    }
    ZeroMemory(&cls,sizeof(cls));cls.lpfnWndProc=Window;cls.hInstance=instance;
    cls.lpszClassName="M1KsCheck";cls.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);
    if(!RegisterClass(&cls))return 1;
    w=CreateWindow(cls.lpszClassName,"M1 synthetic KS read check - no camera",WS_OVERLAPPEDWINDOW,
        10,10,780,480,NULL,NULL,instance,NULL);
    output=CreateWindow("EDIT","",WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,
        8,8,744,416,w,NULL,instance,NULL);
    ShowWindow(w,show);UpdateWindow(w);Run();
    while(GetMessage(&msg,NULL,0,0)>0){TranslateMessage(&msg);DispatchMessage(&msg);}
    return 0; /* verdict is report, not GUI exit status */
}
