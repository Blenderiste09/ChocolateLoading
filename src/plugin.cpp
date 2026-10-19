// ChocolateLoading - RAM snapshots of the Portal 2 game state (snap_save / snap_load).
//
// Entry points:
//  * Early loader (the supported way): tools/chocoload.exe starts portal2.exe, loads this DLL before
//    the engine and sets CHOCOLOAD_EARLY=1. DllMain hooks NtAllocateVirtualMemory (vatrack) and waits
//    for tier0.dll to be mapped to hook its allocator before anything is allocated (memhook). A worker
//    thread then hooks the client's per-frame callback, and the first frame finishes the setup on the
//    main thread (console commands, snapshot init) and registers us as a server plugin too, for the
//    level callbacks.
//  * plugin_load chocolateloading (fallback): everything is set up from Load(). The hook comes too late
//    to own the allocations made before it, so restores are not reliable in this mode.
#define _CRT_SECURE_NO_WARNINGS
#include "sdk.h"
#include "arena.h"
#include "autotest.h"
#include "control.h"
#include "d3dguard.h"
#include "fileguard.h"
#include "gameapi.h"
#include "memhook.h"
#include "snapshot.h"
#include "soundguard.h"
#include "steamguard.h"
#include "tlsguard.h"
#include "util.h"
#include "vatrack.h"
#include "videoguard.h"
#include <windows.h>
#include <string.h>
#include <wctype.h>

#define CHOCO_VERSION "0.3.0"

// Modules whose writable (.data/.bss) sections are captured, and whose direct VirtualAlloc reservations
// are tracked. Their globals point into or index the game state, so they must revert together with the
// heap. Every Source module that touches game/render state is listed, including vstdlib.dll (thread
// pool, cvars, KeyValues). Our own hooks live in .rdata or in our module, never in these sections.
// tier0.dll is excluded: its .data holds per-thread allocator/clock state, and its VirtualAlloc regions
// are its own heap (unused with route-all). Modules that are not loaded are skipped.
static const char* DEFAULT_DOMAIN =
    "engine.dll client.dll server.dll matchmaking.dll vstdlib.dll "
    "materialsystem.dll shaderapidx9.dll shaderapivk.dll datacache.dll studiorender.dll stdshader_dx9.dll "
    "vphysics.dll vscript.dll vgui2.dll vguimatsurface.dll inputsystem.dll "
    "soundsystem.dll soundemittersystem.dll scenefilecache.dll filesystem_stdio.dll "
    "localize.dll mdllib.dll vaudio_miles.dll vaudio_speex.dll valve_avi.dll launcher.dll";

static HMODULE g_self = nullptr;
static bool    g_setup_done = false;        // hooks installed, snapshot initialised
static bool    g_commands = false;          // snap_* console commands registered
static volatile LONG g_setup_pending = 0;   // early loader: finish the setup on the next frame

static void register_commands() {
    if (g_commands) return;
    control::RegisterCommands();
    g_commands = true;
}

// Arena size: -snap_arena_mb <256..2048> (default 1024).
static size_t arena_reserve_bytes() {
    int mb = util::CommandLineInt("-snap_arena_mb", 1024);
    if (mb < 256) mb = 256;
    if (mb > 2048) mb = 2048;
    return (size_t)mb << 20;
}

// Snapshot options given on the command line (see AGENTS.md for the list).
static void parse_options() {
    snapshot::Options& o = snapshot::Opts();
    if (util::CommandLineHas("-snap_zombies_free")) o.zombie_policy = snapshot::ZOMBIES_FREE;
    if (util::CommandLineHas("-snap_zombies_keep")) o.zombie_policy = snapshot::ZOMBIES_KEEP;
    if (util::CommandLineHas("-snap_autotest_verify")) o.verify_heap = true;
    if (util::CommandLineHas("-snap_full_revert")) o.full_revert = true;
    if (util::CommandLineHas("-snap_no_va")) o.capture_va = false;
    if (util::CommandLineHas("-snap_no_thread_refs")) o.keep_thread_refs = false;
    o.copy_threads = util::CommandLineInt("-snap_copy_threads", o.copy_threads);
    autotest::ParseCommandLine();
}

// ---- per-frame hook (IBaseClientDLL::FrameStageNotify) -----------------------------------------
typedef void (__thiscall* FrameStageFn)(void*, int);
static FrameStageFn o_FrameStageNotify = nullptr;
static void finish_setup_early();

static void __fastcall H_FrameStageNotify(void* self, void*, int stage) {
    if (stage == FRAME_START) {
        if (g_setup_pending && InterlockedExchange(&g_setup_pending, 0)) finish_setup_early();
        if (g_setup_done) {
            control::OnFrameStart();
            autotest::Tick();
        }
    }
    o_FrameStageNotify(self, stage);
}

static bool hook_frame(void* client) {
    if (!client) return false;
    void** vt = *(void***)client;
    if (vt[clientslot::FrameStageNotify] == (void*)H_FrameStageNotify) return true;   // already hooked
    return util::HookVTableSlot(client, clientslot::FrameStageNotify, (void*)H_FrameStageNotify, (void**)&o_FrameStageNotify);
}

// ---- early loader ---------------------------------------------------------------------------------
// Main-thread part of the early setup, run from the first FRAME_START once the engine is up.
static void finish_setup_early() {
    if (g_setup_done || !gameapi::InitConsole()) return;
    control::SetDomain(DEFAULT_DOMAIN);
    char err[256] = "";
    if (!snapshot::Init(nullptr, err, sizeof(err))) util::ConWarn("[snap] init: %s", err);
    register_commands();
    snapshot::ForceEligible(true);   // the allocator was hooked before any map existed
    // Everything is captured from process start, so blocks allocated after a snapshot cannot be
    // referenced from outside it: free them on restore (no leak). -snap_zombies_keep overrides.
    snapshot::Opts().zombie_policy = snapshot::ZOMBIES_FREE;
    parse_options();
    g_setup_done = true;
    util::Con("ChocolateLoading %s ready (early loader): arena at %p", CHOCO_VERSION, (void*)arena::Base());
    // Also register as a server plugin (the engine reuses this already loaded DLL and Load() sees the
    // setup is done) to receive LevelInit/LevelShutdown: a snapshot outlives a level change only while
    // the guards keep what the old map instance released alive (GPU objects, files, MP3 decoders, videos,
    // Steam registrations).
    gameapi::RunCommand("plugin_load chocolateloading");
}

// Worker thread: completes the allocator hook once tier0/vstdlib are initialised (KeyValues), then
// hooks the client's per-frame callback as soon as client.dll is loaded. Never runs under the loader lock.
static DWORD WINAPI BootstrapEarly(void*) {
    // Retry until tier0 (g_pMemAlloc) and vstdlib (KeyValuesSystem) are initialised: ~60 s at most.
    char err[256] = "";
    bool hooked = false;
    for (int i = 0; i < 60000 && !hooked; i++) {
        HMODULE t0 = GetModuleHandleA("tier0.dll");
        void** mem_alloc = t0 ? (void**)GetProcAddress(t0, "g_pMemAlloc") : nullptr;
        hooked = mem_alloc && *mem_alloc && GetModuleHandleA("vstdlib.dll") &&
                 memhook::Install(arena_reserve_bytes(), err, sizeof(err));
        if (!hooked) Sleep(1);
    }
    if (!hooked) {
        util::Log("early: allocator hook failed: %s", err[0] ? err : "tier0/vstdlib never initialised");
        return 0;
    }
    memhook::SetRouteAll(true);   // already on if InstallEarly succeeded; needed if it did not
    util::Log("early: allocator hook complete (route-all), arena at %p", (void*)arena::Base());
    void* client = nullptr;
    for (int i = 0; i < 120000 && !client; i++) {
        client = gameapi::Interface("client.dll", "VClient016");
        if (!client) Sleep(2);
    }
    if (!hook_frame(client)) { util::Log("early: could not hook FrameStageNotify"); return 0; }
    InterlockedExchange(&g_setup_pending, 1);
    util::Log("early: FrameStageNotify hooked, setup continues on the first frame");
    return 0;
}

// LdrRegisterDllNotification: called right after a DLL is mapped, before its initialisation runs.
struct UStr { USHORT Length, MaximumLength; PWSTR Buffer; };
struct DllLoadedData { ULONG Flags; const UStr* FullDllName; const UStr* BaseDllName; PVOID DllBase; ULONG SizeOfImage; };
typedef VOID (CALLBACK* DllNotifyFn)(ULONG reason, const DllLoadedData* data, PVOID ctx);
typedef LONG (NTAPI* LdrRegisterDllNotificationFn)(ULONG flags, DllNotifyFn fn, PVOID ctx, PVOID* cookie);
static const ULONG LDR_DLL_NOTIFICATION_REASON_LOADED = 1;

static bool ustr_ieq(const UStr* s, const wchar_t* lit) {
    if (!s || !s->Buffer) return false;
    size_t n = s->Length / sizeof(wchar_t);
    if (wcslen(lit) != n) return false;
    for (size_t i = 0; i < n; i++) if (towlower(s->Buffer[i]) != lit[i]) return false;
    return true;
}

// Hooks tier0's allocator the moment tier0.dll is mapped, before its own and vstdlib's static
// initialisation, so that not a single engine allocation lands in tier0's heap: such blocks are never
// reverted and corrupt structures that mix them with arena blocks (e.g. vstdlib's lock-free job queue).
// Likewise, vaudio_miles.dll is hooked before it creates any MP3 decoder (soundguard), the D3D9 runtime
// before the shader API creates its device (d3dguard), steam_api.dll before the game registers its
// first callback (steamguard) and binkw32.dll before the first video opens (videoguard).
static VOID CALLBACK OnDllNotify(ULONG reason, const DllLoadedData* d, PVOID) {
    if (reason != LDR_DLL_NOTIFICATION_REASON_LOADED || !d) return;
    if (ustr_ieq(d->BaseDllName, L"tier0.dll")) {
        char err[256] = "";
        if (memhook::InstallEarly((uintptr_t)d->DllBase, d->SizeOfImage, arena_reserve_bytes(), err, sizeof(err)))
            util::Log("early: tier0 mapped at %p -> allocator hooked before tier0 initialisation", d->DllBase);
        else
            util::Log("early: tier0 mapped but the early hook failed: %s (falling back to the late hook)", err);
    } else if (ustr_ieq(d->BaseDllName, L"vaudio_miles.dll")) {
        soundguard::Hook((uintptr_t)d->DllBase, d->SizeOfImage);
    } else if (ustr_ieq(d->BaseDllName, L"d3d9.dll")) {
        d3dguard::HookRuntime((uintptr_t)d->DllBase, "d3d9.dll");
    } else if (ustr_ieq(d->BaseDllName, L"dxvk_d3d9.dll")) {
        d3dguard::HookRuntime((uintptr_t)d->DllBase, "dxvk_d3d9.dll");
    } else if (ustr_ieq(d->BaseDllName, L"steam_api.dll")) {
        steamguard::Hook((uintptr_t)d->DllBase);
    } else if (ustr_ieq(d->BaseDllName, L"binkw32.dll")) {
        videoguard::Hook((uintptr_t)d->DllBase);
    }
}

static void start_early_loader() {
    HMODULE pinned;   // never unload: our hooks stay installed for the life of the process
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCSTR)g_self, &pinned);
    util::InitLog(g_self);
    util::Log("early: DllMain (tier0 %s)", GetModuleHandleA("tier0.dll") ? "ALREADY loaded - too late for the early hook" : "not loaded yet");
    vatrack::Install(DEFAULT_DOMAIN);   // before the engine creates the hunk
    fileguard::Install();               // before the engine opens its first file
    tlsguard::Install();                // before the game's CRTs store their per-thread data
    // A D3D9 runtime may already be loaded (e.g. by an overlay): hook it now, the others when mapped.
    if (HMODULE d3d9 = GetModuleHandleA("d3d9.dll")) d3dguard::HookRuntime((uintptr_t)d3d9, "d3d9.dll");
    if (HMODULE dxvk = GetModuleHandleA("dxvk_d3d9.dll")) d3dguard::HookRuntime((uintptr_t)dxvk, "dxvk_d3d9.dll");
    if (HMODULE steam = GetModuleHandleA("steam_api.dll")) steamguard::Hook((uintptr_t)steam);
    if (HMODULE bink = GetModuleHandleA("binkw32.dll")) videoguard::Hook((uintptr_t)bink);
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    LdrRegisterDllNotificationFn reg = nt ? (LdrRegisterDllNotificationFn)GetProcAddress(nt, "LdrRegisterDllNotification") : nullptr;
    PVOID cookie = nullptr;
    if (!reg || reg(0, OnDllNotify, nullptr, &cookie) < 0) util::Log("early: LdrRegisterDllNotification failed");
    HANDLE th = CreateThread(nullptr, 0, BootstrapEarly, nullptr, 0, nullptr);
    if (th) CloseHandle(th); else util::Log("early: could not start the bootstrap thread");
}

// ---- server plugin interface ------------------------------------------------------------------------
class ChocolatePlugin : public IServerPluginCallbacks {
public:
    bool Load(CreateInterfaceFn interfaceFactory, CreateInterfaceFn) override {
        util::InitLog(g_self);
        util::Log("Load() version %s", CHOCO_VERSION);
        if (g_setup_done) {   // set up by the early loader, or loaded again after plugin_unload
            register_commands();
            util::Con("[snap] ChocolateLoading %s already set up", CHOCO_VERSION);
            return true;
        }
        HMODULE pinned;   // the allocator hook can never be removed: never unload
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCSTR)g_self, &pinned);
        if (!gameapi::InitConsole()) return false;
        char err[256] = "";
        if (!memhook::Install(arena_reserve_bytes(), err, sizeof(err))) {
            util::ConWarn("[snap] allocator hook failed: %s", err);
            return false;
        }
        control::SetDomain(DEFAULT_DOMAIN);
        if (!snapshot::Init(interfaceFactory, err, sizeof(err))) util::ConWarn("[snap] init: %s", err);
        util::InstallCrashLogger();
        util::Module vaudio;   // late: decoders created before this point are not guarded
        if (util::GetModule("vaudio_miles.dll", &vaudio)) soundguard::Hook(vaudio.base, vaudio.end - vaudio.base);
        if (!hook_frame(gameapi::Interface("client.dll", "VClient016"))) {
            util::ConWarn("[snap] could not hook FrameStageNotify");
            return false;
        }
        register_commands();
        parse_options();
        g_setup_done = true;
        util::Con("ChocolateLoading %s loaded: arena at %p, domain: %s", CHOCO_VERSION, (void*)arena::Base(), memhook::DomainDescription());
        util::ConWarn("[snap] loaded with plugin_load: allocations made before this point are not captured and "
                      "restores are NOT reliable. Launch the game with chocoload.exe instead.");
        return true;
    }
    void Unload() override {
        // The hooks stay (the DLL is pinned and the arena still holds the engine heap); only the
        // console commands go away. A later plugin_load registers them again.
        gameapi::RemoveCommands();
        g_commands = false;
        util::Log("Unload()");
    }
    void Pause() override {}
    void UnPause() override {}
    const char* GetPluginDescription() override { return "ChocolateLoading " CHOCO_VERSION " (RAM snapshots)"; }
    void LevelInit(const char* map) override {
        util::Log("LevelInit(%s)", map ? map : "");
        control::OnLevelInit(map);
    }
    void ServerActivate(edict_t*, int, int) override {}
    void GameFrame(bool) override {}
    void LevelShutdown() override {
        util::Log("LevelShutdown()");
        control::OnLevelShutdown();
    }
    void ClientActive(edict_t*) override {}
    void ClientFullyConnect(edict_t*) override {}
    void ClientDisconnect(edict_t*) override {}
    void ClientPutInServer(edict_t*, const char*) override {}
    void SetCommandClient(int) override {}
    void ClientSettingsChanged(edict_t*) override {}
    PLUGIN_RESULT ClientConnect(bool*, edict_t*, const char*, const char*, char*, int) override { return PLUGIN_CONTINUE; }
    PLUGIN_RESULT ClientCommand(edict_t*, const CCommand&) override { return PLUGIN_CONTINUE; }
    PLUGIN_RESULT NetworkIDValidated(const char*, const char*) override { return PLUGIN_CONTINUE; }
    void OnQueryCvarValueFinished(QueryCvarCookie_t, edict_t*, EQueryCvarValueStatus, const char*, const char*) override {}
    void OnEdictAllocated(edict_t*) override {}
    void OnEdictFreed(const edict_t*) override {}
};

static ChocolatePlugin g_plugin;

extern "C" __declspec(dllexport) void* CreateInterface(const char* name, int* ret) {
    // The engine dispatches to fixed vtable slots (ClientActive = 9, ClientFullyConnect = 10, ...),
    // i.e. the 003 layout, which is what IServerPluginCallbacks implements.
    if (name && strcmp(name, "ISERVERPLUGINCALLBACKS003") == 0) {
        if (ret) *ret = 0;
        return static_cast<IServerPluginCallbacks*>(&g_plugin);
    }
    if (ret) *ret = 1;
    return nullptr;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        char early[8] = "";
        if (GetEnvironmentVariableA("CHOCOLOAD_EARLY", early, sizeof(early)) && early[0] == '1') {
            start_early_loader();
            util::InstallCrashLogger();
        }
    } else if (reason == DLL_PROCESS_DETACH && reserved) {
        // ExitProcess: record who asked the game to exit (a TerminateProcess never gets here).
        util::LogStack("process exit requested");
    }
    return TRUE;
}
