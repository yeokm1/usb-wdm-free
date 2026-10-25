/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Test-only GUI for X4 CDC binding and the separately identified simulated
 * ACM device. X4 gets open/GetCommState/close only. Generated echo traffic
 * and line settings are used only if the USB product location or bus-reported
 * description explicitly identifies our SIMULATED device. No board echo claim.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>

static GUID ports = {0x4d36e978,0xe325,0x11ce,{0xbf,0xc1,0x08,0x00,0x2b,0xe1,0x03,0x18}};
static char report[16384];
static HWND output;
typedef DWORD (WINAPI *DEVSTATUS)(PULONG,PULONG,DWORD,ULONG);
typedef BOOL (WINAPI *UPDATEDRIVER)(HWND,LPCSTR,LPCSTR,DWORD,PBOOL);
static void Append(const char *text);

/* Public devpkey.h/devpropdef.h ABI facts, queried dynamically so the same
 * source still runs on 2000/XP. No Vista-only static SetupAPI import.
 */
typedef struct { GUID Format; DWORD Property; } PROBE_KEY;
typedef BOOL (WINAPI *DEVPROPERTY)(HDEVINFO,PSP_DEVINFO_DATA,const PROBE_KEY *,
                                  DWORD *,PBYTE,DWORD,DWORD *,DWORD);
static BOOL Simulator(HDEVINFO set,PSP_DEVINFO_DATA dev,const char *location)
{
    static const PROBE_KEY busDescription = {
        {0x540b947e,0x8b40,0x45bc,{0xa8,0xa2,0x6a,0x0b,0x89,0x4c,0xbd,0xa2}},4
    };
    HMODULE module=GetModuleHandle("setupapi.dll");
    DEVPROPERTY property=module ? (DEVPROPERTY)GetProcAddress(module,"SetupDiGetDevicePropertyW") : NULL;
    WCHAR text[256];
    DWORD type=0;
    if (!lstrcmp(location,"CDC ACM echo - SIMULATED device")) return TRUE;
    ZeroMemory(text,sizeof(text));
    if (!property || !property(set,dev,&busDescription,&type,(PBYTE)text,sizeof(text),NULL,0)
        || type != 0x12) return FALSE; /* DEVPROP_TYPE_STRING */
    text[255]=0;
    return !lstrcmpW(text,L"CDC ACM echo - SIMULATED device");
}

static void Echo(HANDLE handle)
{
    static const DWORD lengths[]={1,63,64,65,128,1537};
    unsigned char sent[1537],received[1537];
    DCB dcb;
    COMMTIMEOUTS timeouts;
    DWORD i,j,written,read,total,started,remaining,calls,error,empty;
    char line[160];
    ZeroMemory(&dcb,sizeof(dcb)); dcb.DCBlength=sizeof(dcb);
    if (!GetCommState(handle,&dcb)) { Append("SIM echo: GetCommState failed\r\n"); return; }
    dcb.BaudRate=115200; dcb.ByteSize=8; dcb.Parity=NOPARITY; dcb.StopBits=ONESTOPBIT;
    dcb.fBinary=TRUE; dcb.fParity=FALSE; dcb.fOutxCtsFlow=FALSE; dcb.fOutxDsrFlow=FALSE;
    dcb.fDtrControl=DTR_CONTROL_ENABLE; dcb.fRtsControl=RTS_CONTROL_ENABLE;
    dcb.fOutX=FALSE; dcb.fInX=FALSE; dcb.fDsrSensitivity=FALSE;
    ZeroMemory(&timeouts,sizeof(timeouts));
    timeouts.ReadIntervalTimeout=50; timeouts.ReadTotalTimeoutConstant=3000;
    timeouts.WriteTotalTimeoutConstant=3000;
    if (!SetCommState(handle,&dcb) || !SetCommTimeouts(handle,&timeouts)) {
        wsprintf(line,"SIM echo configuration failed: %lu\r\n",GetLastError()); Append(line); return;
    }
    for(i=0;i<sizeof(lengths)/sizeof(lengths[0]);i++) {
        for(j=0;j<lengths[i];j++) sent[j]=(unsigned char)((j*37+i*11)&255);
        written=0;
        if (!WriteFile(handle,sent,lengths[i],&written,NULL) || written != lengths[i]) {
            wsprintf(line,"SIM write %lu failed: transferred=%lu error=%lu\r\n",
                lengths[i],written,GetLastError()); Append(line); return;
        }
        total=0; calls=0; error=0; empty=0; started=GetTickCount();
        while(total<lengths[i] && calls++<4096) {
            remaining=GetTickCount()-started;
            if(remaining>=3000) break;
            /* Keep one payload deadline even after successful empty reads.
             * usbredir's packet length does not reveal the parent URB size. */
            timeouts.ReadTotalTimeoutConstant=3000-remaining;
            if(!SetCommTimeouts(handle,&timeouts)) { error=GetLastError(); break; }
            read=0;
            if (!ReadFile(handle,received+total,lengths[i]-total,&read,NULL)) {
                error=GetLastError(); break;
            }
            if(read>lengths[i]-total) { error=ERROR_INVALID_DATA; break; }
            if(GetTickCount()-started>=3000) break;
            if(!read) { empty++; Sleep(1); continue; }
            total+=read;
        }
        if(total != lengths[i]) {
            wsprintf(line,"SIM read %lu incomplete: got=%lu error=%lu empty=%lu calls=%lu elapsed=%lu ms\r\n",
                lengths[i],total,error,empty,calls,GetTickCount()-started);
            Append(line); return;
        }
        for(j=0;j<total;j++) if(sent[j] != received[j]) {
            wsprintf(line,"SIM echo mismatch length=%lu offset=%lu\r\n",total,j); Append(line); return;
        }
        wsprintf(line,"SIM echo PASS %lu bytes (empty reads=%lu elapsed=%lu ms)\r\n",
            total,empty,GetTickCount()-started); Append(line);
    }
}

static void Append(const char *text)
{
    if (lstrlen(report) + lstrlen(text) < (int)sizeof(report)) lstrcat(report,text);
}

static void Save(void)
{
    DWORD n;
    HANDLE file = CreateFile("A:\\CDCLOG.TXT",GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);
    if (file != INVALID_HANDLE_VALUE) {
        WriteFile(file,report,lstrlen(report),&n,NULL);
        CloseHandle(file);
    }
}

static void StringValue(HKEY key,LPCSTR name,char *value,DWORD capacity)
{
    DWORD type,size = capacity;
    value[0] = 0;
    if (RegQueryValueEx(key,name,NULL,&type,(PBYTE)value,&size) != ERROR_SUCCESS ||
        type != REG_SZ) value[0] = 0;
    value[capacity-1] = 0;
}

static void Collect(void)
{
    HDEVINFO set;
    SP_DEVINFO_DATA dev;
    DWORD i,type,size,flags,problem;
    char ids[1024],description[256],service[256],location[256],port[64],path[80],line[1024];
    char provider[256],inf[128],section[128];
    HKEY key;
    HMODULE cfg = LoadLibrary("cfgmgr32.dll");
    DEVSTATUS status = cfg ? (DEVSTATUS)GetProcAddress(cfg,"CM_Get_DevNode_Status") : NULL;
    HANDLE handle;
    DCB dcb;
    report[0] = 0;
    Append("CDC report: X4 binding/open; identified SIMULATED device gets generated echo\r\n"
           "No physical board echo/reset/bootloader claim.\r\n\r\n");
    set = SetupDiGetClassDevs(&ports,NULL,NULL,DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) {
        wsprintf(line,"Ports enumeration failed: %lu\r\n",GetLastError()); Append(line);
    } else {
        for (i=0;i<128;i++) {
            ZeroMemory(&dev,sizeof(dev)); dev.cbSize=sizeof(dev);
            if (!SetupDiEnumDeviceInfo(set,i,&dev)) break;
            ZeroMemory(ids,sizeof(ids));
            if (!SetupDiGetDeviceRegistryProperty(set,&dev,SPDRP_HARDWAREID,&type,
                    (PBYTE)ids,sizeof(ids)-2,&size)) continue;
            {
                char *p=ids;
                BOOL match=FALSE;
                while (*p) {
                    if (!lstrcmpi(p,"USB\\VID_041E&PID_3278&MI_01") ||
                        !lstrcmpi(p,"USB\\VID_2341&PID_0043")) match=TRUE;
                    p += lstrlen(p)+1;
                }
                if (!match) continue;
            }
            description[0]=service[0]=location[0]=0;
            SetupDiGetDeviceRegistryProperty(set,&dev,SPDRP_DEVICEDESC,&type,
                (PBYTE)description,sizeof(description)-1,NULL);
            SetupDiGetDeviceRegistryProperty(set,&dev,SPDRP_SERVICE,&type,
                (PBYTE)service,sizeof(service)-1,NULL);
            SetupDiGetDeviceRegistryProperty(set,&dev,SPDRP_LOCATION_INFORMATION,&type,
                (PBYTE)location,sizeof(location)-1,NULL);
            description[255]=service[255]=location[255]=0;
            wsprintf(line,"Device: %s\r\nService: %s\r\nLocation: %s\r\n",description,service,location); Append(line);
            flags=problem=0;
            if (status && !status(&flags,&problem,dev.DevInst,0)) {
                wsprintf(line,"Devnode flags=%08lX problem=%lu\r\n",flags,problem); Append(line);
            }
            key=SetupDiOpenDevRegKey(set,&dev,DICS_FLAG_GLOBAL,0,DIREG_DRV,KEY_QUERY_VALUE);
            if (key != INVALID_HANDLE_VALUE) {
                StringValue(key,"ProviderName",provider,sizeof(provider));
                StringValue(key,"InfPath",inf,sizeof(inf));
                StringValue(key,"InfSection",section,sizeof(section));
                wsprintf(line,"Provider=%s INF=%s section=%s\r\n",provider,inf,section); Append(line);
                RegCloseKey(key);
            }
            port[0]=0;
            key=SetupDiOpenDevRegKey(set,&dev,DICS_FLAG_GLOBAL,0,DIREG_DEV,KEY_QUERY_VALUE);
            if (key != INVALID_HANDLE_VALUE) {
                StringValue(key,"PortName",port,sizeof(port)); RegCloseKey(key);
            }
            wsprintf(line,"PortName=%s\r\n",port); Append(line);
            /* Only an exact COMn name from this function is opened. */
            if (port[0]=='C' && port[1]=='O' && port[2]=='M' && port[3]>='0' && port[3]<='9') {
                unsigned int k=3;
                while (port[k]>='0' && port[k]<='9') k++;
                if (port[k]) continue;
                wsprintf(path,"\\\\.\\%s",port);
                handle=CreateFile(path,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,0,NULL);
                if (handle == INVALID_HANDLE_VALUE) {
                    wsprintf(line,"CreateFile failed: %lu\r\n",GetLastError()); Append(line);
                } else {
                    ZeroMemory(&dcb,sizeof(dcb)); dcb.DCBlength=sizeof(dcb);
                    if (GetCommState(handle,&dcb)) {
                        wsprintf(line,"Open/GetCommState passed: baud=%lu bytes=%u parity=%u stop=%u\r\n",
                            dcb.BaudRate,dcb.ByteSize,dcb.Parity,dcb.StopBits); Append(line);
                    } else {
                        wsprintf(line,"GetCommState failed: %lu\r\n",GetLastError()); Append(line);
                    }
                    if (Simulator(set,&dev,location)) {
                        Append("SIM identity confirmed by USB product property.\r\n");
                        Echo(handle);
                    }
                    CloseHandle(handle); Append("Port closed.\r\n");
                }
            }
        }
        SetupDiDestroyDeviceInfoList(set);
    }
    if (cfg) FreeLibrary(cfg);
    Save();
}

static LRESULT CALLBACK Proc(HWND window,UINT message,WPARAM w,LPARAM l)
{
    if (message==WM_SIZE && output) MoveWindow(output,0,34,LOWORD(l),HIWORD(l)-34,TRUE);
    if (message==WM_COMMAND && (LOWORD(w)==1 || LOWORD(w)==3)) {
        HMODULE lib=LoadLibrary("newdev.dll");
        UPDATEDRIVER update=lib ? (UPDATEDRIVER)GetProcAddress(lib,"UpdateDriverForPlugAndPlayDevicesA") : NULL;
        BOOL reboot=FALSE,ok=FALSE;
        DWORD error=ERROR_CALL_NOT_IMPLEMENTED;
        char line[160];
        if (update) {
            ok=update(window,LOWORD(w)==1 ? "USB\\VID_041E&PID_3278&MI_01" : "USB\\VID_2341&PID_0043",
                "A:\\USBCDC.INF",1,&reboot);
            error=ok ? 0 : GetLastError();
        }
        if (lib) FreeLibrary(lib);
        Collect();
        wsprintf(line,"\r\nCDC update ok=%u error=%08lX reboot=%u\r\n",ok,error,reboot);
        Append(line); Save(); SetWindowText(output,report); return 0;
    }
    if (message==WM_COMMAND && LOWORD(w)==2) { Collect(); SetWindowText(output,report); return 0; }
    if (message==WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProc(window,message,w,l);
}

int WINAPI WinMain(HINSTANCE instance,HINSTANCE previous,LPSTR command,int show)
{
    WNDCLASS wc;
    HWND window,install;
    MSG message;
    (void)previous; (void)command;
    ZeroMemory(&wc,sizeof(wc)); wc.lpfnWndProc=Proc; wc.hInstance=instance;
    wc.lpszClassName="CdcProbe"; wc.hCursor=LoadCursor(NULL,IDC_ARROW);
    if (!RegisterClass(&wc)) return 1;
    Collect();
    window=CreateWindow("CdcProbe","CDC binding/open probe - A:\\CDCLOG.TXT",WS_OVERLAPPEDWINDOW,
        20,20,920,600,NULL,NULL,instance,NULL);
    install=CreateWindow("BUTTON","Install X4 MI_01 from A:",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,
        0,0,260,30,window,(HMENU)1,instance,NULL);
    CreateWindow("BUTTON","Refresh report",WS_CHILD|WS_VISIBLE|WS_TABSTOP,265,0,180,30,
        window,(HMENU)2,instance,NULL);
    CreateWindow("BUTTON","Install simulated CDC from A:",WS_CHILD|WS_VISIBLE|WS_TABSTOP,450,0,270,30,
        window,(HMENU)3,instance,NULL);
    output=CreateWindow("EDIT",report,WS_CHILD|WS_VISIBLE|WS_VSCROLL|WS_HSCROLL|ES_MULTILINE|ES_READONLY,
        0,34,900,526,window,NULL,instance,NULL);
    SendMessage(output,WM_SETFONT,(WPARAM)GetStockObject(ANSI_FIXED_FONT),TRUE);
    ShowWindow(window,show); UpdateWindow(window); SetFocus(install);
    while (GetMessage(&message,NULL,0,0)>0) {
        if (!IsDialogMessage(window,&message)) { TranslateMessage(&message); DispatchMessage(&message); }
    }
    return 0;
}
