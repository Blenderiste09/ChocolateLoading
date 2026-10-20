// stackdump: prints EIP and a heuristic call stack (return addresses found on the stack that point
// into executable sections) for threads of a running 32-bit process. Build as x86.
//   stackdump.exe <pid> [thread_id|all] [stack_dwords]
// Addresses are printed as module+rva so they can be looked up in the Ghidra projects
// (analysis address = 0x10000000 + rva for the game DLLs).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>

struct Mod { uintptr_t base, end; std::string name; std::vector<std::pair<uintptr_t, uintptr_t>> code; };

static std::vector<Mod> load_modules(HANDLE proc) {
    std::vector<Mod> mods;
    HMODULE hm[1024]; DWORD need = 0;
    if (!EnumProcessModulesEx(proc, hm, sizeof(hm), &need, LIST_MODULES_32BIT)) return mods;
    for (DWORD i = 0; i < need / sizeof(HMODULE); i++) {
        MODULEINFO mi; char name[MAX_PATH];
        if (!GetModuleInformation(proc, hm[i], &mi, sizeof(mi))) continue;
        GetModuleBaseNameA(proc, hm[i], name, MAX_PATH);
        Mod m; m.base = (uintptr_t)mi.lpBaseOfDll; m.end = m.base + mi.SizeOfImage; m.name = name;
        IMAGE_DOS_HEADER dos; IMAGE_NT_HEADERS32 nt; SIZE_T rd;
        if (ReadProcessMemory(proc, (void*)m.base, &dos, sizeof(dos), &rd) &&
            ReadProcessMemory(proc, (void*)(m.base + dos.e_lfanew), &nt, sizeof(nt), &rd)) {
            std::vector<IMAGE_SECTION_HEADER> sec(nt.FileHeader.NumberOfSections);
            uintptr_t so = m.base + dos.e_lfanew + offsetof(IMAGE_NT_HEADERS32, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
            ReadProcessMemory(proc, (void*)so, sec.data(), sec.size() * sizeof(IMAGE_SECTION_HEADER), &rd);
            for (auto& s : sec)
                if (s.Characteristics & IMAGE_SCN_MEM_EXECUTE)
                    m.code.push_back({ m.base + s.VirtualAddress, m.base + s.VirtualAddress + s.Misc.VirtualSize });
        }
        mods.push_back(m);
    }
    return mods;
}

static const Mod* find(const std::vector<Mod>& mods, uintptr_t a, bool code_only) {
    for (auto& m : mods) {
        if (a < m.base || a >= m.end) continue;
        if (!code_only) return &m;
        for (auto& c : m.code) if (a >= c.first && a < c.second) return &m;
        return nullptr;
    }
    return nullptr;
}

static void fmt(const std::vector<Mod>& mods, uintptr_t a, char* out, size_t n) {
    const Mod* m = find(mods, a, false);
    if (m) snprintf(out, n, "%s+0x%x", m->name.c_str(), (unsigned)(a - m->base));
    else snprintf(out, n, "0x%08x", (unsigned)a);
}

// A plausible return address is preceded by a CALL instruction.
static bool after_call(HANDLE proc, uintptr_t ra) {
    unsigned char b[8]; SIZE_T rd;
    if (!ReadProcessMemory(proc, (void*)(ra - 7), b, 7, &rd)) return false;
    if (b[2] == 0xE8) return true;                         // call rel32 (5 bytes)
    if (b[5] == 0xFF && (b[6] & 0x38) == 0x10) return true; // call r/m (2 bytes)
    if (b[4] == 0xFF && (b[5] & 0x38) == 0x10) return true; // call [r+disp8] (3 bytes)
    if (b[1] == 0xFF && (b[2] & 0x38) == 0x10) return true; // call [r+disp32] (6 bytes)
    if (b[0] == 0xFF && (b[1] & 0x38) == 0x10) return true; // call [disp32]/sib (7 bytes)
    return false;
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: stackdump <pid> [tid|all] [stack_dwords]\n"); return 1; }
    DWORD pid = strtoul(argv[1], nullptr, 0);
    DWORD only = (argc >= 3 && strcmp(argv[2], "all") != 0) ? strtoul(argv[2], nullptr, 0) : 0;
    int depth = argc >= 4 ? atoi(argv[3]) : 4096;
    HANDLE proc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc) { printf("OpenProcess failed %lu\n", GetLastError()); return 1; }
    std::vector<Mod> mods = load_modules(proc);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) { printf("CreateToolhelp32Snapshot failed (%lu)\n", GetLastError()); CloseHandle(proc); return 1; }
    THREADENTRY32 te; te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        if (only && te.th32ThreadID != only) continue;
        HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!th) continue;
        SuspendThread(th);
        CONTEXT ctx; memset(&ctx, 0, sizeof(ctx)); ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(th, &ctx)) {
            char e[128]; fmt(mods, ctx.Eip, e, sizeof(e));
            FILETIME c, x, k, u; GetThreadTimes(th, &c, &x, &k, &u);
            double cpu = (((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime) + ((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime)) / 1e7;
            printf("=== thread %lu  cpu %.1fs  EIP %s  ESP %08lx EBP %08lx EAX %08lx ECX %08lx EDX %08lx ESI %08lx EDI %08lx\n",
                   te.th32ThreadID, cpu, e, ctx.Esp, ctx.Ebp, ctx.Eax, ctx.Ecx, ctx.Edx, ctx.Esi, ctx.Edi);
            std::vector<uint32_t> stk(depth);
            SIZE_T rd = 0;
            ReadProcessMemory(proc, (void*)ctx.Esp, stk.data(), depth * 4, &rd);
            int shown = 0;
            for (size_t i = 0; i < rd / 4 && shown < 40; i++) {
                uintptr_t v = stk[i];
                if (!find(mods, v, true) || !after_call(proc, v)) continue;
                char f[128]; fmt(mods, v, f, sizeof(f));
                printf("   [esp+%04x] %s\n", (unsigned)(i * 4), f);
                shown++;
            }
        }
        ResumeThread(th);
        CloseHandle(th);
    }
    CloseHandle(snap);
    CloseHandle(proc);
    return 0;
}
