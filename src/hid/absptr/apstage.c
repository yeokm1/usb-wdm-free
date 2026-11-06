/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * APSTAGE.EXE - test helper for absptr's 2000 and XP legs (docs/design/
 * absptr.md, test plan): stages an INF with SetupCopyOEMInf, so that the
 * next plug of a matching device finds it the way Windows finds any OEM
 * INF. Writes the result to COM1 and to the console.
 *
 *   APSTAGE <path\to\absptr2k.inf>
 *
 * 2000 and XP only (setupapi.dll). Original code.
 */
#include <windows.h>
#include <setupapi.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    char dest[MAX_PATH], line[MAX_PATH + 64];
    HANDLE com;
    DWORD n;
    BOOL ok;

    if (argc != 2) {
        printf("usage: APSTAGE <inf>\n");
        return 2;
    }
    dest[0] = 0;
    ok = SetupCopyOEMInf(argv[1], NULL, SPOST_PATH, 0, dest, sizeof(dest), NULL, NULL);
    if (ok)
        sprintf(line, "APSTAGE staged %s as %s\r\n", argv[1], dest);
    else
        sprintf(line, "APSTAGE failed %lu for %s\r\n", GetLastError(), argv[1]);
    fputs(line, stdout);
    com = CreateFile("COM1", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (com != INVALID_HANDLE_VALUE) {
        WriteFile(com, line, (DWORD)strlen(line), &n, NULL);
        CloseHandle(com);
    }
    return ok ? 0 : 1;
}
