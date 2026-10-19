#define _CRT_SECURE_NO_WARNINGS
#include "callsite.h"
#include <windows.h>
#include <string.h>

extern "C" IMAGE_DOS_HEADER __ImageBase;   // this DLL's own base address

namespace callsite {

namespace {

enum Cls { C_PASS = 1, C_SELF, C_GAME, C_OTHER };

// Cache of classified modules (append-only; an entry becomes visible once `ready` is set).
struct ModEnt { uintptr_t base; int cls; char name[24]; volatile LONG ready; };
ModEnt        g_mc[256];
volatile LONG g_mc_count = 0;
char          g_modlist[1100] = " ";   // lower-case, space padded: " engine.dll client.dll "

// Reads the DLL name from the module's export directory, lower-cased. Lock-free (no loader calls).
void export_name(uintptr_t base, char* out, size_t cap) {
    out[0] = 0;
    __try {
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return;
        IMAGE_DATA_DIRECTORY d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!d.VirtualAddress) return;
        IMAGE_EXPORT_DIRECTORY* ed = (IMAGE_EXPORT_DIRECTORY*)(base + d.VirtualAddress);
        const char* nm = (const char*)(base + ed->Name);
        size_t i = 0;
        for (; i + 1 < cap && nm[i]; i++) { char c = nm[i]; out[i] = (c >= 'A' && c <= 'Z') ? c + 32 : c; }
        out[i] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
    }
}

bool in_modlist(const char* name) {
    char key[40];
    if (strlen(name) > 36) return false;
    key[0] = ' ';
    strcpy(key + 1, name);
    strcat(key, " ");
    return strstr(g_modlist, key) != nullptr;
}

bool is_passthrough(const char* n) {
    // Thin API layers between the game and ntdll (kernel32/kernelbase, and the application-
    // compatibility shims that may wrap them).
    return !strcmp(n, "kernelbase.dll") || !strcmp(n, "kernel32.dll") || !strcmp(n, "apphelp.dll") ||
           !strcmp(n, "acgenral.dll") || !strcmp(n, "aclayers.dll") || !strcmp(n, "acspecfc.dll");
}

int classify(uintptr_t addr, const char** name) {
    *name = "";
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void*)addr, &mbi, sizeof(mbi)) || mbi.Type != MEM_IMAGE) return C_OTHER;
    uintptr_t base = (uintptr_t)mbi.AllocationBase;
    if (base == (uintptr_t)&__ImageBase) { *name = "chocolateloading.dll"; return C_SELF; }
    LONG n = g_mc_count;
    if (n > 256) n = 256;
    for (LONG i = 0; i < n; i++)
        if (g_mc[i].ready && g_mc[i].base == base) { *name = g_mc[i].name; return g_mc[i].cls; }
    char nm[64];
    export_name(base, nm, sizeof(nm));
    int cls = !nm[0] ? C_OTHER : is_passthrough(nm) ? C_PASS : in_modlist(nm) ? C_GAME : C_OTHER;
    LONG idx = InterlockedIncrement(&g_mc_count) - 1;
    if (idx < 256) {
        g_mc[idx].base = base;
        g_mc[idx].cls = cls;
        strncpy(g_mc[idx].name, nm, sizeof(g_mc[idx].name) - 1);
        g_mc[idx].name[sizeof(g_mc[idx].name) - 1] = 0;
        MemoryBarrier();
        g_mc[idx].ready = 1;
        *name = g_mc[idx].name;
    }
    return cls;
}

typedef USHORT (NTAPI* RtlBackTraceFn)(ULONG, ULONG, PVOID*, PULONG);
RtlBackTraceFn g_backtrace = nullptr;

} // namespace

void SetGameModules(const char* module_list) {
    if (!g_backtrace) {
        HMODULE nt = GetModuleHandleA("ntdll.dll");
        g_backtrace = nt ? (RtlBackTraceFn)GetProcAddress(nt, "RtlCaptureStackBackTrace") : nullptr;
    }
    char tmp[sizeof(g_modlist)];
    size_t j = 0;
    tmp[j++] = ' ';
    for (const char* p = module_list; *p && j + 2 < sizeof(tmp); p++) {
        char c = *p;
        if (c == ',' || c == ';' || c == '\t') c = ' ';
        tmp[j++] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
    }
    tmp[j++] = ' ';
    tmp[j] = 0;
    if (strcmp(tmp, g_modlist) != 0) strcpy(g_modlist, tmp);
}

__declspec(noinline) bool DirectGameCaller(char* owner, size_t olen) {
    if (!g_backtrace) return false;
    void* frames[24];
    USHORT n = g_backtrace(0, 24, frames, nullptr);
    bool leading = true;
    for (USHORT i = 0; i < n; i++) {
        const char* nm;
        int c = classify((uintptr_t)frames[i], &nm);
        if (c == C_SELF) { if (leading) continue; return false; }   // our own call
        leading = false;
        if (c == C_PASS) continue;
        if (c != C_GAME) return false;   // ntdll (heap growth), the driver, Steam, ...
        if (owner && olen) { strncpy(owner, nm, olen - 1); owner[olen - 1] = 0; }
        return true;
    }
    return false;
}

} // namespace callsite
