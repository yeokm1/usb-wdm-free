/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original bounded read-only removable-file probe. Report writes A:FREAD.TXT
 * on the explicitly staged virtual floppy. No physical disk write API.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
static char report[30000];
static unsigned int used, files, dirs, failures;
static DWORD total;
static HWND view;
static void Log(const char *fmt,...)
{
    char line[512]; va_list ap; unsigned int n;
    va_start(ap,fmt); _vsnprintf(line,sizeof(line)-1,fmt,ap); va_end(ap);
    line[sizeof(line)-1]=0; n=(unsigned int)strlen(line);
    if(n < sizeof(report)-used) { memcpy(report+used,line,n+1); used+=n; }
    else ++failures;
    if(view) { SetWindowTextA(view,report); UpdateWindow(view); }
}
static void Walk(const char *path,unsigned int depth)
{
    WIN32_FIND_DATAA f; HANDLE search,h; char pattern[MAX_PATH],name[MAX_PATH];
    unsigned char buffer[4096]; DWORD got,crc,count,err; unsigned int i,k;
    if(depth>8 || ++dirs>64) { Log("LIMIT directory depth/count\r\n"); ++failures; return; }
    if(strlen(path)+4>=MAX_PATH) { ++failures; return; }
    Log("LIST %s (read-only)\r\n",path);
    wsprintfA(pattern,"%s\\*.*",path); SetLastError(0); search=FindFirstFileA(pattern,&f);
    if(search==INVALID_HANDLE_VALUE) {
        err=GetLastError(); Log("DIR %s error=%lu\r\n",path,err);
        if(err!=ERROR_FILE_NOT_FOUND) ++failures; return;
    }
    do {
        if(files==3) { Log("SCOPE: first 3 files only; remaining files not tested\r\n"); break; }
        if(!strcmp(f.cFileName,".") || !strcmp(f.cFileName,"..")) continue;
        if(strlen(path)+strlen(f.cFileName)+2>=MAX_PATH) { ++failures; continue; }
        wsprintfA(name,"%s\\%s",path,f.cFileName);
        if(f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { Walk(name,depth+1); continue; }
        if(++files>256 || f.nFileSizeHigh || f.nFileSizeLow>4UL*1024*1024 || total>8UL*1024*1024) {
            Log("LIMIT file count/size\r\n"); ++failures; break;
        }
        h=CreateFileA(name,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,NULL);
        if(h==INVALID_HANDLE_VALUE) { Log("READ %s open error=%lu\r\n",name,GetLastError()); ++failures; continue; }
        crc=0xffffffffUL; count=0; err=0;
        for(;;) {
            if(!ReadFile(h,buffer,sizeof(buffer),&got,NULL)) { err=GetLastError(); break; }
            if(!got) break;
            if(got>4UL*1024*1024-count || got>8UL*1024*1024-total) { err=ERROR_BUFFER_OVERFLOW; break; }
            count+=got; total+=got;
            for(i=0;i<got;++i) { crc^=buffer[i]; for(k=0;k<8;++k) crc=(crc>>1)^((crc&1)?0xedb88320UL:0); }
        }
        CloseHandle(h);
        if(err || count!=f.nFileSizeLow) ++failures;
        Log("READ %s bytes=%lu expected=%lu CRC32=%08lX error=%lu %s\r\n",name,count,f.nFileSizeLow,crc^0xffffffffUL,err,
            !err && count==f.nFileSizeLow?"PASS":"FAIL");
    } while(FindNextFileA(search,&f));
    err=GetLastError(); FindClose(search);
    if(err!=ERROR_NO_MORE_FILES && files<3) { Log("ENUM error=%lu\r\n",err); ++failures; }
}
int WINAPI WinMain(HINSTANCE inst,HINSTANCE prev,LPSTR cmd,int show)
{
    OSVERSIONINFOA os; HANDLE h; DWORD written,mask,type; HWND w,edit; MSG msg;
    char root[4]="B:\\",base[3]="B:"; unsigned int i;
    (void)prev; (void)show;
    /* Optional explicit single drive letter; never A: (virtual report). */
    if(cmd && cmd[0] && !cmd[1] && ((cmd[0]>='B' && cmd[0]<='Z') || (cmd[0]>='b' && cmd[0]<='z')))
        root[0]=base[0]=(char)(cmd[0]>='a'?cmd[0]-'a'+'A':cmd[0]);
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOOPENFILEERRORBOX);
    memset(&os,0,sizeof(os)); os.dwOSVersionInfoSize=sizeof(os); GetVersionExA(&os);
    Log("USBFLOP read-only media probe\r\nWindows %lu.%lu build %lu platform %lu\r\n",os.dwMajorVersion,os.dwMinorVersion,os.dwBuildNumber&0xffffUL,os.dwPlatformId);
    Log("Physical source %s READ ONLY; report destination virtual A:\\FREAD.TXT\r\nNo file contents displayed; CRC32 is a data checksum.\r\n",base);
    mask=GetLogicalDrives();
    for(i=0;i<26;++i) if(mask&(1UL<<i)) {
        char drive[4]="A:\\"; drive[0]=(char)('A'+i); type=GetDriveTypeA(drive);
        Log("DRIVE %s type=%lu (2=removable)\r\n",drive,type);
    }
    w=CreateWindowA("STATIC","USBFLOP read-only result (A: virtual report)",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
        0,0,630,445,NULL,NULL,inst,NULL);
    if(!w) return 1;
    edit=CreateWindowA("EDIT",report,WS_CHILD|WS_VISIBLE|WS_VSCROLL|WS_HSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOHSCROLL,
        3,3,610,390,w,NULL,inst,NULL);
    SendMessage(edit,WM_SETFONT,(WPARAM)GetStockObject(ANSI_FIXED_FONT),0); view=edit;
    ShowWindow(w,SW_SHOW); UpdateWindow(w); UpdateWindow(edit);
    if(GetDriveTypeA(root)!=DRIVE_REMOVABLE) { Log("REFUSED: %s not removable\r\n",base); ++failures; }
    else Walk(base,0);
    Log("RESULT files=%u dirs=%u bytes=%lu failures=%u %s\r\n",files,dirs,total,failures,
        !failures && files && total?"READ PASS":"NO FUNCTIONAL PASS");
    h=CreateFileA("A:\\FREAD.TXT",GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
    if(h!=INVALID_HANDLE_VALUE) { WriteFile(h,report,used,&written,NULL); CloseHandle(h); }
    while(IsWindow(w) && GetMessage(&msg,NULL,0,0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    return failures?1:0;
}
