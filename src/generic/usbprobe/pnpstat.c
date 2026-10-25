/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Read-only PnP metadata: default synthetic USB VID FFFF, or explicit VID:PID.
 * Published SetupAPI/Configuration Manager contracts; no binary inspection.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <string.h>
typedef DWORD (WINAPI *CMSTATUS)(PULONG,PULONG,DWORD,ULONG);
static char report[32768];
static void Append(const char *s) { if(lstrlen(report)+lstrlen(s)<sizeof(report)) lstrcat(report,s); }
static int Target(char *command, char *match)
{
    unsigned int i;
    while(*command==' ') command++;
    if(!*command) { lstrcpy(match,"VID_FFFF"); return 1; }
    if(lstrlen(command)!=9 || command[4]!=':') return 0;
    lstrcpy(match,"VID_0000&PID_0000");
    for(i=0;i<9;i++) if(i!=4) {
        char c=command[i];
        if(c>='a'&&c<='f') c=(char)(c-'a'+'A');
        if(!((c>='0'&&c<='9')||(c>='A'&&c<='F'))) return 0;
        match[i<4 ? i+4 : i+8]=c;
    }
    return 1;
}
int WINAPI WinMain(HINSTANCE inst,HINSTANCE old,LPSTR cmd,int show)
{
    HDEVINFO set;SP_DEVINFO_DATA dev;DWORD i,type,flags,problem,cr,bytes;
    char ids[4096],upper[4096],match[20],value[1024],line[1200];HANDLE file;HMODULE cfg;
    CMSTATUS status;
    (void)inst;(void)old;(void)show;
    if(!Target(cmd,match)) return 4;
    cfg=LoadLibrary("cfgmgr32.dll");status=cfg?(CMSTATUS)GetProcAddress(cfg,"CM_Get_DevNode_Status"):NULL;
    set=SetupDiGetClassDevs(NULL,NULL,NULL,DIGCF_ALLCLASSES|DIGCF_PRESENT);
    if(set==INVALID_HANDLE_VALUE) return 1;
    for(i=0;;i++) {
        DWORD prop;
        ZeroMemory(&dev,sizeof(dev));dev.cbSize=sizeof(dev);
        if(!SetupDiEnumDeviceInfo(set,i,&dev)) break;
        ZeroMemory(ids,sizeof(ids));
        if(!SetupDiGetDeviceRegistryProperty(set,&dev,SPDRP_HARDWAREID,&type,(PBYTE)ids,sizeof(ids)-2,NULL)) continue;
        {
            DWORD n;
            char *found;
            lstrcpy(upper,ids);
            for(n=0;upper[n];n++) if(upper[n]>='a'&&upper[n]<='z') upper[n]=(char)(upper[n]-'a'+'A');
            found=strstr(upper,match);
            if(!found || (found[lstrlen(match)] && found[lstrlen(match)]!='&')) continue;
        }
        Append(ids);Append("\r\n");
        for(prop=0;prop<3;prop++) {
            DWORD properties[3]={SPDRP_DEVICEDESC,SPDRP_SERVICE,SPDRP_DRIVER};
            ZeroMemory(value,sizeof(value));
            if(SetupDiGetDeviceRegistryProperty(set,&dev,properties[prop],&type,(PBYTE)value,sizeof(value)-1,NULL)) {
                wsprintf(line,"  property%lu=%s\r\n",properties[prop],value);Append(line);
            }
        }
        flags=problem=0;cr=status?status(&flags,&problem,dev.DevInst,0):~0UL;
        wsprintf(line,"  CM=%lu flags=%08lX problem=%lu\r\n",cr,flags,problem);Append(line);
    }
    SetupDiDestroyDeviceInfoList(set);if(cfg) FreeLibrary(cfg);
    if(!report[0]) { Append("No present device matching ");Append(match);Append(".\r\n"); }
    file=CreateFile("A:\\PNPSTAT.TXT",GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(file==INVALID_HANDLE_VALUE) return 2;
    i=WriteFile(file,report,lstrlen(report),&bytes,NULL)&&bytes==(DWORD)lstrlen(report);
    CloseHandle(file);return i?0:3;
}
