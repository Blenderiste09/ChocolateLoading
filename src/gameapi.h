// Thin wrappers around the engine interfaces the plugin talks to: interface lookup, console variables
// and commands (ICvar), and the client engine (IVEngineClient). Main thread only.
#pragma once
#include "sdk.h"

namespace gameapi {

// CreateInterface lookup in an already loaded module, e.g. ("engine.dll", "VEngineClient015").
void* Interface(const char* module, const char* name);

// Resolves ICvar and checks its layout. Must succeed before the functions below are used.
bool InitConsole();

ConVar* FindVar(const char* name);   // the parent ConVar (the one that holds the value), or null
void AddCommand(const char* name, const char* help, FnCommandCallback_t callback);
void RemoveCommands();               // unregisters every command added by AddCommand

// Queues a console command exactly as if typed in the console (it runs later in the frame).
void RunCommand(const char* cmd);
// True when fully signed on to a map and no loading screen is shown.
bool InGame();

} // namespace gameapi
