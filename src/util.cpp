#define _CRT_SECURE_NO_WARNINGS
#include "util.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

namespace util {

// ---- logging -----------------------------------------------------------------------------------
namespace {
CRITICAL_SECTION g_log_cs;
HANDLE  g_log_file = INVALID_HANDLE_VALUE;
bool    g_log_init = false;
volatile LONG g_mute = 0;
typedef void (*MsgFn)(const char* fmt, ...);
MsgFn   g_msg = nullptr;
MsgFn   g_warning = nullptr;

void write_file(const char* s, size_t n) {
    if (g_log_file == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(g_log_file, s, (DWORD)n, &written, nullptr);
}

void vlog(bool console, bool warn, const char* fmt, va_list ap) {
    char buf[2048];
    int n = _vsnprintf(buf, sizeof(buf) - 2, fmt, ap);
    if (n < 0 || n > (int)sizeof(buf) - 2) n = (int)sizeof(buf) - 2;   // truncated
    buf[n] = 0;
    if (g_log_init) {
        EnterCriticalSection(&g_log_cs);
        SYSTEMTIME st;
        GetLocalTime(&st);
        char ts[32];
        int tn = _snprintf(ts, sizeof(ts), "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        write_file(ts, tn);
        write_file(buf, n);
        if (n == 0 || buf[n - 1] != '\n') write_file("\n", 1);
        LeaveCriticalSection(&g_log_cs);
    }
    if (console && !g_mute) {
        if (n == 0 || buf[n - 1] != '\n') { buf[n++] = '\n'; buf[n] = 0; }
        if (!g_msg) {   // the log may be opened before tier0 is loaded (early loader)
            HMODULE t0 = GetModuleHandleA("tier0.dll");
            if (t0) {
                g_warning = (MsgFn)GetProcAddress(t0, "Warning");
                g_msg = (MsgFn)GetProcAddress(t0, "Msg");
            }
        }
        MsgFn f = warn ? g_warning : g_msg;
        if (f) f("%s", buf);
    }
}
} // namespace

void InitLog(HMODULE self) {
    if (g_log_init) return;
    InitializeCriticalSection(&g_log_cs);
    char path[MAX_PATH];
    GetModuleFileNameA(self, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) strcpy(slash + 1, "chocolateloading.log"); else strcpy(path, "chocolateloading.log");
    g_log_file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    g_log_init = true;
    Log("==== ChocolateLoading log opened ====");
}

void Log(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vlog(false, false, fmt, ap); va_end(ap); }
void Con(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vlog(true, false, fmt, ap); va_end(ap); }
void ConWarn(const char* fmt, ...) { va_list ap; va_start(ap, fmt); vlog(true, true, fmt, ap); va_end(ap); }
void MuteConsole(bool mute) { InterlockedExchange(&g_mute, mute ? 1 : 0); }

// ---- modules -----------------------------------------------------------------------------------
static bool fill_module(HMODULE h, Module* out) {
    memset(out, 0, sizeof(*out));
    char path[MAX_PATH];
    if (!GetModuleFileNameA(h, path, MAX_PATH)) return false;
    const char* nm = strrchr(path, '\\');
    nm = nm ? nm + 1 : path;
    strncpy(out->name, nm, sizeof(out->name) - 1);
    _strlwr(out->name);
    uintptr_t base = (uintptr_t)h;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    out->base = base;
    out->end = base + nt->OptionalHeader.SizeOfImage;
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        DWORD c = sec[i].Characteristics;
        if ((c & IMAGE_SCN_MEM_WRITE) && !(c & IMAGE_SCN_MEM_SHARED) && out->rw_count < 8) {
            Section& s = out->rw[out->rw_count++];
            s.start = base + sec[i].VirtualAddress;
            // VirtualSize (not SizeOfRawData) so that the zero-initialised .bss tail is included.
            DWORD vs = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
            s.end = s.start + ((vs + 0xFFF) & ~0xFFFu);
            memcpy(s.name, sec[i].Name, 8);
            s.name[8] = 0;
        }
    }
    return true;
}

bool GetModule(const char* name, Module* out) {
    HMODULE h = GetModuleHandleA(name);
    return h && fill_module(h, out);
}

// ---- patching ----------------------------------------------------------------------------------
static bool has_execute(DWORD protect) {
    DWORD p = protect & 0xFF;
    return p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

bool WritePointer(void** where, void* value) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(where, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    DWORD writable = has_execute(mbi.Protect) ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
    DWORD old;
    if (!VirtualProtect(where, sizeof(void*), writable, &old)) return false;
    InterlockedExchangePointer(where, value);   // atomic: other threads may read the slot concurrently
    VirtualProtect(where, sizeof(void*), old, &old);
    FlushInstructionCache(GetCurrentProcess(), where, sizeof(void*));
    return true;
}

bool HookVTableSlot(void* object, int index, void* fn, void** original) {
    void** vt = *(void***)object;
    if (original) *original = vt[index];
    return WritePointer(&vt[index], fn);
}

namespace {
// Trampolines live in one executable page (32 bytes each), writable only while being filled.
uint8_t* g_tramp_page = nullptr;
size_t   g_tramp_used = 0;
const size_t TRAMP_SIZE = 32, TRAMP_PAGE = 4096, MAX_PROLOGUE = TRAMP_SIZE - 5;

// Bytes taken by a ModRM operand (ModRM byte, SIB, displacement). 32-bit addressing has no
// EIP-relative form, so such an operand means the same thing wherever the instruction is copied.
int modrm_len(const uint8_t* m) {
    int mod = m[0] >> 6, rm = m[0] & 7, n = 1;
    if (mod == 3) return n;
    if (rm == 4) { n++; if (mod == 0 && (m[1] & 7) == 5) n += 4; }   // SIB (with a disp32 base)
    if (mod == 1) n += 1;
    else if (mod == 2 || (mod == 0 && rm == 5)) n += 4;
    return n;
}

// Length of the instruction at p when it can run unchanged from a trampoline (the usual prologue
// instructions: push/pop, mov/lea/arithmetic on registers and memory, cmp [abs], sub/and esp...), else
// 0: relative branches, returns and anything unknown.
int prologue_insn_len(const uint8_t* p) {
    uint8_t op = p[0];
    if (op == 0x64 || op == 0x65) {                                        // fs/gs segment prefix
        int n = prologue_insn_len(p + 1);
        return n ? n + 1 : 0;
    }
    if (op >= 0x40 && op <= 0x5F) return 1;                                 // inc/dec/push/pop r32
    if (op == 0x90) return 1;                                               // nop
    if (op >= 0xB0 && op <= 0xB7) return 2;                                 // mov r8, imm8
    if (op >= 0xB8 && op <= 0xBF) return 5;                                 // mov r32, imm32
    if (op == 0x6A) return 2;                                               // push imm8
    if (op == 0x68) return 5;                                               // push imm32
    if (op == 0xA1 || op == 0xA3) return 5;                                 // mov eax <-> [abs32]
    switch (op) {
    case 0x01: case 0x03: case 0x09: case 0x0B: case 0x21: case 0x23: case 0x29: case 0x2B:
    case 0x31: case 0x33: case 0x39: case 0x3B: case 0x85: case 0x89: case 0x8B: case 0x8D:
        return 1 + modrm_len(p + 1);                                        // op r/m, r (no immediate)
    case 0x80: case 0x83: case 0xC6:
        return 1 + modrm_len(p + 1) + 1;                                    // op r/m, imm8
    case 0x81: case 0xC7:
        return 1 + modrm_len(p + 1) + 4;                                    // op r/m, imm32
    case 0xFF:
        return ((p[1] >> 3) & 7) == 6 ? 1 + modrm_len(p + 1) : 0;           // push r/m only
    }
    return 0;
}

uint8_t* new_trampoline(const uint8_t* prologue, size_t len, const uint8_t* resume_at) {
    if (!g_tramp_page) {
        g_tramp_page = (uint8_t*)VirtualAlloc(nullptr, TRAMP_PAGE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READ);
        if (!g_tramp_page) return nullptr;
    }
    if (g_tramp_used + TRAMP_SIZE > TRAMP_PAGE) return nullptr;
    uint8_t* t = g_tramp_page + g_tramp_used;
    DWORD old;
    if (!VirtualProtect(g_tramp_page, TRAMP_PAGE, PAGE_EXECUTE_READWRITE, &old)) return nullptr;
    memcpy(t, prologue, len);                                 // the relocated original instructions
    t[len] = 0xE9;                                             // jmp rel32 back into the original
    *(int32_t*)(t + len + 1) = (int32_t)(resume_at - (t + len + 5));
    VirtualProtect(g_tramp_page, TRAMP_PAGE, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), t, TRAMP_SIZE);
    g_tramp_used += TRAMP_SIZE;
    return t;
}

// Replaces the first 5 bytes of `target` with `jmp handler`, atomically when they sit inside one
// aligned 8-byte block (always the case for ntdll stubs and hotpatchable functions).
bool write_jump(uint8_t* target, const void* handler) {
    uint8_t jmp[5];
    jmp[0] = 0xE9;
    *(int32_t*)(jmp + 1) = (int32_t)((const uint8_t*)handler - (target + 5));
    DWORD old;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old)) return false;
    uintptr_t off = (uintptr_t)target & 7;
    if (off <= 3) {
        volatile LONGLONG* block = (volatile LONGLONG*)(target - off);
        LONGLONG cur, next;
        do {
            cur = *block;
            next = cur;
            memcpy((uint8_t*)&next + off, jmp, 5);
        } while (InterlockedCompareExchange64(block, next, cur) != cur);
    } else {
        memcpy(target, jmp, 5);
    }
    VirtualProtect(target, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    return true;
}
} // namespace

bool InlineHook(const char* dll, const char* fn, void* handler, void** original) {
    HMODULE m = GetModuleHandleA(dll);
    void* target = m ? (void*)GetProcAddress(m, fn) : nullptr;
    return target && InlineHookAt(target, handler, original);
}

bool InlineHookAt(void* fn, void* handler, void** original) {
    uint8_t* target = (uint8_t*)fn;
    // Follow jump thunks inside the module. A jump to another module is someone else's hook (or ours):
    // the prologue check below refuses it, so a function is never hooked twice.
    for (int i = 0; i < 4 && target[0] == 0xE9; i++) {
        uint8_t* next = target + 5 + *(int32_t*)(target + 1);
        MEMORY_BASIC_INFORMATION a, b;
        if (!VirtualQuery(target, &a, sizeof(a)) || !VirtualQuery(next, &b, sizeof(b)) || a.AllocationBase != b.AllocationBase) break;
        target = next;
    }
    size_t len = 0;
    while (len < 5) {
        int n = prologue_insn_len(target + len);
        if (!n || len + n > MAX_PROLOGUE) return false;
        len += n;
    }
    uint8_t* tramp = new_trampoline(target, len, target + len);
    if (!tramp) return false;
    *original = tramp;   // publish BEFORE the jump goes live
    if (!write_jump(target, handler)) { *original = nullptr; return false; }
    return true;
}

void* FindExport(uintptr_t base, const char* name) {
    __try {
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
        IMAGE_DATA_DIRECTORY d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (nt->Signature != IMAGE_NT_SIGNATURE || !d.VirtualAddress) return nullptr;
        IMAGE_EXPORT_DIRECTORY* ed = (IMAGE_EXPORT_DIRECTORY*)(base + d.VirtualAddress);
        const DWORD* names = (const DWORD*)(base + ed->AddressOfNames);
        const WORD*  ords  = (const WORD*)(base + ed->AddressOfNameOrdinals);
        const DWORD* funcs = (const DWORD*)(base + ed->AddressOfFunctions);
        for (DWORD i = 0; i < ed->NumberOfNames; i++) {
            if (strcmp((const char*)(base + names[i]), name) != 0) continue;
            DWORD rva = funcs[ords[i]];
            if (rva >= d.VirtualAddress && rva < d.VirtualAddress + d.Size) return nullptr;   // forwarder
            return (void*)(base + rva);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

int FirstRetImm(const void* fn) {
    const uint8_t* c = (const uint8_t*)fn;
    for (int i = 0; i < 0x600; i++) {
        if (c[i] == 0xC2 && c[i + 2] == 0x00) return c[i + 1];
        if (c[i] == 0xCC && c[i + 1] == 0xCC && c[i + 2] == 0xCC) break;   // padding: end of function
    }
    return -2;
}

static bool in_image(const Module& m, uintptr_t p) { return p >= m.base && p < m.end; }

static void* find_vtable_by_rtti(const Module& m, const char* tname) {
    size_t len = strlen(tname);
    // 1) RTTI type descriptor: { vftable, spare, name[] } -> the name string is at +8.
    uintptr_t td = 0;
    for (uintptr_t p = m.base; p + len < m.end; p++)
        if (*(const char*)p == '.' && memcmp((const void*)p, tname, len + 1) == 0) { td = p - 8; break; }
    if (!td) return nullptr;
    // 2) complete object locator: { signature 0, offset 0, cdOffset, pTypeDescriptor, pClassDescriptor }.
    for (uintptr_t p = m.base; p + 20 < m.end; p += 4) {
        const uint32_t* col = (const uint32_t*)p;
        if (col[3] != td || col[0] != 0 || col[1] != 0 || !in_image(m, col[4])) continue;
        // 3) the vftable is preceded by a pointer to its complete object locator.
        for (uintptr_t q = m.base; q + 8 < m.end; q += 4)
            if (*(const uint32_t*)q == (uint32_t)p && in_image(m, *(const uint32_t*)(q + 4)))
                return (void*)(q + 4);
    }
    return nullptr;
}

void* FindVTableByRTTI(const Module& m, const char* tname) {
    __try {
        return find_vtable_by_rtti(m, tname);
    } __except (EXCEPTION_EXECUTE_HANDLER) {   // an unreadable page inside the image
        return nullptr;
    }
}

void* FindObjectWithVTable(const Module& m, void* vtable) {
    for (int i = 0; i < m.rw_count; i++)
        for (uintptr_t p = m.rw[i].start; p + 4 <= m.rw[i].end; p += 4) {
            if ((p & 0xFFF) == 0) {   // skip pages that are not committed
                MEMORY_BASIC_INFORMATION mbi;
                if (!VirtualQuery((void*)p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) { p += 0xFFC; continue; }
            }
            if (*(void**)p == vtable) return (void*)p;
        }
    return nullptr;
}

// ---- diagnostics -------------------------------------------------------------------------------
void DescribeAddress(const void* addr, char* out, size_t cap) {
    HMODULE m = nullptr;
    char path[MAX_PATH];
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m) &&
        GetModuleFileNameA(m, path, sizeof(path))) {
        const char* name = strrchr(path, '\\');
        _snprintf(out, cap, "%s+0x%x", name ? name + 1 : path, (unsigned)((uintptr_t)addr - (uintptr_t)m));
    } else {
        _snprintf(out, cap, "%p", addr);
    }
    out[cap - 1] = 0;
}

void LogStack(const char* title) {
    void* frames[32];
    USHORT n = RtlCaptureStackBackTrace(1, 32, frames, nullptr);
    Log("%s (thread %lu):", title, GetCurrentThreadId());
    for (USHORT i = 0; i < n; i++) {
        char where[MAX_PATH + 16];
        DescribeAddress(frames[i], where, sizeof(where));
        Log("    %s", where);
    }
}

namespace {
LONG CALLBACK log_fatal_exception(EXCEPTION_POINTERS* e) {
    static volatile LONG logged = 0;
    DWORD code = e->ExceptionRecord->ExceptionCode;
    bool fatal = code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
                 code == EXCEPTION_PRIV_INSTRUCTION || code == EXCEPTION_STACK_OVERFLOW ||
                 code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == 0xC0000374 /* STATUS_HEAP_CORRUPTION */ ||
                 code == 0xC0000409 /* STATUS_STACK_BUFFER_OVERRUN */;
    if (fatal && InterlockedIncrement(&logged) <= 8) {   // first-chance: may still be handled
        char where[MAX_PATH + 16];
        DescribeAddress(e->ExceptionRecord->ExceptionAddress, where, sizeof(where));
        ULONG_PTR* info = e->ExceptionRecord->ExceptionInformation;
        if (code == EXCEPTION_ACCESS_VIOLATION && e->ExceptionRecord->NumberParameters >= 2)
            Log("exception 0x%08lx at %s: %s %p", code, where, info[0] == 0 ? "read of" : info[0] == 1 ? "write to" : "execute at", (void*)info[1]);
        else
            Log("exception 0x%08lx at %s", code, where);
        if (code != EXCEPTION_STACK_OVERFLOW) LogStack("  stack");
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
} // namespace

void InstallCrashLogger() {
    static bool installed = false;
    if (!installed) installed = AddVectoredExceptionHandler(1, log_fatal_exception) != nullptr;
}

// ---- threads -----------------------------------------------------------------------------------
namespace {
HANDLE g_suspended[1024];
int    g_suspended_count = 0;
}

// NtGetNextThread walks the threads of one process (Vista+). Toolhelp's Thread32Next walks every thread
// of the system instead, remapping its snapshot at each step: ~50 ms with Steam and a browser running.
typedef LONG (NTAPI* NtGetNextThreadFn)(HANDLE process, HANDLE thread, ACCESS_MASK access, ULONG attributes,
                                        ULONG flags, PHANDLE next);

int SuspendOtherThreads(const DWORD* keep, int keep_count) {
    static NtGetNextThreadFn next_thread =
        (NtGetNextThreadFn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtGetNextThread");
    g_suspended_count = 0;
    if (!next_thread) return 0;
    DWORD self = GetCurrentThreadId();
    const ACCESS_MASK access = THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION;
    HANDLE cur = nullptr, next;
    bool cur_kept = false;   // cur is in g_suspended (the walk still needs it as its position)
    while (next_thread(GetCurrentProcess(), cur, access, 0, 0, &next) >= 0) {
        if (cur && !cur_kept) CloseHandle(cur);
        cur = next;
        cur_kept = false;
        DWORD tid = GetThreadId(cur);
        bool skip = tid == self;
        for (int i = 0; i < keep_count && !skip; i++) skip = keep[i] == tid;
        if (skip || g_suspended_count >= 1024 || SuspendThread(cur) == (DWORD)-1) continue;
        g_suspended[g_suspended_count++] = cur;
        cur_kept = true;
    }
    if (cur && !cur_kept) CloseHandle(cur);
    // SuspendThread only queues the suspension: a thread waiting in the kernel stops when it gets to run
    // it. GetThreadContext returns once the thread has really stopped. Asking only now, after every
    // suspension was queued, lets those waits overlap instead of adding up.
    for (int i = 0; i < g_suspended_count; i++) {
        CONTEXT ctx;
        ctx.ContextFlags = CONTEXT_CONTROL;
        GetThreadContext(g_suspended[i], &ctx);
    }
    return g_suspended_count;
}

const void* const* SuspendedThreads(int* count) {
    *count = g_suspended_count;
    return (const void* const*)g_suspended;
}

void ResumeOtherThreads() {
    for (int i = 0; i < g_suspended_count; i++) {
        ResumeThread(g_suspended[i]);
        CloseHandle(g_suspended[i]);
    }
    g_suspended_count = 0;
}

// ---- misc --------------------------------------------------------------------------------------
double Now() {
    static LARGE_INTEGER freq = {};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}

// Returns the position just after `flag` when it appears as a whole token, else nullptr.
static const char* find_flag(const char* flag) {
    const char* cl = GetCommandLineA();
    size_t n = strlen(flag);
    for (const char* p = strstr(cl, flag); p; p = strstr(p + 1, flag)) {
        bool starts = p == cl || p[-1] == ' ' || p[-1] == '\t' || p[-1] == '"';
        bool ends = p[n] == 0 || p[n] == ' ' || p[n] == '\t' || p[n] == '"';
        if (starts && ends) return p + n;
    }
    return nullptr;
}

bool CommandLineHas(const char* flag) { return find_flag(flag) != nullptr; }

int CommandLineInt(const char* flag, int default_value) {
    const char* p = find_flag(flag);
    if (!p) return default_value;
    while (*p == ' ' || *p == '\t' || *p == '"') p++;
    return (*p >= '0' && *p <= '9') || *p == '-' ? atoi(p) : default_value;
}

bool CommandLineStr(const char* flag, char* out, size_t cap) {
    out[0] = 0;
    const char* p = find_flag(flag);
    if (!p) return false;
    while (*p == ' ' || *p == '\t' || *p == '"') p++;
    size_t n = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '"' && n + 1 < cap) out[n++] = *p++;
    out[n] = 0;
    return n > 0;
}

} // namespace util
