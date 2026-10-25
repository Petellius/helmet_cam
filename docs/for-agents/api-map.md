# API map

Task → call → when it may be called → failure value. The signatures are in `dk2ml.h` (C) and `dk2ml.hpp` (C++
helpers). "Init" means only inside `DK2ML_PluginInit`. "Main" means the main thread: hook callbacks of main-thread
functions, events, tasks and GUI callbacks.

## Pick the least invasive way
| Need | Use | Section |
|---|---|---|
| Code every frame, on state change, map load, GUI reload, window resize | event | [Events](#events) |
| React to a game GUI event (screen opened, `TriggerEvent`) | GUI event | [GUI](#gui) |
| A button in the Esc menu (e.g. to open in-mission settings) | `AddMenuButton` | [GUI](#gui) |
| Buttons, text, menus | GUI XML in `mod\gui\` + GUI kit | [GUI](#gui) |
| Player settings | `AddOption` | [Options](#options) |
| State a replay should repeat (a switch the player flipped mid-mission) | `AddReplayData` | [Replays](#replays) |
| Read or write game state | bindings + `Field` | [symbols.md](symbols.md#binding-dk2mlhpp) |
| Call a game function | `dk2ml::Fn` | [symbols.md](symbols.md#binding-dk2mlhpp) |
| Change what a game function does, or observe one no event covers | safe hook | [Hooks](#hooks) |

## Lookups
| Task | C | C++ | When | Failure |
|---|---|---|---|---|
| Function or global address | `ResolveSymbol(name)` | `Fn`, `Global` | any thread | NULL |
| Field offset | `GetFieldOffset(type, field)` | `Field` | any thread | -1 |
| Type size | `GetTypeSize(type)` | `TypeSize` | any thread | 0 |
| Enum value | `GetEnumValue(enum, name, &out)` | `Enum` | any thread | `DK2ML_ERROR` |
| Resolve all bindings | n/a | `ResolveAll(api)` | init | false, with each missing name logged |

## Hooks
| Task | C | C++ | When | Failure |
|---|---|---|---|---|
| Create callbacks (start disabled) | `CreateSafeHook(target, pre, post, user)` | n/a | init, or any thread later | `DK2ML_ERROR`, logged; also once the plugin was switched off |
| Create and enable | n/a | `dk2ml::Hook(api, fn, pre, post, user)` | init, or any thread later | false (target NULL: nothing logged) |
| Enable or disable own callbacks | `EnableHook(target)` / `DisableHook(target)` | n/a | any thread | `DK2ML_ERROR` if no safe hook exists on target |

Callbacks:
```cpp
int  Pre(DK2ML_Regs* regs, void* user);   // return DK2ML_CALL_ORIGINAL (0) or DK2ML_SKIP_ORIGINAL (1)
void Post(DK2ML_Regs* regs, void* user);  // after the original returned
```
| Task | C++ (C) |
|---|---|
| Read argument n (`this` = 0) in pre | `dk2ml::Arg<T>(regs, n)` (`DK2ML_Arg`, `DK2ML_ArgFloat`, `DK2ML_ArgDouble`) |
| Change argument n for the original | `dk2ml::SetArg<T>(regs, n, v)` (`DK2ML_SetArg`, `DK2ML_SetArgFloat`) |
| Read the result in post | `dk2ml::Result<T>(regs)` (`regs->rax`, `DK2ML_ResultFloat`) |
| Set the result in post, or in a pre that skips | `dk2ml::SetResult<T>(regs, v)` (`DK2ML_SetResult`, `DK2ML_SetResultFloat`) |
| Pass data from pre to post for one call | `regs->scratch[0..3]` (uint64) |

Facts:
- Read arguments in the pre. In the post, the argument registers hold whatever the function left.
- `T` for `Arg`/`Result` must fit in 8 bytes and be trivially copyable. A struct over 8 bytes is a pointer (`const T*`).
- Multiple plugins can hook the same target. Pres run in creation order, which follows the mods' order in the Mods menu, and posts in reverse. A skipping pre ends the chain for that call: later callbacks don't run, the skipper's own post doesn't run, and earlier callbacks' posts run with the skipper's result. The log names each skipper once.
- A post always runs if its pre ran, even if the hook was disabled in between.
- Nesting: posts are tracked 64 hooked calls deep per thread. Deeper, callbacks with a post are skipped entirely.
- Crash containment: a crash in a callback is caught, the call continues unhooked, and all of the plugin's hooks, options and GUI callbacks are switched off for the session. Not covered: the plugin's own threads, damage done before the crash, and a crash in a game function the callback called while a post-hooked call sits between them on the stack.
- Register-level details: `DK2ML_Regs` in `dk2ml.h`. Full example: `src\Plugin.cpp` (`UpdateCameraPre`).

## Events
Subscribe in init. Callbacks run on the main thread in subscription order, under crash containment.
`dk2ml::On(api, type, fn, user)` wraps `Subscribe`. A captureless lambda works. Subscribing twice gives two callbacks.

| `DK2ML_EVENT_...` | When | Event fields |
|---|---|---|
| `FRAME` | every frame, inside ImGui's frame; not while a random map generates | `gameClient`, `newState` = the current state |
| `STATE_CHANGED` | `GameClient::m_state` changed, checked once per frame | `oldState`, `newState` (`GameClient::eCGameState`; -1 = no GameClient) |
| `MAP_LOADED` | a mission starts or restarts | `gameClient` |
| `GUI_LOADED` | the game (re)built its GUI. Items found before are gone | |
| `PLUGINS_LOADED` | once, after every plugin's init, before game code. `GetInterface` works from here on | `gameClient` is NULL |
| `WINDOW_RESIZED` | the window's client size changed | `width`, `height` |
| `GUI_EVENT` | a subscribed game GUI event. Use `SubscribeGuiEvent`, not `Subscribe` | `guiEventId`, `guiEventParams` |

- Compare states by name: `dk2ml::Enum StateRunning{"GameClient::eCGameState", "CGAMESTATE_RUNNING"}`, then `e->newState == StateRunning.Get()`. The main menu is `CGAMESTATE_MAIN_MENU`.
- `api->GetGameState()` gives the current state at any time.
- If this game build lacks the names an event needs, the loader logs it and the event never arrives.
- Failure: `DK2ML_ERROR` for an unknown type, `GUI_EVENT`, a NULL fn, or a call after init.

## Options
Settings on the plugin's page of the main menu's Modloader screen, drawn with the game's widgets. `AddOption` is
init only and adds in call order. The plugin owns the values. The loader reads `*value` when the screen opens, writes
it when the player changes it, then calls `onChange`, where the plugin saves.

| `DK2ML_OPTION_...` | `value` | Widget | Requirements |
|---|---|---|---|
| `HEADER` | none | section title | |
| `BOOL` | `bool*` | checkbox | |
| `FLOAT` | `float*` | slider | `max > min`; optional `format`, one `%f`/`%g`/`%e` |
| `INT` | `int*` | slider | `max > min`; optional `format`, one `%d`/`%i` |
| `CHOICE` | `int*` (index) | left/right selector | `choices` with `choiceCount` 1–64 |
| `KEY` | `int*` (virtual-key code) | rebind button | |
| `BUTTON` | none | button | `onChange` runs on click |

- Set `structSize = sizeof(DK2ML_Option)`. `value` must point to static storage. Strings are copied, up to 512 bytes.
- Failure: `DK2ML_ERROR`, with the reason logged. The dry run prints `[dry run] PROBLEM: AddOption "<label>": <reason>`.
- Save to `api->GetConfigDir()` (R16). Pattern: `LoadSettings` before `AddOption`, `SaveSettings` as `onChange`. See `src\Plugin.cpp`.
- The screen exists on the main menu only. In-mission settings need an ImGui window ([cookbook.md](cookbook.md#imgui-window)), opened from an Esc-menu button (`AddMenuButton`).

## GUI
Mod GUI: every `.xml` in the mod's `gui\` folder (template: `mod\gui\`) is loaded. Its top-level items join the GUI.
XML rules: action targets must name existing items, `type="None"` is rejected, and `TriggerEvent` accepts only
`GUI::Events::eEventType` names. The game's `data\gui\_config.xml` documents the elements and action types. XML
errors go to the game's `log.txt`.

GUI kit: main thread only, from `GUI_LOADED` on. Every function accepts NULL items. If the kit is unavailable in this
game build, everything returns NULL, 0, `""` or `DK2ML_ERROR`.

| Task | C | C++ (`dk2ml::gui::`) | Failure |
|---|---|---|---|
| Find an item by name (recursive; NULL = whole GUI) | `GuiFind(under, name)` | `Find(api, name, under)` | NULL |
| Parent, name, children | `GuiParent`, `GuiName`, `GuiChildren` | `Parent`, `Name`, `Children` | NULL, `""`, 0 |
| Shown flag, show or hide | `GuiIsShown`, `GuiShow` | `IsShown`, `Show` | |
| Set text (StaticText, or Button's 3 state texts) | `GuiSetText(item, utf8)` | `SetText` | `DK2ML_ERROR` for other types. Text is cut to the XML text's length |
| Move under a parent (`ACTION_ADD_CHILD`) | `GuiAddChild(parent, item)` | `AddChild` | `DK2ML_ERROR`. Doesn't show the item |
| Set origin (relative to parent center, +y up) | `GuiSetOrigin(item, x, y)` | `SetOrigin` | `DK2ML_ERROR` |
| Run OnClick actions | `GuiClick(item)` | `Click` | `DK2ML_ERROR` |
| Bind `<Action type="Callback">` to a function | `GuiSetCallback(item, itemEvent, fn, user)` | `OnAction(api, item, itemEvent, fn, user)` | `DK2ML_ERROR` if the item has no Callback action for that event |
| Game GUI event (`GUI::Events::eEventType`) | `SubscribeGuiEvent(id, fn, user)` (init) | `dk2ml::OnGuiEvent(api, id, fn, user)` | `DK2ML_ERROR`, false for id ≤ 0 |
| Button in the Esc menus (API 2) | `AddMenuButton(&button)` (init) | `AddMenuButton(api, button)` | `DK2ML_ERROR`, logged; false on an API 1 loader |

- Item callbacks need XML: `<OnClick><Action type="Callback" target="<an existing item>"/></OnClick>`. Then, after `GUI_LOADED`: `dk2ml::Enum EVENT_CLICK{"GUI::Item::eItemEventType", "EVENT_CLICK"}` and `gui::OnAction(api, item, EVENT_CLICK.Get(), fn)`.
- A clone copies callbacks. Bind on the template before cloning.
- GUI events arrive after the game handled them and can't block it. To block one, safe-hook the game's handler.
- Esc-menu buttons: `DK2ML_MenuButton` with `structSize`, `onClick`, and an `icon` (64×64 texture path, optional `iconHover`) or a short `label`; optional `tooltip`. Strings are copied. The loader makes the items in both Esc menus (`Menu_Ingame`, `Menu_Ingame_Campaign`) and puts every plugin's buttons in rows of 3 above the menu's own buttons, in load order, each row from the left, going up; up to 45 (more are hidden). They hide when the plugin is switched off. Code: [cookbook.md](cookbook.md#put-a-button-into-the-esc-menu).
- Putting an item into another game menu: an item has one parent. Each frame, if `Parent(item) != target`, call `AddChild`, then `Show`. Code: [cookbook.md](cookbook.md#put-a-button-into-another-game-menu).
- Coordinates: a 2560×1440 canvas, (0,0) at the center, +y up. Don't compute screen positions from `GUI::Item::m_globalOrigin`.

## Input
| Task | Call |
|---|---|
| Is the game window focused | `api->IsGameFocused()`. Check it before `GetAsyncKeyState`, which sees keys from every window |
| Is a game menu open (ignores captures) | `api->IsGameMenuOpen()` / `dk2ml::GameMenuOpen(api)` |
| Keep clicks and keys on your ImGui window off the game | `api->CaptureGameInput(1)` / `0`, or `dk2ml::CaptureInput(api, bool)`, only while ImGui uses the input (R28, [pattern](#imgui-window-input)) |
| Rebindable key | `DK2ML_OPTION_KEY` ([Options](#options)). The Modloader screen marks a key bound by another plugin too with "(also <mod>)", so pick unusual defaults |
| Key combination (Ctrl+Z) | A `DK2ML_OPTION_KEY` holds one key: pair it with a `DK2ML_OPTION_CHOICE` modifier (None/Ctrl/Shift/Alt), or capture the whole combination in your own window. Act only when exactly that modifier is held (R27) |
| Game window (HWND) | `api->GetGameWindow()`, NULL before it exists |

Hotkey pattern: in `FRAME`, `down = IsGameFocused() && (GetAsyncKeyState(vk) & 0x8000)`. Act on `down && !wasDown`,
then store `wasDown = down`. Skip while `IsGameMenuOpen()`. There's no API for "the player is typing".

A plain key MUST NOT act while Ctrl or Alt is held (R27): that press is another mod's or the game's combination. Keep
`wasDown` on the raw state, so a key held through the combination waits for its next press:
```cpp
bool down = api->IsGameFocused() && (GetAsyncKeyState(vk) & 0x8000) != 0;
bool chord = vk != VK_CONTROL && vk != VK_MENU && ((GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_MENU)) & 0x8000);
if (down && !wasDown && !chord) { /* act */ }
wasDown = down;
```
A combination hotkey instead checks that exactly its modifiers are held (Ctrl, Shift and Alt each down or up as bound).

### ImGui window input
Capture makes the game's `IsAnyMenuOpened` true, so the game takes no clicks or keys at all: no selecting troopers, no
HUD. Capture only while ImGui uses the input (R28). ImGui sets `WantCaptureMouse` (the mouse over one of its windows,
or an item held) and `WantCaptureKeyboard` (an item active, text typed) in its NewFrame, and `FRAME` runs after it, so
the flags are the current frame's. `ImGui::GetIO` was inlined away, so they're read through the context:
```cpp
dk2ml::Global<void*> GImGui{"GImGui"}; // ImGuiContext*
dk2ml::Field<uint8_t> ImGuiContext_IO{"ImGuiContext", "IO"}; // ImGuiIO, inside the context
dk2ml::Field<bool> ImGuiIO_WantCaptureMouse{"ImGuiIO", "WantCaptureMouse"};
dk2ml::Field<bool> ImGuiIO_WantCaptureKeyboard{"ImGuiIO", "WantCaptureKeyboard"};

bool g_capturing = false;

// in FRAME, before drawing the window
void* context = *GImGui;
int io = ImGuiContext_IO.Offset();
bool capture = g_windowOpen && context &&
               (g_waitingForKeyToBind || dk2ml::At<bool>(context, io + ImGuiIO_WantCaptureMouse.Offset()) ||
                dk2ml::At<bool>(context, io + ImGuiIO_WantCaptureKeyboard.Offset()));
if (capture != g_capturing && dk2ml::CaptureInput(api, capture)) {
    g_capturing = capture;
}
```
Another plugin's ImGui window under the mouse sets the flags too. That's harmless: the game only loses the clicks
that land on an ImGui window.

## Replays
A replay repeats the orders the game recorded, not what a plugin changed. To have a replay repeat a plugin's state too,
keep it in the replay file (API 3). The loader appends one block after the game's data, where the game stops reading,
with each plugin's bytes under its key. The block goes along when the player saves the replay under a name.

| Task | C | C++ | When | Failure |
|---|---|---|---|---|
| Keep data in replays | `AddReplayData(&data)` | `dk2ml::AddReplayData(api, key, save, load, user)` | init | `DK2ML_ERROR`, logged; false on an API 1/2 loader |
| Write in `save` | `ReplayWrite(out, data, size)` | `dk2ml::ReplayOut w{api, out}; w.Put<T>(v)` | inside `save` only | `DK2ML_ERROR`, logged; past 1 MiB nothing of it is stored |
| Read in `load` | `data`, `size` | `dk2ml::ByteReader r{data, size}; r.Get(&v)` | inside `load` | `Get` false once the data runs out |
| Is a replay playing | `IsReplaying()` | `dk2ml::IsReplaying(api)` | main | 0/false without a GameClient or on an API 1/2 loader |

- `save(out, user)` runs when the game has just written the mission's replay file (`last_replay_N.rpl`): write the mission's data. Nothing written: nothing stored.
- `load(data, size, user)` runs when a replay starts, restarts or is skipped through: that key's bytes, or NULL and 0 if the replay has none (an older replay, or the plugin wasn't running). Reset to the defaults then. `data` is valid only during the call.
- Keep the replay's state apart from what the mission records. Watching right after a mission, `save` runs during the replay's start and `load` then gets those bytes. `load` can run more than once per start, its order with `MAP_LOADED` isn't fixed, and `IsReplaying` may not be set yet inside it.
- The bytes may come from another player's replay file: check every count and size, and read flags as integers (`ByteReader::Get` refuses `bool` and enums).
- The key is 1–63 printable ASCII characters without spaces, e.g. `author.mod`. Never change it: replays saved with it are read back by it.
- The bytes are the plugin's own format. Start them with a version number and skip what a newer version added.
- Record changes with the game time (`GameCommon::m_gameTime`) and, while replaying, work out the state at the current game time from the start each time: the player can skip back. Code: [cookbook.md](cookbook.md#repeat-a-plugins-state-in-replays).
- `save` and `load` are crash-contained. They don't run once the plugin is switched off. If this game build lacks the replay names, the loader logs it and neither runs.

## Threads and tasks
| Call | Allowed from |
|---|---|
| `AddOption`, `Subscribe`, `SubscribeGuiEvent`, `PublishInterface`, `AddMenuButton`, `AddReplayData` | init only |
| `CreateSafeHook` | init, or any thread later |
| `EnableHook`, `DisableHook`, `Log`, lookups, `GetInterface`, `AddTask` | any thread, any time |
| GUI kit, game functions, ImGui, `ReplayWrite`, `IsReplaying` | main thread only |

- `AddTask(fn, user)` / `dk2ml::Post(api, fn, user)` runs `fn(user)` on the main thread at the start of the next frame, under crash containment, in the order added. `user` must stay valid until it runs.
- Failure: `DK2ML_ERROR` if fn is NULL, 4096 tasks are queued, the plugin was switched off, or this game build has no frame tick.
- Hook callbacks run on whatever thread the game calls the function on.
- The plugin's own threads aren't crash-contained, keep running after the plugin is switched off (give them their own way to stop), and must not touch game state.

## Interfaces
Plugin-to-plugin function tables.

| Task | C | C++ | When | Failure |
|---|---|---|---|---|
| Publish a table | `PublishInterface(name, version, table)` | `dk2ml::Publish(api, name, version, &table)` | init | `DK2ML_ERROR` if the name is taken or invalid |
| Look one up | `GetInterface(name, minVersion, &version)` | `dk2ml::Get<T>(api, name, minVersion)` | from `PLUGINS_LOADED` on | NULL (always NULL during init) |

- The name is 1–63 printable ASCII characters without spaces, for example `author.mod.Api`. The table's first field is `uint32_t structSize`, new fields go at the end, and the version goes up when fields are added. The table must be static.
- Look the table up on every use. It becomes NULL once the publisher is switched off.
- `dk2ml.log` lists every published interface and its publisher.
- Code: [cookbook.md](cookbook.md#offer-or-use-an-interface).

## Other
| Task | Call | Notes |
|---|---|---|
| Log a line | `api->Log(fmt, ...)` | printf-style, to `dk2ml.log`, prefixed with the DLL name. Each line is flushed, so don't log every frame |
| Settings folder | `api->GetConfigDir()` | `%LOCALAPPDATA%\KillHouseGames\DoorKickers2\dk2ml\<dll name>\`, trailing `\`, `""` if unavailable |
| Game folder | `api->GetGameDir()` | trailing `\` |
| Plugin and mod paths | `info->pluginPath`, `info->pluginDir`, `info->modDir` | read-only use (R16). `modDir` is empty for `mods_native` |
| Game state | `api->GetGameState()` | -1 = no GameClient |
| Game version | `api->GetGameVersion()` | e.g. 112, 0 if unknown |
| Loader API version | `api->apiVersion` | `DK2ML_API_VERSION` (2 added `AddMenuButton`; 3 added `AddReplayData`, `ReplayWrite`, `IsReplaying`) |
| Manifest | `DK2ML_PLUGIN_MANIFEST(minApi, name, version, author, url, gameVersion)` | file scope, once. Read without running code |
