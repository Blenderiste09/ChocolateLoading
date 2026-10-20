// Minimal Portal 2 (Windows, 2026 build) SDK surface used by the plugin.
// Every vtable index here was checked against the Mac binaries (symbols) and the current
// Windows binaries; see re/notes.md for the evidence.
#pragma once
#include <stdint.h>

typedef void* (*CreateInterfaceFn)(const char* name, int* ret);

struct edict_t;
typedef int QueryCvarCookie_t;
enum PLUGIN_RESULT { PLUGIN_CONTINUE = 0, PLUGIN_OVERRIDE, PLUGIN_STOP };
enum EQueryCvarValueStatus { eQueryCvarValueStatus_ValueIntact = 0 };

// ---- console command arguments (tier1 CCommand) --------------------------------------------
struct CCommand {
    enum { COMMAND_MAX_ARGC = 64, COMMAND_MAX_LENGTH = 512 };
    int         m_nArgc;
    int         m_nArgv0Size;
    char        m_pArgSBuffer[COMMAND_MAX_LENGTH];
    char        m_pArgvBuffer[COMMAND_MAX_LENGTH];
    const char* m_ppArgv[COMMAND_MAX_ARGC];

    int ArgC() const { return m_nArgc; }
    const char* Arg(int i) const { return (i >= 0 && i < m_nArgc) ? m_ppArgv[i] : ""; }
    const char* ArgS() const { return m_nArgv0Size ? &m_pArgSBuffer[m_nArgv0Size] : ""; }
};

// ---- ConCommandBase / ConCommand / ConVar memory layout -------------------------------------
// We never implement the ConCommand vtable ourselves: a command object copies the vtable pointer
// of an existing engine ConCommand, so Dispatch() is the engine's own implementation which calls
// m_fnCommandCallback when m_bUsingNewCommandCallback is set.
typedef void (*FnCommandCallback_t)(const CCommand& command);

struct ConCommandBase {
    void*           vtable;           // +0x00
    ConCommandBase* m_pNext;          // +0x04
    bool            m_bRegistered;    // +0x08
    const char*     m_pszName;        // +0x0C
    const char*     m_pszHelpString;  // +0x10
    int             m_nFlags;         // +0x14
};

struct ConCommand : ConCommandBase {
    FnCommandCallback_t m_fnCommandCallback;   // +0x18
    void*               m_fnCompletionCallback; // +0x1C
    uint8_t             m_bits;               // +0x20: bit0 has completion, bit1 new callback, bit2 interface
};
static_assert(sizeof(ConCommandBase) == 0x18, "ConCommandBase layout");

struct ConVar : ConCommandBase {
    void*       iconvar_vtable;     // +0x18
    ConVar*     m_pParent;          // +0x1C
    const char* m_pszDefaultValue;  // +0x20
    char*       m_pszString;        // +0x24
    int         m_StringLength;     // +0x28
    float       m_fValue;           // +0x2C
    int         m_nValue;           // +0x30
};

enum { FCVAR_DONTRECORD = (1 << 17) };

// ---- ICvar (VEngineCvar007) vtable indices ----------------------------------------------------
namespace icvar {
    enum {
        RegisterConCommand   = 9,
        UnregisterConCommand = 10,
        FindCommandBase      = 13,   // const/non-const overload pair at 13/14
        FindVar              = 15,   // pair at 15/16
    };
}

// ---- IMemAlloc vtable slots (tier0 CStdMemAlloc, MSVC overload order) -------------------------
namespace memslot {
    enum {
        AllocDbg = 0, Alloc = 1,
        ReallocDbg = 2, Realloc = 3,
        FreeDbg = 4, Free = 5,
        ExpandDbg = 6, Expand = 7,
        GetSize = 8,
        RegionAllocDbg = 37, RegionAlloc = 38,
    };
}

// ---- IKeyValuesSystem (vstdlib KeyValuesSystem()) --------------------------------------------
namespace kvslot { enum { AllocKeyValuesMemory = 1, FreeKeyValuesMemory = 2 }; }

// ---- IThreadPool (vstdlib g_pThreadPool) ------------------------------------------------------
namespace poolslot { enum { NumThreads = 7, NumIdleThreads = 8, SuspendExecution = 9, ResumeExecution = 10 }; }

// ---- IFileSystem (VFileSystem017) -------------------------------------------------------------
namespace fsslot { enum { AsyncFinishAll = 44, AsyncSuspend = 47, AsyncResume = 48 }; }

// ---- IBaseClientDLL (VClient016) --------------------------------------------------------------
namespace clientslot { enum { IN_ClearStates = 18, FrameStageNotify = 35 }; }

// ---- IEngineSound ("IEngineSoundClient003", engine.dll) ----------------------------------------
// Slot 9 = StopAllSounds(bool clear_buffers) -> S_StopAllSounds (Windows engine!0x10017880, which logs
// "Stopping All Sounds..."); same slot on Mac (CEngineSoundClient vtable).
namespace soundslot { enum { StopAllSounds = 9 }; }

// IVEngineClient ("VEngineClient015", engine.dll; CEngineClient vtable at engine+0x3553f4 in the RE
// build). Verified on Windows: slot 7 = ClientCmd, 104 = ExecuteClientCmd, 108 = ClientCmd_Unrestricted
// (cdll_engine_int.cpp:0x330 split-screen guard + Cbuf_AddText(cmd) + Cbuf_AddText("\n")), same as Mac.
// Also verified: 25 IsInGame (signon state == 6), 27 IsDrawingLoadingImage (scr_drawloading), 139
// RecordDemoCustomData (Windows engine!0x1007c080, Mac CEngineClient::RecordDemoCustomData): it starts
// with `mov ecx,[demorecorder]` and calls the demo recorder's slots below.
namespace engclientslot {
    enum { IsInGame = 25, IsDrawingLoadingImage = 27, ClientCmd_Unrestricted = 108, RecordDemoCustomData = 139 };
}
// The client's demo recorder (CDemoRecorder, engine.dll global `demorecorder`): 4 IsRecording,
// 14 RecordCustomData(int callback_index, const void* data, size_t length).
namespace demorecslot { enum { IsRecording = 4, RecordCustomData = 14 }; }
enum ClientFrameStage_t {
    FRAME_UNDEFINED = -1, FRAME_START = 0,
    FRAME_NET_UPDATE_START, FRAME_NET_UPDATE_POSTDATAUPDATE_START, FRAME_NET_UPDATE_POSTDATAUPDATE_END,
    FRAME_NET_UPDATE_END, FRAME_RENDER_START, FRAME_RENDER_END,
};

// ---- IServerPluginCallbacks (ISERVERPLUGINCALLBACKS003 layout of this engine) ----------------
// The engine's CServerPlugin dispatches to fixed vtable slots regardless of the version string a
// plugin answers to; those slots are the 003 layout, with ClientFullyConnect inserted at slot 10
// (checked against the CServerPlugin::* call offsets in both the Mac and Windows engine).
class IServerPluginCallbacks {
public:
    virtual bool          Load(CreateInterfaceFn interfaceFactory, CreateInterfaceFn gameServerFactory) = 0; // 0
    virtual void          Unload() = 0;                                                                     // 1
    virtual void          Pause() = 0;                                                                      // 2
    virtual void          UnPause() = 0;                                                                    // 3
    virtual const char*   GetPluginDescription() = 0;                                                       // 4
    virtual void          LevelInit(const char* mapName) = 0;                                               // 5
    virtual void          ServerActivate(edict_t* edictList, int edictCount, int clientMax) = 0;            // 6
    virtual void          GameFrame(bool simulating) = 0;                                                   // 7
    virtual void          LevelShutdown() = 0;                                                              // 8
    virtual void          ClientActive(edict_t* entity) = 0;                                                // 9
    virtual void          ClientFullyConnect(edict_t* entity) = 0;                                          // 10
    virtual void          ClientDisconnect(edict_t* entity) = 0;                                            // 11
    virtual void          ClientPutInServer(edict_t* entity, const char* playername) = 0;                   // 12
    virtual void          SetCommandClient(int index) = 0;                                                  // 13
    virtual void          ClientSettingsChanged(edict_t* edict) = 0;                                        // 14
    virtual PLUGIN_RESULT ClientConnect(bool* allow, edict_t* entity, const char* name, const char* address,
                                        char* reject, int maxrejectlen) = 0;                                // 15
    virtual PLUGIN_RESULT ClientCommand(edict_t* entity, const CCommand& args) = 0;                         // 16
    virtual PLUGIN_RESULT NetworkIDValidated(const char* userName, const char* networkID) = 0;              // 17
    virtual void          OnQueryCvarValueFinished(QueryCvarCookie_t cookie, edict_t* player,
                                                   EQueryCvarValueStatus status, const char* cvarName,
                                                   const char* cvarValue) = 0;                              // 18
    virtual void          OnEdictAllocated(edict_t* edict) = 0;                                             // 19
    virtual void          OnEdictFreed(const edict_t* edict) = 0;                                           // 20
};

// ---- helpers ------------------------------------------------------------------------------------
template <class T> inline T vfunc(void* obj, int index) { return (T)((*(void***)obj)[index]); }
