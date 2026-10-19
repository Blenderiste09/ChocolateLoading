// peek: dumps dwords of another 32-bit process, telling whether each value points to an image, a
// mapped or a private region.
//   peek.exe <pid> <hexaddr> [count]          (default 16)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    if (argc < 3) { printf("usage: peek <pid> <hexaddr> [count]\n"); return 1; }
    DWORD pid = strtoul(argv[1], nullptr, 0);
    uintptr_t addr = strtoul(argv[2], nullptr, 16);
    int count = argc > 3 ? atoi(argv[3]) : 16;
    HANDLE p = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!p) { printf("OpenProcess failed\n"); return 1; }
    for (int i = 0; i < count; i++) {
        DWORD v = 0; SIZE_T rd = 0;
        if (!ReadProcessMemory(p, (void*)(addr + i * 4), &v, 4, &rd)) { printf("%08x: <unreadable>\n", (unsigned)(addr + i * 4)); continue; }
        MEMORY_BASIC_INFORMATION mbi = {};
        const char* kind = "";
        if (v > 0x10000 && VirtualQueryEx(p, (void*)v, &mbi, sizeof(mbi)))
            kind = mbi.State != MEM_COMMIT ? "" : mbi.Type == MEM_IMAGE ? "image" : mbi.Type == MEM_MAPPED ? "mapped" : "private";
        printf("%08x: %08lx  %s\n", (unsigned)(addr + i * 4), v, kind);
    }
    CloseHandle(p);
    return 0;
}
