// Small Win32 helpers shared by the plugin: logging, module/section lookup, code/data patching and
// thread suspension.
#pragma once
#include <windows.h>
#include <stdint.h>
#include <stddef.h>

namespace util {

// ---- logging -----------------------------------------------------------------------------------
// Log() appends to <dll dir>\chocolateloading.log. Con()/ConWarn() also print to the game console
// through tier0's Msg/Warning (resolved lazily, so logging works before tier0 is loaded). Never print
// to the console while other threads are suspended or the arena lock is held: it allocates.
void InitLog(HMODULE self);
void Log(const char* fmt, ...);
void Con(const char* fmt, ...);      // console + log
void ConWarn(const char* fmt, ...);  // console (warning colour) + log
void MuteConsole(bool mute);         // while muted, Con()/ConWarn() only write the log file

// ---- modules -----------------------------------------------------------------------------------
struct Section { uintptr_t start, end; char name[9]; };
struct Module {
    char      name[64];             // lower-case file name, e.g. "engine.dll"
    uintptr_t base, end;            // image range
    Section   rw[8];                // writable, non-shared sections (.data, .bss, ...)
    int       rw_count;
};
bool GetModule(const char* name, Module* out);   // false if the module is not loaded

// ---- patching ----------------------------------------------------------------------------------
// Writes a pointer-sized value, temporarily lifting the page protection (keeps execute rights if the
// page had them, so code on the same page stays runnable).
bool  WritePointer(void** where, void* value);
bool  HookVTableSlot(void* object, int index, void* fn, void** original);
// Hooks `dll!fn` by overwriting its first 5 bytes with a JMP to `handler`. The instructions covering
// those bytes must be simple prologue forms that run unchanged elsewhere (hotpatch "mov edi,edi; push
// ebp; mov ebp,esp", ntdll's "mov eax, imm32" syscall stubs, push/sub esp/cmp [abs32] sequences...);
// a function that starts with a jump thunk is hooked at the jump's target. *original receives a
// trampoline that runs the original function; it is published before the hook goes live, because
// other threads may call the hook immediately.
bool  InlineHook(const char* dll, const char* fn, void* handler, void** original);
bool  InlineHookAt(void* fn, void* handler, void** original);
// Looks up an exported function in a mapped image by parsing its export directory (no loader call, so
// it works from a DLL-load notification). Forwarded exports are not followed (nullptr).
void* FindExport(uintptr_t module_base, const char* name);
// Returns the imm16 of the first "ret imm16" of a function (bytes of stack arguments for __thiscall /
// __stdcall, used to check vtable slots), or -2 when the int3 padding that ends the function comes first.
int   FirstRetImm(const void* fn);
// Finds a class vtable from its RTTI type name (e.g. ".?AVCEngine@@") inside a module image.
void* FindVTableByRTTI(const Module& m, const char* mangled_type_name);
// Finds the first object in the module's writable sections whose vtable pointer is `vtable`.
void* FindObjectWithVTable(const Module& m, void* vtable);

// ---- diagnostics -------------------------------------------------------------------------------
// Formats an address as "module+0xoffset" (or the raw address outside any module).
void DescribeAddress(const void* addr, char* out, size_t cap);
// Logs the calling thread's stack as module+offset return addresses.
void LogStack(const char* title);
// Logs fatal exceptions (access violation, illegal instruction, stack overflow, heap corruption...)
// with their location, even when the game's own crash handler terminates the process silently.
void InstallCrashLogger();

// ---- threads -----------------------------------------------------------------------------------
// Suspends every thread of the process except the caller (waiting until each one is really stopped)
// and returns how many were suspended. ResumeOtherThreads() resumes them and closes the handles.
int  SuspendOtherThreads(const DWORD* keep = nullptr, int keep_count = 0);   // `keep`: thread ids left running
void ResumeOtherThreads();

// ---- misc --------------------------------------------------------------------------------------
double Now();   // seconds, high resolution (QueryPerformanceCounter)

// Process command line options: exact token match ("-snap_autotest" does not match
// "-snap_autotest_probe"); CommandLineInt reads the token that follows the flag.
bool CommandLineHas(const char* flag);
int  CommandLineInt(const char* flag, int default_value);
// Copies the token that follows the flag (unquoted); false and "" when the flag is absent.
bool CommandLineStr(const char* flag, char* out, size_t cap);

} // namespace util
