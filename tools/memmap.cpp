// memmap: lists the committed private read/write regions of a process that are at least `minKB` large.
//   memmap.exe <pid> [minKB]          (default 512)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: memmap <pid> [minKB]\n"); return 1; }
    DWORD pid = strtoul(argv[1], nullptr, 0);
    SIZE_T min_bytes = (SIZE_T)(argc > 2 ? strtoul(argv[2], nullptr, 0) : 512) * 1024;
    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc) { printf("OpenProcess failed (%lu)\n", GetLastError()); return 1; }
    uintptr_t a = 0;
    SIZE_T total = 0;
    MEMORY_BASIC_INFORMATION mbi;
    while (VirtualQueryEx(proc, (void*)a, &mbi, sizeof(mbi))) {
        uintptr_t base = (uintptr_t)mbi.BaseAddress;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.RegionSize >= min_bytes &&
            (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_EXECUTE_READWRITE)) {
            printf("0x%08zx  %7zu KB  protect=0x%lx  allocation base 0x%08zx\n", base, mbi.RegionSize / 1024, mbi.Protect,
                   (uintptr_t)mbi.AllocationBase);
            total += mbi.RegionSize;
        }
        a = base + mbi.RegionSize;
        if (a <= base) break;   // wrapped around the address space
    }
    printf("total: %zu KB\n", total / 1024);
    CloseHandle(proc);
    return 0;
}
