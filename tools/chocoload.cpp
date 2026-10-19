// chocoload.exe - starts Portal 2 with chocolateloading.dll loaded BEFORE the engine, which RAM
// snapshots need (the allocator must be hooked before the first engine allocation).
//
//   chocoload.exe [portal2 arguments...]        e.g.  chocoload.exe -novid +map sp_a1_intro4
//
// The arguments are passed to portal2.exe unchanged. Game folder: the CHOCO_GAME environment variable,
// else the folder of chocoload.exe if portal2.exe is there, else DEFAULT_GAME. chocolateloading.dll must
// sit next to portal2.exe.
//
// How: portal2.exe is created suspended and its entry point is temporarily patched with "jmp $". Once
// the main thread spins there, process initialisation is complete (kernel32 is loaded, the loader lock
// is free) but portal2's main() has not loaded launcher.dll -> tier0.dll yet. We then load the DLL with
// a remote LoadLibraryA, restore the entry bytes and let the game run. CHOCOLOAD_EARLY=1 (inherited by
// the child) tells the DLL to set itself up in early mode.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

static const char* DEFAULT_GAME = "E:\\Program Files (x86)\\Steam\\steamapps\\common\\Portal 2";

static bool file_exists(const char* path) { return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; }

static void find_game_dir(char* game, size_t cap) {
    if (GetEnvironmentVariableA("CHOCO_GAME", game, (DWORD)cap) && game[0]) return;
    char self[MAX_PATH], probe[MAX_PATH];
    GetModuleFileNameA(nullptr, self, sizeof(self));
    char* slash = strrchr(self, '\\');
    if (slash) *slash = 0;
    _snprintf(probe, sizeof(probe), "%s\\portal2.exe", self);
    probe[sizeof(probe) - 1] = 0;
    strncpy(game, file_exists(probe) ? self : DEFAULT_GAME, cap - 1);
    game[cap - 1] = 0;
}

// Our own command line minus the program name, exactly as typed (quotes preserved).
static const char* forwarded_args() {
    const char* p = GetCommandLineA();
    if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p) p++; }
    else { while (*p && *p != ' ' && *p != '\t') p++; }
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

// LoadLibraryA(dll) in the child through a remote thread. kernel32 is mapped at the same address in
// every 32-bit process of a session, so our LoadLibraryA address is valid in the child.
static bool inject(HANDLE proc, const char* dll) {
    size_t n = strlen(dll) + 1;
    void* remote = VirtualAllocEx(proc, nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { printf("VirtualAllocEx failed (%lu)\n", GetLastError()); return false; }
    bool ok = false;
    if (WriteProcessMemory(proc, remote, dll, n, nullptr)) {
        FARPROC load = GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA");
        HANDLE th = CreateRemoteThread(proc, nullptr, 0, (LPTHREAD_START_ROUTINE)load, remote, 0, nullptr);
        if (th) {
            DWORD code = 0;
            if (WaitForSingleObject(th, 30000) == WAIT_OBJECT_0 && GetExitCodeThread(th, &code) && code) ok = true;
            else printf("LoadLibrary in the game process failed or timed out\n");
            CloseHandle(th);
        } else {
            printf("CreateRemoteThread failed (%lu)\n", GetLastError());
        }
    } else {
        printf("WriteProcessMemory failed (%lu)\n", GetLastError());
    }
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    return ok;
}

// Address of the EXE entry point of a freshly created (suspended) process: EBX holds its PEB.
static uintptr_t entry_point(HANDLE proc, HANDLE thread) {
    CONTEXT ctx;
    ctx.ContextFlags = CONTEXT_FULL;
    uintptr_t image = 0;
    if (!GetThreadContext(thread, &ctx) ||
        !ReadProcessMemory(proc, (void*)(uintptr_t)(ctx.Ebx + 8), &image, sizeof(image), nullptr))   // PEB->ImageBaseAddress
        return 0;
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS nt;
    if (!ReadProcessMemory(proc, (void*)image, &dos, sizeof(dos), nullptr) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        !ReadProcessMemory(proc, (void*)(image + dos.e_lfanew), &nt, sizeof(nt), nullptr) || nt.Signature != IMAGE_NT_SIGNATURE)
        return 0;
    return image + nt.OptionalHeader.AddressOfEntryPoint;
}

static bool write_code(HANDLE proc, uintptr_t addr, const void* bytes, size_t n) {
    DWORD old;
    if (!VirtualProtectEx(proc, (void*)addr, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    bool ok = WriteProcessMemory(proc, (void*)addr, bytes, n, nullptr) != 0;
    VirtualProtectEx(proc, (void*)addr, n, old, &old);
    FlushInstructionCache(proc, (void*)addr, n);
    return ok;
}

// Parks the main thread on the entry point, loads the DLL, restores the entry point. On success the
// main thread is left suspended (the caller resumes it).
static bool load_dll_at_entry(const PROCESS_INFORMATION& pi, const char* dll) {
    uintptr_t entry = entry_point(pi.hProcess, pi.hThread);
    unsigned char original[2], spin[2] = { 0xEB, 0xFE };   // jmp $
    if (!entry || !ReadProcessMemory(pi.hProcess, (void*)entry, original, 2, nullptr) || !write_code(pi.hProcess, entry, spin, 2)) {
        printf("could not patch the entry point\n");
        return false;
    }
    ResumeThread(pi.hThread);
    bool parked = false;
    for (int waited = 0; waited < 30000 && !parked; waited += 5) {
        Sleep(5);
        SuspendThread(pi.hThread);
        CONTEXT c;
        c.ContextFlags = CONTEXT_CONTROL;
        parked = GetThreadContext(pi.hThread, &c) && c.Eip == (DWORD)entry;
        if (!parked) ResumeThread(pi.hThread);
    }
    if (!parked) { printf("the game never reached its entry point\n"); return false; }
    bool ok = inject(pi.hProcess, dll);
    if (!write_code(pi.hProcess, entry, original, 2)) { printf("could not restore the entry point\n"); ok = false; }
    return ok;
}

int main() {
    char game[MAX_PATH], exe[MAX_PATH], dll[MAX_PATH];
    find_game_dir(game, sizeof(game));
    _snprintf(exe, sizeof(exe), "%s\\portal2.exe", game);
    _snprintf(dll, sizeof(dll), "%s\\chocolateloading.dll", game);
    exe[sizeof(exe) - 1] = dll[sizeof(dll) - 1] = 0;
    if (!file_exists(exe)) { printf("portal2.exe not found: %s (set CHOCO_GAME to the game folder)\n", exe); return 1; }
    if (!file_exists(dll)) { printf("chocolateloading.dll not found: %s\n", dll); return 1; }

    // Command line: "portal2.exe" + our own arguments, unchanged.
    char cmd[8192];
    const char* args = forwarded_args();
    if (strlen(exe) + strlen(args) + 4 > sizeof(cmd)) { printf("command line too long\n"); return 1; }
    sprintf(cmd, "\"%s\"%s%s", exe, *args ? " " : "", args);

    SetEnvironmentVariableA("CHOCOLOAD_EARLY", "1");   // inherited by the game process only
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(exe, cmd, nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, game, &si, &pi)) {
        printf("CreateProcess failed (%lu)\n", GetLastError());
        return 1;
    }
    printf("started portal2.exe (pid %lu)\n", pi.dwProcessId);
    bool ok = load_dll_at_entry(pi, dll);
    if (ok) {
        ResumeThread(pi.hThread);
        printf("chocolateloading.dll loaded before the engine\n");
    } else {
        TerminateProcess(pi.hProcess, 1);   // never leave a half-started game behind
        printf("failed: the game was closed\n");
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return ok ? 0 : 1;
}
