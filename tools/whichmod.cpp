// whichmod: lists the 32-bit modules of a process, or names the module that contains an address.
//   whichmod.exe <pid> [hexaddr]
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: whichmod <pid> [hexaddr]\n"); return 1; }
    DWORD pid = strtoul(argv[1], nullptr, 0);
    uintptr_t addr = argc > 2 ? strtoul(argv[2], nullptr, 16) : 0;
    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc) { printf("OpenProcess failed (%lu)\n", GetLastError()); return 1; }
    HMODULE mods[2048];
    DWORD need = 0;
    if (!EnumProcessModulesEx(proc, mods, sizeof(mods), &need, LIST_MODULES_32BIT)) {
        printf("EnumProcessModulesEx failed (%lu)\n", GetLastError());
        CloseHandle(proc);
        return 1;
    }
    bool found = false;
    for (DWORD i = 0; i < need / sizeof(HMODULE); i++) {
        MODULEINFO mi;
        char name[MAX_PATH];
        if (!GetModuleInformation(proc, mods[i], &mi, sizeof(mi))) continue;
        GetModuleBaseNameA(proc, mods[i], name, MAX_PATH);
        uintptr_t base = (uintptr_t)mi.lpBaseOfDll, end = base + mi.SizeOfImage;
        if (!addr) {
            printf("%-28s base=0x%08zx end=0x%08zx size=0x%lx\n", name, base, end, mi.SizeOfImage);
        } else if (addr >= base && addr < end) {
            printf("%s  base=0x%08zx end=0x%08zx  (address at +0x%zx)\n", name, base, end, addr - base);
            found = true;
        }
    }
    if (addr && !found) printf("0x%08zx is not inside a module\n", addr);
    CloseHandle(proc);
    return 0;
}
