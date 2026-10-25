// Minimal dk2ml plugin in plain C: logs camera updates (at most once per interval), with two settings on the
// Modloader screen (main menu > Modloader > your mod).
// Build a 64-bit DLL (MSVC: cl /LD /O2 /I<folder with dk2ml.h> ExamplePlugin.cpp) into <your mod>\native\, enable the
// mod in the Mods menu, then watch dk2ml.log in the game folder.
#include <windows.h>

#include <stdbool.h>

#include "dk2ml.h"

/* optional manifest, shown to players: min plugin API, name, version, author, url, game version */
DK2ML_PLUGIN_MANIFEST(1, "Example plugin", "1.0.0", "dk2ml", "", 112);

static const DK2ML_API* g_api;
static DWORD g_lastLog;

// settings: the loader shows and changes these. A real plugin saves them in OnChanged, under GetConfigDir().
static bool g_logEnabled = true;
static float g_intervalSeconds = 1.0f;

// GameClient::UpdateCamera(this, int dt): `this` is arg 0 (rcx), dt arg 1 (edx). Safe hooks get registers (see
// dk2ml.h); DK2ML_Arg / DK2ML_ArgFloat read arguments by position.
static int UpdateCameraPre(DK2ML_Regs* regs, void* user)
{
    if (g_logEnabled && GetTickCount() - g_lastLog > (DWORD)(g_intervalSeconds * 1000)) {
        g_lastLog = GetTickCount();
        g_api->Log("GameClient::UpdateCamera(dt=%d)", (int)DK2ML_Arg(regs, 1));
    }

    return DK2ML_CALL_ORIGINAL;
}

static void OnChanged(const DK2ML_Option* option, void* user)
{
    g_api->Log("setting changed: %s", option->label);
}

DK2ML_EXPORT int DK2ML_PluginInit(const DK2ML_API* api, const DK2ML_PluginInfo* info)
{
    g_api = api;

    // functions by name, from the DoorKickers2.pdb shipped with the game
    void* target = api->ResolveSymbol("GameClient::UpdateCamera");
    if (!target) {
        return 1;
    }

    // hooks start disabled
    if (api->CreateSafeHook(target, UpdateCameraPre, NULL, NULL) != DK2ML_OK) {
        return 2;
    }
    if (api->EnableHook(target) != DK2ML_OK) {
        return 2;
    }

    // shown as the game's own checkbox and slider
    DK2ML_Option log = {sizeof(DK2ML_Option), DK2ML_OPTION_BOOL, "Log camera updates", "Writes to dk2ml.log",
                        &g_logEnabled};
    log.onChange = OnChanged;
    api->AddOption(&log);

    DK2ML_Option interval = {
        sizeof(DK2ML_Option), DK2ML_OPTION_FLOAT, "Log interval", NULL, &g_intervalSeconds, 0.1f, 10.0f, "%.1f s"};
    interval.onChange = OnChanged;
    api->AddOption(&interval);

    // type sizes and field offsets by name too
    api->Log("Camera is %u bytes, m_rotAngles at +%d", api->GetTypeSize("Camera"),
             api->GetFieldOffset("Camera", "m_rotAngles"));

    return 0;
}
