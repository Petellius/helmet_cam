# Cookbook

Code recipes for common plugin tasks. Each recipe holds only what no other doc covers. Rules are cited as `R<n>`.
API facts are in [api-map.md](api-map.md), and binding and calling-convention facts in [symbols.md](symbols.md).
All bindings below are at namespace scope (R4) and resolved by `dk2ml::ResolveAll` in init. Check every name with
`symtest` first (R3).

## Read game state
```cpp
dk2ml::Global<void*> g_pGameClient{"g_pGameClient"};
dk2ml::Field<int> GameClient_m_state{"GameClient", "m_state"};
dk2ml::Enum StateRunning{"GameClient::eCGameState", "CGAMESTATE_RUNNING"};

void* client = *g_pGameClient; // null early on
bool inMission = client && GameClient_m_state(client) == StateRunning.Get();
```
Nested fields, bitfields and the global-address rule: [symbols.md](symbols.md#binding-dk2mlhpp).

## Call a game function
```cpp
struct Vector3 { float x, y, z; }; // must match the game's layout (R7): symtest --type Vector3

// void Camera::UpdateViewMatrix()
dk2ml::Fn<void(void* camera)> UpdateViewMatrix{"Camera::UpdateViewMatrix"};
// Vector3 GameClient::ConvertScreenToMapCoords(float x, float y) const: hidden result pointer after `this`
dk2ml::Fn<Vector3*(const void* client, Vector3* result, float x, float y)> ScreenToMap{"GameClient::ConvertScreenToMapCoords"};

Vector3 at;
ScreenToMap(client, &at, mouseX, mouseY); // main thread only (R5)
```

## Hook a function with pre and post
```cpp
dk2ml::Fn<void(void* camera)> SetDefaults{"Camera::SetDefaults"};

int Pre(DK2ML_Regs* regs, void*)
{
    void* camera = dk2ml::Arg<void*>(regs, 0);
    regs->scratch[0] = reinterpret_cast<uint64_t>(camera); // handed to Post for this call
    return DK2ML_CALL_ORIGINAL;                            // or SetResult, then DK2ML_SKIP_ORIGINAL (R10)
}

void Post(DK2ML_Regs* regs, void*)
{
    void* camera = reinterpret_cast<void*>(regs->scratch[0]);
    (void)camera; // dk2ml::Result<T>(regs) is the original's result
}

// in DK2ML_PluginInit: creates and enables
if (!dk2ml::Hook(api, SetDefaults, Pre, Post)) return 2;
```
`Camera::SetDefaults` runs for every temporary render camera, so compare `camera` with the object you want (R6).
Chain order, skipping and containment: [api-map.md](api-map.md#hooks).

## React to mission start and end
```cpp
dk2ml::Enum StateRunning{"GameClient::eCGameState", "CGAMESTATE_RUNNING"};

void OnState(const DK2ML_Event* e, void*)
{
    if (e->newState == StateRunning.Get()) { /* a mission is running */ }
    else if (e->oldState == StateRunning.Get()) { /* it ended: menu, loading or restart */ }
}

// in DK2ML_PluginInit (R11):
dk2ml::On(api, DK2ML_EVENT_STATE_CHANGED, OnState);
dk2ml::On(api, DK2ML_EVENT_FRAME, [](const DK2ML_Event*, void*) { /* every frame */ });
```
- Troop placement: `GameGUI::m_deploySlots` holds elements exactly while troops are being placed. The game's own deploy code relies on it.
- Other events: [api-map.md](api-map.md#events).

## Hotkey
`OnFrame` in the template's `src\Plugin.cpp` is the reference: focus check, press edge, a `DK2ML_OPTION_KEY` option.
The rules are in [api-map.md](api-map.md#input).

## Settings with shipped defaults
Files in `native\` are part of what Workshop players approve, and changing them triggers a new permission prompt. To
ship defaults, put a defaults file in `native\`. On first start, copy it to `api->GetConfigDir()`, and from then on
read and write only the copy (R16). The load, save and `AddOption` pattern is in the template's `src\Plugin.cpp`.

## Put a button into the Esc menu
`AddMenuButton` (API 2) in init. The loader makes the button in both Esc menus (single missions and campaigns), shares
rows of 3 above the menu's own buttons among all plugins, and calls `onClick` on the main thread. No GUI XML, no frame handler:
```cpp
// in DK2ML_PluginInit (R11); icons are 64x64 textures in mod\textures\gui\ (or set label = "CAM" instead)
DK2ML_MenuButton b = {sizeof(b)};
b.icon = "data/textures/gui/mymod_normal.dds";
b.iconHover = "data/textures/gui/mymod_hover.dds";
b.tooltip = "My Mod settings";
b.onClick = [](void*, float, float, void*) { g_windowOpen = !g_windowOpen; };
dk2ml::gui::AddMenuButton(api, b);
```
Declare `DK2ML_PLUGIN_MANIFEST(2, ...)`, or check the result: on an API 1 loader `gui::AddMenuButton` returns false.
Esc stays open after the click, so clicks on an ImGui window opened from it would also reach the Esc menu below.
Capture input while ImGui uses it, not for as long as the window is open (R28,
[api-map.md](api-map.md#imgui-window-input)).

## Put a button into another game menu
The XML item goes in `mod\gui\<file>.xml` with a prefixed name (R18). An item has one parent, so move it every frame
it isn't in the target yet:
```cpp
// in a DK2ML_EVENT_FRAME handler
void* button = dk2ml::gui::Find(api, "#mymod_button");
void* menu = dk2ml::gui::Find(api, "Menu_Ingame");
void* row = menu ? dk2ml::gui::Find(api, "Extra Buttons", menu) : nullptr; // "Extra Buttons" exists in several menus
if (button && row && dk2ml::gui::Parent(api, button) != row && dk2ml::gui::AddChild(api, row, button))
    dk2ml::gui::Show(api, button);
```

## React to a click (or hover, scroll, drag)
XML: a `Callback` action under the event. The target must be an existing item.
```xml
<OnClick><Action type="Callback" target="#mymod_button"/></OnClick>
```
Code, after every `GUI_LOADED` (R12):
```cpp
dk2ml::Enum EVENT_CLICK{"GUI::Item::eItemEventType", "EVENT_CLICK"};

dk2ml::gui::OnAction(api, button, EVENT_CLICK.Get(), [](void* item, float x, float y, void*) { /* clicked */ });
```

## React to, or block, a game GUI event
```cpp
dk2ml::Enum GUI_GAME_CUSTOMIZE_OPENED{"GUI::Events::eEventType", "GUI_GAME_CUSTOMIZE_OPENED"};

// in DK2ML_PluginInit:
dk2ml::OnGuiEvent(api, GUI_GAME_CUSTOMIZE_OPENED.Get(), [](const DK2ML_Event* e, void*) { /* e->guiEventParams */ });
```
The game's `EventSystem::TriggerEvent` calls every listener, newest first, so no listener can stop the game's own
handling. To block one (for example a BACK that should close your panel instead of the screen), safe-hook the game's
handler, such as `GameGUI::Customize_OnEvent`, and skip it for that event.

## ImGui window
The game links Dear ImGui and renders it every frame. Draw from a `DK2ML_EVENT_FRAME` callback, which runs inside
ImGui's frame.
- Resolve the functions by name: `ImGui::Begin`, `ImGui::End`, `ImGui::Checkbox`, `ImGui::Text`, and so on.
- Some were inlined away and don't exist, among them `SetNextWindowPos`, `CollapsingHeader` and `PopID`. Check with `--find "ImGui::*"`, and use the underlying function: `ButtonEx` for `Button`, `SliderScalar` for `SliderFloat`, `SeparatorEx` for `Separator`.
- Bind flags by name. `ImGuiWindowFlags_` and the other flag enums are in the PDB (`--enum ImGuiWindowFlags_`), and their values change between ImGui versions (R2).
- Match ImGui's types exactly (`struct ImVec2 { float x, y; };`), and pass by reference where the real signature does: `ButtonEx(const char*, const ImVec2&, int)` takes a pointer.
- Clicks on the window also reach the map. Capture input while ImGui uses it (`WantCaptureMouse` / `WantCaptureKeyboard`), not while the window is open: a capture blocks the whole map and HUD (R28, [api-map.md](api-map.md#imgui-window-input)).
- The Modloader screen exists only on the main menu, so in-mission settings need a window like this, opened from an
  [Esc-menu button](#put-a-button-into-the-esc-menu). Free Camera has one.

## Worker thread → main thread
```cpp
struct Result { int value; };

void ApplyResult(void* user) // main thread, start of the next frame
{
    auto* r = static_cast<Result*>(user);
    /* may call the game here */
    delete r;
}

// on the worker thread
dk2ml::Post(api, ApplyResult, new Result{42}); // user stays valid until the task runs
```
Limits and failure cases: [api-map.md](api-map.md#threads-and-tasks).

## Offer or use an interface
```cpp
// public header shared with other mods: append fields only, and raise the version when you do
struct MyModApi { uint32_t structSize; bool (*IsActive)(); void (*SetZoom)(float); };

// publisher, in DK2ML_PluginInit
static const MyModApi kApi = {sizeof(MyModApi), IsActive, SetZoom};
dk2ml::Publish(api, "author.mymod.Api", 1, &kApi);

// user, from DK2ML_EVENT_PLUGINS_LOADED on; look it up on every use
if (auto* m = dk2ml::Get<MyModApi>(api, "author.mymod.Api", 1)) m->SetZoom(2.0f);
```
Naming and lifetime rules: [api-map.md](api-map.md#interfaces).

## Repeat a plugin's state in replays
`AddReplayData` (API 3) in init. Record each change with its game time while playing; the loader asks for the bytes when
the game writes the replay and hands them back when it plays. While replaying, work the state out from the start at
the current game time, so skipping back undoes later changes:
```cpp
dk2ml::Global<void*> g_pGameClient{"g_pGameClient"};
dk2ml::Field<int> GameCommon_m_gameTime{"GameCommon", "m_gameTime"}; // ms

struct Change { int32_t timeMs; uint8_t on; };
bool g_startOn = false;            // at mission start (reset in MAP_LOADED)
std::vector<Change> g_changes;     // this mission, in game-time order
bool g_replayStartOn = false;      // the replay being watched
std::vector<Change> g_replayChanges;

void SetOn(bool on) // the player's switch, in a mission
{
    if (void* client = *g_pGameClient) g_changes.push_back({GameCommon_m_gameTime(client), on ? uint8_t(1) : uint8_t(0)});
}

bool On() // in play or in a replay
{
    void* client = *g_pGameClient;
    const bool replaying = dk2ml::IsReplaying(api);
    bool on = replaying ? g_replayStartOn : g_startOn;
    for (const Change& c : replaying ? g_replayChanges : g_changes) {
        if (client && c.timeMs > GameCommon_m_gameTime(client)) break;
        on = c.on != 0;
    }
    return on;
}

void SaveReplay(DK2ML_ReplayWriter* out, void*)
{
    dk2ml::ReplayOut w{api, out};
    w.Put<uint16_t>(1); // this format's version
    w.Put<uint8_t>(g_startOn ? 1 : 0);
    w.Put<uint32_t>(static_cast<uint32_t>(g_changes.size()));
    for (const Change& c : g_changes) { w.Put(c.timeMs); w.Put(c.on); }
}

void LoadReplay(const void* data, size_t size, void*)
{
    g_replayStartOn = false; // none, or a version this one doesn't know: the defaults
    g_replayChanges.clear();
    dk2ml::ByteReader r{data, size};
    uint16_t version = 0;
    uint8_t startOn = 0;
    uint32_t count = 0;
    if (!r.Get(&version) || version != 1 || !r.Get(&startOn) || !r.Get(&count)) return;
    for (uint32_t i = 0; i < count; ++i) {
        Change c;
        if (!r.Get(&c.timeMs) || !r.Get(&c.on)) { g_replayChanges.clear(); return; }
        g_replayChanges.push_back(c);
    }
    g_replayStartOn = startOn != 0;
}

// in DK2ML_PluginInit (R11)
dk2ml::AddReplayData(api, "author.mymod", SaveReplay, LoadReplay);
```
`LoadReplay` reads bytes that may come from another player's file, so it checks every read and keeps nothing on a
failure. Declare `DK2ML_PLUGIN_MANIFEST(3, ...)`, or check the result: on an older loader `AddReplayData` returns false, and
replays show the defaults. Rules: [api-map.md](api-map.md#replays).

## Debug with Visual Studio
1. `.\tools\build.ps1 -Config RelWithDebInfo`, then install.
2. The human starts the game and attaches: Visual Studio > Debug > Attach to Process > `DoorKickers2.exe` (R23). Breakpoints in the plugin bind once it's loaded, and game frames have names from the game's PDB.
3. `api->Log` also goes to the debugger output, so Sysinternals DebugView shows it live.
