#define _CRT_SECURE_NO_WARNINGS
#include "sarlink.h"
#include "gameapi.h"
#include "snapshot.h"
#include "util.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace sarlink {

namespace {

// ---- demo records -------------------------------------------------------------------------------------
typedef bool (__thiscall* IsRecordingFn)(void*);
typedef void (__thiscall* RecordCustomDataFn)(void*, int callback_index, const void* data, size_t length);
const uint8_t SAR_QUEUED_COMMAND = 0x10;   // SAR record type: a command, as text
void** g_recorder = nullptr;               // engine.dll's `demorecorder` global (a CDemoRecorder*)
bool   g_resolved = false;

// IVEngineClient::RecordDemoCustomData begins with
//   push ebp; mov ebp,esp; mov ecx,[demorecorder]; mov eax,[ecx]; mov edx,[eax+IsRecording*4]
// which gives the address of the global.
void** find_recorder() {
    void* ec = gameapi::Interface("engine.dll", "VEngineClient015");
    if (!ec) return nullptr;
    const uint8_t* f = (const uint8_t*)vfunc<void*>(ec, engclientslot::RecordDemoCustomData);
    static const uint8_t prologue[] = { 0x55, 0x8B, 0xEC, 0x8B, 0x0D };
    if (memcmp(f, prologue, sizeof(prologue)) || f[9] != 0x8B || f[10] != 0x01 || f[11] != 0x8B || f[12] != 0x50 ||
        f[13] != demorecslot::IsRecording * 4)
        return nullptr;
    return *(void***)(f + 5);
}

// ---- HUD line -----------------------------------------------------------------------------------------
int   g_hud_id = 50;
void* g_hud_cmd = nullptr;     // SAR's sar_hud_set_text when the line was last set (null: SAR absent)
char  g_hud_text[400] = "";
unsigned g_frames = 0;
const int HUD_MAX_NAMES = 8;

void compose(char* out, size_t cap) {
    const char* names[snapshot::MAX_SNAPSHOTS];
    int n = snapshot::Names(names, snapshot::MAX_SNAPSHOTS);
    if (!n) {
        _snprintf(out, cap, "Snap: #A0A0A0none");
        out[cap - 1] = 0;
        return;
    }
    const char* cur = snapshot::Current();
    size_t len = 0;
    len += _snprintf(out + len, cap - len, "Snap:");
    for (int i = 0; i < n && i < HUD_MAX_NAMES; i++) {
        bool c = !_stricmp(names[i], cur);
        len += _snprintf(out + len, cap - len, "%s %s%s%s", i ? "," : "", c ? "#80FF80" : "", names[i], c ? "#r" : "");
    }
    if (n > HUD_MAX_NAMES) _snprintf(out + len, cap - len, " +%d", n - HUD_MAX_NAMES);
    out[cap - 1] = 0;
}

} // namespace

void RecordDemoCommand(const char* cmd, const char* name) {
    if (!g_resolved) {
        g_resolved = true;
        g_recorder = find_recorder();
        if (!g_recorder) util::Log("sarlink: demo recorder not found, snapshots are NOT recorded in demos");
    }
    void* rec = g_recorder ? *g_recorder : nullptr;
    if (!rec || !vfunc<IsRecordingFn>(rec, demorecslot::IsRecording)(rec)) return;
    // 8 bytes of radial menu cursor position (zero, as SAR writes until the co-op radial menu records
    // one), the record type, the command and its terminator.
    uint8_t buf[8 + 1 + 16 + snapshot::MAX_NAME + 1] = {};
    buf[8] = SAR_QUEUED_COMMAND;
    int n = _snprintf((char*)buf + 9, sizeof(buf) - 10, "%s %s", cmd, name);
    if (n < 0) return;
    vfunc<RecordCustomDataFn>(rec, demorecslot::RecordCustomData)(rec, 0, buf, 9 + n + 1);
    util::Log("sarlink: '%s %s' recorded in the demo", cmd, name);
}

void SetHudId(int id) { g_hud_id = id; }

void UpdateHud() {
    if (g_hud_id <= 0) return;
    void* sar = gameapi::FindCommand("sar_hud_set_text");
    if (!sar) { g_hud_cmd = nullptr; return; }
    char text[sizeof(g_hud_text)];
    compose(text, sizeof(text));
    if (sar == g_hud_cmd && !strcmp(text, g_hud_text)) return;
    char cmd[512];
    _snprintf(cmd, sizeof(cmd), "sar_hud_set_text %d \"%s\"", g_hud_id, text);
    cmd[sizeof(cmd) - 1] = 0;
    gameapi::RunCommand(cmd);
    g_hud_cmd = sar;
    strcpy(g_hud_text, text);
}

void OnFrame() {
    if (++g_frames % 30 == 0) UpdateHud();
}

} // namespace sarlink
