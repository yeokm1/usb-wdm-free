/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (C) 2026 Yeo Kheng Meng
 * Original VfW preview/check utility from documented AVICap APIs and vfw.h.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vfw.h>

enum { SelectCamera=100, Refresh, Preview, Stop, Format, Source, Capture=200 };
static HWND mainWindow, cameras, capture, statusWindow;
static int connected, cameraFault;
static volatile LONG frames, lastBytes;
static CRITICAL_SECTION errorLock;
static char cameraError[256];

static void Status(const char *text)
{
    SetWindowText(statusWindow, text);
}

static LRESULT CALLBACK CameraError(HWND window, int id, LPCSTR description)
{
    (void)window;
    if (!id) return (LRESULT)TRUE; /* AVICap clears the prior error with ID0. */
    EnterCriticalSection(&errorLock);
    wsprintf(cameraError, "Camera error %d: ", id);
    lstrcpyn(cameraError+lstrlen(cameraError), description ? description : "",
        sizeof(cameraError)-lstrlen(cameraError));
    LeaveCriticalSection(&errorLock);
    PostMessage(mainWindow, WM_APP+1, 0, 0);
    return (LRESULT)TRUE;
}

static LRESULT CALLBACK Frame(HWND window, LPVIDEOHDR header)
{
    (void)window;
    if (header && header->lpData && header->dwBytesUsed) {
        InterlockedIncrement((LONG *)&frames);
        InterlockedExchange((LONG *)&lastBytes, (LONG)header->dwBytesUsed);
    }
    return (LRESULT)TRUE;
}

static void Buttons(void)
{
    CAPDRIVERCAPS caps;
    ZeroMemory(&caps, sizeof(caps));
    if (connected) capDriverGetCaps(capture, &caps, sizeof(caps));
    EnableWindow(cameras, !connected);
    EnableWindow(GetDlgItem(mainWindow, Refresh), !connected);
    EnableWindow(GetDlgItem(mainWindow, Preview), capture && (connected || SendMessage(cameras, CB_GETCOUNT, 0, 0)>0));
    EnableWindow(GetDlgItem(mainWindow, Stop), connected);
    EnableWindow(GetDlgItem(mainWindow, Format), connected && caps.fHasDlgVideoFormat);
    EnableWindow(GetDlgItem(mainWindow, Source), connected && caps.fHasDlgVideoSource);
}

static void Enumerate(void)
{
    UINT i;
    char name[128], version[128], line[272];
    SendMessage(cameras, CB_RESETCONTENT, 0, 0);
    for (i=0; i<10; i++) {
        if (capGetDriverDescription(i, name, sizeof(name), version, sizeof(version))) {
            LRESULT row;
            wsprintf(line, "%u: %s (%s)", i, name, version);
            row=SendMessage(cameras, CB_ADDSTRING, 0, (LPARAM)line);
            if (row!=CB_ERR && row!=CB_ERRSPACE)
                SendMessage(cameras, CB_SETITEMDATA, (WPARAM)row, i);
        }
    }
    if (SendMessage(cameras, CB_GETCOUNT, 0, 0)>0) {
        SendMessage(cameras, CB_SETCURSEL, 0, 0);
        Status("Select a camera and press Preview. Live images appear below.");
    } else Status("No VfW capture device found. Install the camera driver, then Refresh.");
    Buttons();
}

static void Describe(void)
{
    DWORD size, read;
    BITMAPINFOHEADER *format;
    CAPSTATUS status;
    char line[384], code[5];
    unsigned int i;
    if (!connected) return;
    size=capGetVideoFormatSize(capture);
    if (size<sizeof(BITMAPINFOHEADER) || size>1024UL*1024) {
        Status("Connected; camera returned an unsupported format description."); return;
    }
    format=HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
    if (!format) { Status("Connected; unable to allocate format description."); return; }
    read=capGetVideoFormat(capture, format, size);
    if (read<sizeof(*format) || read>size || format->biSize<sizeof(*format) || format->biSize>read) {
        HeapFree(GetProcessHeap(), 0, format);
        Status("Connected; camera returned a truncated format description."); return;
    }
    for (i=0; i<4; i++) {
        unsigned char c=(unsigned char)(format->biCompression>>(i*8));
        code[i]=(c>=32 && c<127) ? (char)c : '.';
    }
    code[4]=0;
    ZeroMemory(&status, sizeof(status));
    capGetStatus(capture, &status, sizeof(status));
    wsprintf(line, "%ld x %ld, %u bits, %s (0x%08lX)\r\n"
        "Preview=%s; frames observed=%ld; last buffer=%ld bytes",
        format->biWidth, format->biHeight, format->biBitCount,
        format->biCompression==BI_RGB ? "RGB" : code, format->biCompression,
        status.fLiveWindow ? "on" : "off", frames, lastBytes);
    HeapFree(GetProcessHeap(), 0, format);
    Status(line);
}

static void Disconnect(void)
{
    if (!connected) return;
    capPreview(capture, FALSE);
    if (capDriverDisconnect(capture)) {
        connected=0; Status("Preview stopped; camera disconnected.");
    } else Status("Camera disconnect failed. Close CamCheck to release the capture window.");
    Buttons();
}

static void StartPreview(void)
{
    BOOL rate;
    rate=capPreviewRate(capture, 66);
    if (!capPreview(capture, TRUE)) {
        cameraFault=1;
        Status("Connected, but preview could not start. Choose Format or Source, then retry Preview.");
    } else {
        cameraFault=0;
        if (rate) Describe();
        else Status("Preview started using the driver's default preview interval.");
    }
}

static void Connect(void)
{
    LRESULT row, index;
    if (connected) { StartPreview(); return; }
    row=SendMessage(cameras, CB_GETCURSEL, 0, 0);
    if (row==CB_ERR) return;
    index=SendMessage(cameras, CB_GETITEMDATA, (WPARAM)row, 0);
    if (index==CB_ERR || index<0 || index>9) return;
    InterlockedExchange((LONG *)&frames, 0); InterlockedExchange((LONG *)&lastBytes, 0);
    if (!capDriverConnect(capture, (UINT)index)) {
        Status("Camera connection failed. Check that its driver is working and the camera is available."); return;
    }
    connected=1;
    capPreviewScale(capture, TRUE);
    StartPreview();
    Buttons();
}

static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w, LPARAM l)
{
    if (message==WM_COMMAND) {
        switch (LOWORD(w)) {
        case Refresh: if (!connected) Enumerate(); break;
        case Preview: Connect(); break;
        case Stop: Disconnect(); break;
        case Format:
            if (connected) { capDlgVideoFormat(capture); StartPreview(); }
            break;
        case Source:
            if (connected) { capDlgVideoSource(capture); StartPreview(); }
            break;
        }
        return 0;
    }
    if (message==WM_TIMER) { if (connected && !cameraFault) Describe(); return 0; }
    if (message==WM_APP+1) {
        char text[256];
        EnterCriticalSection(&errorLock); lstrcpy(text, cameraError); LeaveCriticalSection(&errorLock);
        cameraFault=1; Status(text); return 0;
    }
    if (message==WM_SIZE && capture) {
        int width=LOWORD(l), height=HIWORD(l);
        int i, buttonWidth=(width-48)/5;
        const int ids[5]={Preview, Stop, Refresh, Format, Source};
        if (buttonWidth<1) buttonWidth=1;
        MoveWindow(cameras, 8, 8, width>16 ? width-16 : 1, 240, TRUE);
        for (i=0; i<5; i++)
            MoveWindow(GetDlgItem(window, ids[i]), 8+i*(buttonWidth+8), 40, buttonWidth, 28, TRUE);
        MoveWindow(capture, 8, 76, width>16 ? width-16 : 1, height>160 ? height-160 : 1, TRUE);
        MoveWindow(statusWindow, 8, height>76 ? height-76 : 0, width>16 ? width-16 : 1, 68, TRUE);
        return 0;
    }
    if (message==WM_DESTROY) {
        KillTimer(window, 1);
        if (capture) {
            capSetCallbackOnFrame(capture, NULL); capSetCallbackOnError(capture, NULL);
            if (connected) { capPreview(capture, FALSE); capDriverDisconnect(capture); }
            DestroyWindow(capture); capture=NULL;
        }
        PostQuitMessage(0); return 0;
    }
    return DefWindowProc(window, message, w, l);
}

static HWND Button(HINSTANCE instance, const char *name, int id, int x, int width)
{
    return CreateWindow("BUTTON", name, WS_CHILD|WS_VISIBLE|WS_TABSTOP,
        x, 40, width, 28, mainWindow, (HMENU)id, instance, NULL);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous, LPSTR command, int show)
{
    WNDCLASS wc;
    MSG message;
    HWND preview;
    RECT client, work;
    int width, height;
    (void)previous; (void)command;
    InitializeCriticalSection(&errorLock);
    ZeroMemory(&wc, sizeof(wc)); wc.hInstance=instance; wc.lpfnWndProc=WindowProc;
    wc.lpszClassName="CamCheck"; wc.hCursor=LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);
    if (!RegisterClass(&wc)) { DeleteCriticalSection(&errorLock); return 1; }
    if (!SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0)) {
        work.left=work.top=0;
        work.right=GetSystemMetrics(SM_CXSCREEN); work.bottom=GetSystemMetrics(SM_CYSCREEN);
    }
    width=work.right-work.left-40; height=work.bottom-work.top-40;
    if (width>800) width=800;
    if (height>680) height=680;
    mainWindow=CreateWindow("CamCheck", "CamCheck", WS_OVERLAPPEDWINDOW,
        work.left+20, work.top+20, width, height, NULL, NULL, instance, NULL);
    if (!mainWindow) { DeleteCriticalSection(&errorLock); return 1; }
    cameras=CreateWindow("COMBOBOX", "", WS_CHILD|WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,
        8, 8, 760, 240, mainWindow, (HMENU)SelectCamera, instance, NULL);
    preview=Button(instance, "&Preview", Preview, 8, 120);
    Button(instance, "&Stop", Stop, 136, 120);
    Button(instance, "&Refresh", Refresh, 264, 120);
    Button(instance, "&Format...", Format, 392, 120);
    Button(instance, "S&ource...", Source, 520, 120);
    statusWindow=CreateWindow("STATIC", "", WS_CHILD|WS_VISIBLE,
        8, 560, 760, 68, mainWindow, NULL, instance, NULL);
    capture=capCreateCaptureWindow("Camera preview", WS_CHILD|WS_VISIBLE,
        8, 76, 760, 480, mainWindow, Capture);
    if (!capture) Status("Unable to create the AVICap preview window.");
    else {
        capSetCallbackOnError(capture, CameraError); capSetCallbackOnFrame(capture, Frame);
        Enumerate();
    }
    ShowWindow(mainWindow, show); UpdateWindow(mainWindow);
    GetClientRect(mainWindow, &client);
    SendMessage(mainWindow, WM_SIZE, 0, MAKELPARAM(client.right, client.bottom));
    SetFocus(preview); SetTimer(mainWindow, 1, 500, NULL);
    while (GetMessage(&message, NULL, 0, 0)>0) {
        if (!IsDialogMessage(mainWindow, &message)) { TranslateMessage(&message); DispatchMessage(&message); }
    }
    DeleteCriticalSection(&errorLock);
    return 0;
}
