// ptrinfo: describes where an address of another 32-bit process lives (module, or private/mapped region
// with its allocation base), and optionally follows pointer fields of the structure at that address.
//   ptrinfo.exe <pid> <hexaddr> [hexoffset ...]
// Example: ptrinfo 1234 101c3c50 8 30   -> the struct, then the pointers stored at +0x8 and +0x30.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>

static void describe(HANDLE proc, uintptr_t a, const char* label) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQueryEx(proc, (void*)a, &mbi, sizeof(mbi))) { printf("%s 0x%08zx  <unknown>\n", label, a); return; }
    const char* state = mbi.State == MEM_COMMIT ? "committed" : mbi.State == MEM_RESERVE ? "reserved" : "free";
    const char* type = mbi.Type == MEM_IMAGE ? "image" : mbi.Type == MEM_MAPPED ? "mapped" : mbi.Type == MEM_PRIVATE ? "private" : "-";
    char module[MAX_PATH] = "";
    HMODULE mods[2048];
    DWORD need = 0;
    if (EnumProcessModulesEx(proc, mods, sizeof(mods), &need, LIST_MODULES_32BIT))
        for (DWORD i = 0; i < need / sizeof(HMODULE); i++) {
            MODULEINFO mi;
            uintptr_t b = GetModuleInformation(proc, mods[i], &mi, sizeof(mi)) ? (uintptr_t)mi.lpBaseOfDll : 0;
            if (b && a >= b && a < b + mi.SizeOfImage) { GetModuleBaseNameA(proc, mods[i], module, MAX_PATH); break; }
        }
    printf("%s 0x%08zx  %s %s, allocation base 0x%08zx, region %zu KB  %s\n", label, a, state, type,
           (uintptr_t)mbi.AllocationBase, mbi.RegionSize / 1024, module[0] ? module : "(not a module)");
}

int main(int argc, char** argv) {
    if (argc < 3) { printf("usage: ptrinfo <pid> <hexaddr> [hexoffset ...]\n"); return 1; }
    DWORD pid = strtoul(argv[1], nullptr, 0);
    uintptr_t addr = strtoul(argv[2], nullptr, 16);
    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc) { printf("OpenProcess failed (%lu)\n", GetLastError()); return 1; }
    describe(proc, addr, "address   ");
    for (int i = 3; i < argc; i++) {
        uintptr_t off = strtoul(argv[i], nullptr, 16), value = 0;
        if (!ReadProcessMemory(proc, (void*)(addr + off), &value, 4, nullptr)) { printf("  [+0x%zx] <unreadable>\n", off); continue; }
        printf("  [+0x%zx] = 0x%08zx\n", off, value);
        if (value) describe(proc, value, "    ->    ");
    }
    CloseHandle(proc);
    return 0;
}
