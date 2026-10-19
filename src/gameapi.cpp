#define _CRT_SECURE_NO_WARNINGS
#include "gameapi.h"
#include "util.h"
#include <windows.h>
#include <string.h>

namespace gameapi {

namespace {
typedef void (__thiscall* RegisterFn)(void*, ConCommandBase*);
typedef ConCommandBase* (__thiscall* FindBaseFn)(void*, const char*);
typedef ConVar* (__thiscall* FindVarFn)(void*, const char*);
typedef void (__thiscall* ClientCmdFn)(void*, const char*);
typedef bool (__thiscall* BoolFn)(void*);

void*      g_cvar = nullptr;          // ICvar (VEngineCvar007)
void*      g_cmd_vtable = nullptr;    // vtable of an engine ConCommand, reused by ours
void*      g_engclient = nullptr;     // IVEngineClient (VEngineClient015)
const int  MAX_COMMANDS = 24;
ConCommand g_cmds[MAX_COMMANDS];      // our commands (the engine keeps pointers to them)
int        g_cmd_count = 0;

void* engine_client() {
    if (!g_engclient) g_engclient = Interface("engine.dll", "VEngineClient015");
    return g_engclient;
}
} // namespace

void* Interface(const char* module, const char* name) {
    HMODULE m = GetModuleHandleA(module);
    CreateInterfaceFn f = m ? (CreateInterfaceFn)GetProcAddress(m, "CreateInterface") : nullptr;
    return f ? f(name, nullptr) : nullptr;
}

bool InitConsole() {
    if (g_cvar && g_cmd_vtable) return true;
    g_cvar = Interface("vstdlib.dll", "VEngineCvar007");
    if (!g_cvar) { util::ConWarn("[snap] VEngineCvar007 not found"); return false; }
    // Layout check: looking up "echo" must return a command named "echo". We never implement the
    // ConCommand vtable ourselves; our command objects reuse this engine command's vtable.
    ConCommandBase* echo = vfunc<FindBaseFn>(g_cvar, icvar::FindCommandBase)(g_cvar, "echo");
    if (!echo || !echo->m_pszName || strcmp(echo->m_pszName, "echo") != 0) {
        util::ConWarn("[snap] ICvar layout check failed");
        g_cvar = nullptr;
        return false;
    }
    g_cmd_vtable = echo->vtable;
    return true;
}

ConVar* FindVar(const char* name) {
    if (!g_cvar) return nullptr;
    ConVar* v = vfunc<FindVarFn>(g_cvar, icvar::FindVar)(g_cvar, name);
    return v ? (v->m_pParent ? v->m_pParent : v) : nullptr;
}

void AddCommand(const char* name, const char* help, FnCommandCallback_t callback) {
    if (!g_cmd_vtable || g_cmd_count >= MAX_COMMANDS) return;
    ConCommand* c = &g_cmds[g_cmd_count++];
    memset(c, 0, sizeof(*c));
    c->vtable = g_cmd_vtable;
    c->m_pszName = name;
    c->m_pszHelpString = help;
    c->m_nFlags = FCVAR_DONTRECORD;
    c->m_fnCommandCallback = callback;
    c->m_bits = 0x02;   // m_bUsingNewCommandCallback
    vfunc<RegisterFn>(g_cvar, icvar::RegisterConCommand)(g_cvar, c);
}

void RemoveCommands() {
    for (int i = 0; i < g_cmd_count; i++)
        if (g_cmds[i].m_bRegistered) vfunc<RegisterFn>(g_cvar, icvar::UnregisterConCommand)(g_cvar, &g_cmds[i]);
    g_cmd_count = 0;
}

void RunCommand(const char* cmd) {
    void* ec = engine_client();
    if (!ec) { util::ConWarn("[snap] VEngineClient015 not found, cannot run '%s'", cmd); return; }
    vfunc<ClientCmdFn>(ec, engclientslot::ClientCmd_Unrestricted)(ec, cmd);
}

bool InGame() {
    void* ec = engine_client();
    return ec && vfunc<BoolFn>(ec, engclientslot::IsInGame)(ec) && !vfunc<BoolFn>(ec, engclientslot::IsDrawingLoadingImage)(ec);
}

} // namespace gameapi
