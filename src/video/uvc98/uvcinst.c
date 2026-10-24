/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original test-only SetupAPI/NewDev installer for ROOT\UVCSIM98 only.
 * Never matches USB devices. Fixed A:\UVCSIM.INF input/report paths.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>

static GUID media = {0x4d36e96c,0xe325,0x11ce,{0xbf,0xc1,0x08,0,0x2b,0xe1,3,0x18}};
static const char hardware[] = "ROOT\\UVCSIM98\0";
static char report[4096];
static HWND output;
typedef BOOL (WINAPI *UPDATE)(HWND,LPCSTR,LPCSTR,DWORD,PBOOL);
typedef DWORD (WINAPI *CMSTATUS)(PULONG,PULONG,DWORD,ULONG);

static void Add(const char *line)
{
    if (lstrlen(report) + lstrlen(line) < sizeof(report)) lstrcat(report,line);
}

static void Save(void)
{
    HANDLE file;
    DWORD written;
    SetWindowText(output,report);
    file=CreateFile("A:\\SIMLOG.TXT",GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if (file!=INVALID_HANDLE_VALUE) {
        WriteFile(file,report,lstrlen(report),&written,NULL);
        CloseHandle(file);
    }
}

static BOOL Exact(HDEVINFO set,SP_DEVINFO_DATA *device)
{
    char ids[4096];
    DWORD type,size=0,offset=0;
    if (!SetupDiGetDeviceRegistryProperty(set,device,SPDRP_HARDWAREID,&type,
            (PBYTE)ids,sizeof(ids),&size) || type!=REG_MULTI_SZ || size<2 ||
            size>sizeof(ids) || ids[size-1] || ids[size-2]) return FALSE;
    while (offset<size-1 && ids[offset]) {
        if (!lstrcmpi(ids+offset,hardware)) return TRUE;
        offset+=lstrlen(ids+offset)+1;
    }
    return FALSE;
}

/* Returns an owned set and device, or INVALID_HANDLE_VALUE. */
static HDEVINFO Find(SP_DEVINFO_DATA *device,DWORD *error)
{
    HDEVINFO set=SetupDiGetClassDevs(NULL,NULL,NULL,DIGCF_ALLCLASSES);
    DWORD i;
    if(set==INVALID_HANDLE_VALUE) { *error=GetLastError(); return set; }
    for(i=0;i<4096;i++) {
        ZeroMemory(device,sizeof(*device)); device->cbSize=sizeof(*device);
        if(!SetupDiEnumDeviceInfo(set,i,device)) {
            *error=GetLastError();
            if(*error==ERROR_NO_MORE_ITEMS) *error=ERROR_NOT_FOUND;
            break;
        }
        if(Exact(set,device)) { *error=0; return set; }
    }
    if(i==4096) *error=ERROR_MORE_DATA;
    SetupDiDestroyDeviceInfoList(set);
    return INVALID_HANDLE_VALUE;
}

static void Refresh(void)
{
    SP_DEVINFO_DATA device;
    DWORD error=0,flags=0,problem=0;
    HDEVINFO set=Find(&device,&error);
    char line[256],service[128];
    DWORD type;
    HMODULE cfg;
    CMSTATUS status;
    if(set==INVALID_HANDLE_VALUE) {
        wsprintf(line,"Target ROOT\\UVCSIM98 absent/status error=%lu\r\n",error); Add(line);
    } else {
        service[0]=0;
        SetupDiGetDeviceRegistryProperty(set,&device,SPDRP_SERVICE,&type,(PBYTE)service,sizeof(service),NULL);
        service[127]=0;
        cfg=LoadLibrary("cfgmgr32.dll");
        status=cfg ? (CMSTATUS)GetProcAddress(cfg,"CM_Get_DevNode_Status") : NULL;
        if(status) {
            error=status(&flags,&problem,device.DevInst,0);
            wsprintf(line,"Exact synthetic target service=%s CMresult=%lu flags=%08lX problem=%lu\r\n",service,error,flags,problem); Add(line);
        }
        if(cfg) FreeLibrary(cfg);
        SetupDiDestroyDeviceInfoList(set);
    }
    Save();
}

static void Install(HWND window)
{
    SP_DEVINFO_DATA device;
    DWORD error=0;
    HDEVINFO set=Find(&device,&error);
    char line[160];
    BOOL ok,reboot=FALSE;
    HMODULE library;
    UPDATE update;
    if(set==INVALID_HANDLE_VALUE && error==ERROR_NOT_FOUND) {
        set=SetupDiCreateDeviceInfoList(&media,window);
        ZeroMemory(&device,sizeof(device)); device.cbSize=sizeof(device);
        ok=set!=INVALID_HANDLE_VALUE && SetupDiCreateDeviceInfo(set,"UVCSIM98",&media,
            "SIMULATED YUY2 test source",window,DICD_GENERATE_ID,&device);
        if(ok) ok=SetupDiSetDeviceRegistryProperty(set,&device,SPDRP_HARDWAREID,
            (const BYTE *)hardware,sizeof(hardware));
        if(ok) ok=SetupDiCallClassInstaller(DIF_REGISTERDEVICE,set,&device);
        error=ok ? 0 : GetLastError();
        wsprintf(line,"Create/register exact synthetic root device ok=%u error=%lu\r\n",ok,error); Add(line); Save();
    }
    if(set!=INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set);
    if(error) { wsprintf(line,"Target creation/find failed error=%lu\r\n",error); Add(line); Save(); return; }
    library=LoadLibrary("newdev.dll");
    update=library ? (UPDATE)GetProcAddress(library,"UpdateDriverForPlugAndPlayDevicesA") : NULL;
    if(!update) SetLastError(ERROR_PROC_NOT_FOUND);
    ok=update && update(window,hardware,"A:\\UVCSIM.INF",1,&reboot);
    error=ok ? 0 : GetLastError();
    wsprintf(line,"NewDev ROOT-only install ok=%u error=%lu reboot=%u\r\n",ok,error,reboot); Add(line);
    if(library) FreeLibrary(library);
    Refresh();
}

static void Remove(void)
{
    SP_DEVINFO_DATA device;
    SP_REMOVEDEVICE_PARAMS params;
    DWORD error=0;
    HDEVINFO set=Find(&device,&error);
    BOOL ok=FALSE;
    char line[160];
    if(set!=INVALID_HANDLE_VALUE) {
        ZeroMemory(&params,sizeof(params));
        params.ClassInstallHeader.cbSize=sizeof(SP_CLASSINSTALL_HEADER);
        params.ClassInstallHeader.InstallFunction=DIF_REMOVE;
        params.Scope=DI_REMOVEDEVICE_GLOBAL;
        ok=SetupDiSetClassInstallParams(set,&device,&params.ClassInstallHeader,sizeof(params)) &&
            SetupDiCallClassInstaller(DIF_REMOVE,set,&device);
        error=ok ? 0 : GetLastError();
        SetupDiDestroyDeviceInfoList(set);
    }
    wsprintf(line,"Remove exact synthetic root device ok=%u error=%lu\r\n",ok,error); Add(line); Refresh();
}

static LRESULT CALLBACK Window(HWND window,UINT message,WPARAM w,LPARAM l)
{
    if(message==WM_COMMAND) {
        if(LOWORD(w)==1) Install(window);
        if(LOWORD(w)==2) Refresh();
        if(LOWORD(w)==3) Remove();
        return 0;
    }
    if(message==WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(window,message,w,l);
}

int WINAPI WinMain(HINSTANCE instance,HINSTANCE previous,LPSTR command,int show)
{
    WNDCLASS cls;
    HWND window,button;
    MSG msg;
    UNREFERENCED_PARAMETER(previous); UNREFERENCED_PARAMETER(command);
    ZeroMemory(&cls,sizeof(cls));
    cls.lpfnWndProc=Window; cls.hInstance=instance;
    cls.lpszClassName="UvcSimInstall"; cls.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);
    cls.hCursor=LoadCursor(NULL,IDC_ARROW);
    if(!RegisterClass(&cls)) return 1;
    window=CreateWindow(cls.lpszClassName,"SIMULATED root camera M0 - A:\\SIMLOG.TXT",
        WS_OVERLAPPEDWINDOW,20,20,760,500,NULL,NULL,instance,NULL);
    button=CreateWindow("BUTTON","Install SIMULATED root device",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,
        8,8,300,30,window,(HMENU)1,instance,NULL);
    CreateWindow("BUTTON","Refresh",WS_CHILD|WS_VISIBLE|WS_TABSTOP,314,8,150,30,window,(HMENU)2,instance,NULL);
    CreateWindow("BUTTON","Remove SIMULATED device",WS_CHILD|WS_VISIBLE|WS_TABSTOP,470,8,260,30,window,(HMENU)3,instance,NULL);
    output=CreateWindow("EDIT","",WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,
        8,46,722,400,window,NULL,instance,NULL);
    Add("ROOT-only SIMULATED camera M0 installer. STOP pin only; no frames or USB.\r\n");
    Refresh(); ShowWindow(window,show); SetFocus(button);
    while(GetMessage(&msg,NULL,0,0)>0) {
        if(!IsDialogMessage(window,&msg)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    }
    return 0;
}
