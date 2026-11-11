/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Yeo Kheng Meng
 *
 * NETSET.EXE - test aid for the network drivers' Advanced-tab keywords
 * (ethernet.md section 4.10): reads and writes values in a network
 * adapter's driver key, as the Advanced tab does, so a leg can change a
 * setting without driving the dialog. 98 SE to 7. Usage:
 *   NETSET <text>                     every adapter whose DriverDesc
 *                                     contains <text>: its key and values
 *   NETSET <text> <name> <value>      set <name> to the string <value>
 *   NETSET <text> <name> -            delete <name>
 *   NETSET <text> <name> ?            print <name>'s value, or "absent"
 * The keys are the Net class's: HKLM\System\CurrentControlSet\Services\
 * Class\Net\nnnn on 98 SE and ME, HKLM\SYSTEM\CurrentControlSet\Control\
 * Class\{4D36E972-E325-11CE-BFC1-08002BE10318}\nnnn on NT. A change takes
 * effect when the adapter restarts. Exit code 0 when at least one
 * adapter matched (and every write succeeded), 1 otherwise.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

static const char *Roots[] = {
    "SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E972-E325-11CE-BFC1-08002BE10318}",
    "System\\CurrentControlSet\\Services\\Class\\Net",
};

static int Contains(const char *hay, const char *needle)
{
    size_t n = strlen(needle), i;
    for (; *hay; hay++) {
        for (i = 0; i < n && hay[i]; i++) {
            char a = hay[i], b = needle[i];
            if (a >= 'a' && a <= 'z') a = (char)(a - 32);
            if (b >= 'a' && b <= 'z') b = (char)(b - 32);
            if (a != b) break;
        }
        if (i == n) return 1;
    }
    return 0;
}

static void List(HKEY key)
{
    char name[256];
    BYTE data[512];
    DWORD i, nlen, dlen, type;
    for (i = 0;; i++) {
        nlen = sizeof(name);
        dlen = sizeof(data) - 1;
        if (RegEnumValueA(key, i, name, &nlen, NULL, &type, data, &dlen) != ERROR_SUCCESS) break;
        if (type == REG_SZ) {
            data[dlen < sizeof(data) ? dlen : sizeof(data) - 1] = 0;
            printf("  %s = \"%s\"\n", name, (char *)data);
        } else if (type == REG_DWORD && dlen == 4) {
            printf("  %s = dword %lu\n", name, *(DWORD *)data);
        } else {
            printf("  %s = type %lu, %lu bytes\n", name, type, dlen);
        }
    }
}

int main(int argc, char **argv)
{
    int matched = 0, failed = 0;
    unsigned r;
    if (argc != 2 && argc != 4) {
        printf("usage: NETSET <text> [<name> <value>|-|?]\n");
        return 1;
    }
    for (r = 0; r < sizeof(Roots) / sizeof(Roots[0]); r++) {
        HKEY root;
        DWORD i;
        char sub[32];
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, Roots[r], 0, KEY_READ, &root) != ERROR_SUCCESS) continue;
        for (i = 0; RegEnumKeyA(root, i, sub, sizeof(sub)) == ERROR_SUCCESS; i++) {
            HKEY key;
            char desc[256];
            DWORD len = sizeof(desc) - 1, type;
            if (RegOpenKeyExA(root, sub, 0, KEY_READ | KEY_WRITE, &key) != ERROR_SUCCESS) continue;
            if (RegQueryValueExA(key, "DriverDesc", NULL, &type, (BYTE *)desc, &len) != ERROR_SUCCESS ||
                type != REG_SZ) {
                RegCloseKey(key);
                continue;
            }
            desc[len < sizeof(desc) ? len : sizeof(desc) - 1] = 0;
            if (!Contains(desc, argv[1])) { RegCloseKey(key); continue; }
            matched++;
            printf("NETSET %s\\%s: %s\n", Roots[r], sub, desc);
            if (argc == 2) {
                List(key);
            } else if (!strcmp(argv[3], "?")) {
                char v[256];
                DWORD vl = sizeof(v) - 1;
                if (RegQueryValueExA(key, argv[2], NULL, &type, (BYTE *)v, &vl) != ERROR_SUCCESS)
                    printf("  %s absent\n", argv[2]);
                else if (type == REG_SZ) {
                    v[vl < sizeof(v) ? vl : sizeof(v) - 1] = 0;
                    printf("  %s = \"%s\"\n", argv[2], v);
                } else if (type == REG_DWORD && vl == 4) {
                    printf("  %s = dword %lu\n", argv[2], *(DWORD *)v);
                } else {
                    printf("  %s = type %lu\n", argv[2], type);
                }
            } else if (!strcmp(argv[3], "-")) {
                LONG e = RegDeleteValueA(key, argv[2]);
                printf("  %s deleted: %ld\n", argv[2], e);
                if (e != ERROR_SUCCESS && e != ERROR_FILE_NOT_FOUND) failed++;
            } else {
                LONG e = RegSetValueExA(key, argv[2], 0, REG_SZ, (const BYTE *)argv[3],
                                        (DWORD)strlen(argv[3]) + 1);
                printf("  %s = \"%s\": %ld\n", argv[2], argv[3], e);
                if (e != ERROR_SUCCESS) failed++;
            }
            RegCloseKey(key);
        }
        RegCloseKey(root);
    }
    if (!matched) printf("NETSET: no adapter matches \"%s\"\n", argv[1]);
    return matched && !failed ? 0 : 1;
}
