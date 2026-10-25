# Troubleshooting

Symptom → cause → fix. Exact output strings are in code spans. `<...>` marks a variable part.

## Build
| Symptom | Cause | Fix |
|---|---|---|
| `MSVC x64 build tools not found` | VS 2022 / Build Tools with the C++ workload is not installed | The human installs it |
| `dk2ml.h/dk2ml.hpp not found in <dir>` | the headers aren't in `dk2ml\` | Restore them from the template zip, or configure with `-DDK2ML_INCLUDE=<folder>` |
| `Door Kickers 2 is 64-bit; configure with an x64 toolchain` | 32-bit toolchain | Use `.\tools\build.ps1`, which sets up x64 |
| CMake errors about paths after moving the folder | CMake caches absolute paths | Delete `build\` |

## Dry run output
| Output | Cause | Fix |
|---|---|---|
| `cannot map exe (<n>)` | wrong game folder | Pass the folder that contains `DoorKickers2.exe` |
| `cannot load <path>` | no `DK2ML_PluginInit` export, a 32-bit DLL, or a dependency DLL not found | Define init as `DK2ML_EXPORT int DK2ML_PluginInit(...)`. Build x64. Put dependencies next to the DLL or link them statically |
| `missing function <name>` / `missing global <name>` | the name is wrong or not in this game build | `--find "<partial>*"`. Use the fully qualified name, or the decorated name for an overload |
| `missing field <Type>::<field>` | wrong type or field name, or the field is in a base class | `--type <Type>`, then `--type <base>` |
| `missing type <Type>` / `missing enum <Enum>::<name>` | wrong name or scope | `--types "*<part>*"`, `--enum <Enum>`. Enumerators are C++ names, not XML names |
| `[dk2ml] WARNING: <name> is <N> different functions (overloads?)` | an undecorated overloaded name | Bind the decorated name from `--find` (`OVERLOAD: ResolveSymbol("?...")`) |
| `[dk2ml] WARNING: <Type>::<field> is a bitfield` | the field is a bitfield | Read the storage unit and shift/mask by the bits given |
| `[dry run] PROBLEM: CreateSafeHook(NULL)` | hooking an unresolved address | Check the `ResolveSymbol` result, or use `dk2ml::Fn` + `ResolveAll` |
| `[dry run] PROBLEM: EnableHook/DisableHook(<p>): no safe hook was created on it` | `EnableHook` without `CreateSafeHook` on that target | Create first, or use `dk2ml::Hook` |
| `[dry run] PROBLEM: AddOption "<label>": <reason>` | an invalid option (see the reason) | Fix per [api-map.md](api-map.md#options) |
| `[dry run] WARNING: AddOption "<label>": format "<f>" isn't one plain <kind> conversion` | a bad `format` | One `%f`/`%g`/`%e` (FLOAT) or `%d`/`%i` (INT), plain text around it, under 64 characters |
| `[dry run] PROBLEM: Subscribe(<n>): <reason>` | unknown type, `GUI_EVENT` via `Subscribe`, or NULL fn | Use `SubscribeGuiEvent` for GUI events |
| `[dry run] PROBLEM: SubscribeGuiEvent(<n>): ...` | id 0, an id out of range, or past this build's last event | Resolve the id with `dk2ml::Enum{"GUI::Events::eEventType", ...}` |
| `[dry run] PROBLEM: PublishInterface...` | invalid or duplicate name, NULL table | 1–63 printable ASCII, no spaces, published once |
| `[dry run] PROBLEM: GetInterface("<n>") during init always returns NULL` | lookup during init | Look it up from `PLUGINS_LOADED` on |
| `[dry run] PROBLEM: AddTask(NULL)` | NULL task function | Pass a function |
| `[dry run] PROBLEM: manifest ...` | the manifest is malformed or needs a newer API | Use `DK2ML_PLUGIN_MANIFEST(1, ...)` with a non-empty name |
| `[dry run] <n> GUI kit call(s) during init: there is no GUI yet (use GUI_LOADED)` | GUI kit used in init (R12) | Move it to a `GUI_LOADED` or `FRAME` handler |
| `<N> required and <M> optional name(s) not found in this game build (listed above)` | a summary of `missing ...` lines | Fix each line above it |
| `plugin init returned <N>`, N ≠ 0, no `missing` lines | a step in init failed (a hook or subscription) | Find which `return N`. The `[dk2ml]` lines above say why |

## Plugin doesn't load (`dk2ml.log`)
| Log | Cause | Fix |
|---|---|---|
| no `dk2ml.log` in the game folder | the loader isn't installed | The human copies `dk2ml.dll` and `dbghelp.dll` into the game folder |
| no line mentioning the plugin | the mod isn't enabled in the Mods menu, or the DLL isn't in `<mod>\native\` | Enable it, check the install path, restart the game |
| `<mod title>: enabled in the Mods menu after the game started: its native code loads at the next start` | enabled mid-session | Restart the game |
| `<dir> has DLLs but none exports DK2ML_PluginInit ...` | the export is missing | `DK2ML_EXPORT int DK2ML_PluginInit(...)` |
| `failed to load <path> (error 126)` | a dependency DLL is missing | Ship it in `native\` or link statically |
| `failed to load <path> (error 193)` | not a 64-bit DLL | Build x64 |
| `not loading <path>: a plugin named <file> is already loaded from <dir>` | another mod has the same DLL name | Rename the project (R17) |
| `not loading <path>: it needs plugin API <n>, ...` | the manifest's `minApiVersion` is above the installed loader's | Lower it to the loader's API version, or the human updates the loader |
| `skipping the native code in <dir>: neither one of your mod folders nor a Door Kickers 2 Workshop item` | installed somewhere the loader doesn't trust | Install with `.\tools\install.ps1` (to `mods_upload`) |
| `player chose Quit at the Workshop prompt (...): closing the game, nothing saved` | the player chose Quit; the game closed before any plugin loaded | Start again and choose Continue, or unsubscribe, or set `enabled=0` in `<game>\dk2ml.ini` and disable the mod in the Mods menu |
| `<path> init returned <N>; its hooks are switched off` | init returned non-zero | Run the dry run, which shows why |
| `<path> crashed during init (exception 0x<code>)` | crash in init, often a null binding or GUI or game access in init | Dry run. Don't touch game objects in init (R12) |
| `<path> looked up <N> name(s) this game build doesn't have...` | missing names in game | [After a game update](#after-a-game-update) |
| game shows a message box and starts unmodded | crash in the plugin's `DllMain` or in the loader | Keep `DllMain` empty. The last `loading <path>` line names the DLL. `enabled=0` under `[loader]` in `<game>\dk2ml.ini` starts the game without plugins |

## Wrong behavior in game
| Symptom | Cause | Fix |
|---|---|---|
| A hook runs for unrelated objects or far too often | the function serves many callers, or `FOLDED` | Filter on `this` (R6) |
| A hook never fires | wrong overload bound, inlined at the relevant call sites (LTCG), or the game uses another path | Check `--find` overloads. Disassemble the callers (`disasm.ps1`). Hook a function that really is called |
| Rendering corruption or crashes in game code after adding a hook | a plain detour (R1), a skipped original whose result or out-parameters weren't set, or a written argument of the wrong type | Safe hooks only. Set the result with `SetResult` when skipping. Check the signature ([symbols.md](symbols.md#signatures)) |
| Garbage values from `Field` | wrong `T` size, a 32-bit layout assumed, or a field read through the wrong object | Check `--type`. Check that the object is the type you think |
| `GuiFind` returns NULL | called before `GUI_LOADED`, the name is wrong, or the item lives in another menu | Find after `GUI_LOADED`. Scope the search: `Find(api, name, menu)` |
| Items stop working after the GUI reloads | they were found before a `GUI_LOADED` | Find and bind them again in every `GUI_LOADED` |
| `GuiSetText` fails | the item isn't exactly a StaticText or Button | Target the StaticText itself |
| `GuiSetCallback` / `OnAction` fails | no `<Action type="Callback">` under that event in the XML | Add it. The target must be an existing item |
| A cloned item's callback doesn't fire | bound after cloning | Bind on the template before cloning |
| Clicks on the plugin's ImGui window also act on the map | input not captured | `CaptureGameInput(1)` while ImGui uses the input (`WantCaptureMouse` / `WantCaptureKeyboard`), `0` otherwise (R28) |
| With the plugin's window open, troopers can't be selected and the HUD doesn't respond | input captured for as long as the window is open | Capture only while ImGui uses the input (R28) |
| A hotkey fires in other windows | `GetAsyncKeyState` is global | Check `IsGameFocused()` |
| A hotkey repeats while held | acting on the "down" state | Act on the press edge |
| A hotkey also fires inside another mod's Ctrl/Alt combination (e.g. Ctrl+Z turns the camera) | the key is read without looking at modifiers | Ignore it while Ctrl or Alt is held (R27) |
| Settings lost after a Workshop update | saved in the mod folder | Save in `GetConfigDir()` (R16) |
| A value jitters | another mod writes the same field | Write only while the feature is active. Back off if the value isn't what you last wrote |
| `<name>.dll crashed in its <where> (exception 0x<code>)`, then `... switched off for this session` | a crash in a callback | Fix the crash. The plugin stays off until the game restarts |
| `<mod title>: disabled in the Mods menu, so switched off for this session ...` | the player disabled the mod | Expected. The game also drops the mod's GUI files. Game changes must not depend on the plugin to undo (R14) |
| `WARNING: <a> and <b> both ship <dll>, as different files` | a dependency DLL name clash | Rename or statically link (R19) |
| The game crashes, and a `dk2ml-crash-*.log` exists | unhandled crash | Read its `In a mod's code:` and `Stack` sections. Map `<name>.dll+0x<off>` with the plugin's PDB (`RelWithDebInfo` build) |
| The game crashes with no crash report | the crash happened below a post-hooked call, which can't be unwound | Reproduce with post callbacks disabled to narrow it down |
| Online co-op desyncs | gameplay changed on one machine only | R20 |

## After a game update
1. Dry run against the updated game. Every missing name is listed (`missing ...`).
2. For each one, find its new name: `--find "*<part>*"`, `--types`, `--type`, `--enum`.
3. A name can stay while its signature changes. Recheck the signatures of hooked and called functions (`undname` on the current decorated name, [symbols.md](symbols.md#signatures)).
4. Update `gameVersion` in `mod\mod.xml` and the manifest to the version symtest prints (`game version: <N>`). Rebuild, dry run, and have the human test.
5. Workshop players are asked for permission again, because the code changed.
