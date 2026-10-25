# dk2ml plugin docs for agents

Scope: writing, building and checking a dk2ml plugin (a mod with code for *Door Kickers 2: Task Force North*). Written
for coding agents first, people second.

## Facts
- dk2ml is a mod loader. Players put `dk2ml.dll` and a `dbghelp.dll` stub in the game folder.
- A plugin is a 64-bit Windows DLL that exports `int DK2ML_PluginInit(const DK2ML_API* api, const DK2ML_PluginInfo* info)`. It ships in a game mod's `native\` folder.
- The plugin doesn't link against the loader. Everything goes through the `DK2ML_API` function table passed to init.
- Game functions, globals, fields, type sizes and enum values are resolved by name from `DoorKickers2.pdb`, which ships with the game. Plugins hardcode no addresses.
- `DK2ML_PluginInit` runs once, on the game's main thread, before any game code. No game objects exist yet. Return 0 to stay loaded.
- Language: C++17 (C works too), MSVC x64, static CRT, Release builds.

## Where things are
| What | Template zip (`dk2ml-template-for-devs-v<ver>.zip`) | Loader repository |
|---|---|---|
| C API, the reference | `dk2ml\dk2ml.h` | `include/dk2ml.h` |
| C++ layer (bindings, typed args, `gui::`) | `dk2ml\dk2ml.hpp` | `include/dk2ml.hpp` |
| Plugin source | `src\Plugin.cpp` | `template/src/Plugin.cpp` |
| Mod folder contents (`mod.xml`, `gui\`) | `mod\` | `template/mod/` |
| Build, install and package scripts | `tools\build.ps1`, `install.ps1`, `package.ps1` | `template/tools/` |
| PDB explorer and dry run | `tools\symtest.exe` | built as `build\symtest.exe` |
| Disassembler script (needs LLVM) | `tools\disasm.ps1` | `tools/disasm.ps1` |
| Minimal C plugin | `docs\ExamplePlugin.cpp` | `docs/ExamplePlugin.cpp` |
| These docs | `docs\for-agents\` | `docs/for-agents/` |

`dk2ml.h` is the authority on signatures, threading and failure values. These docs don't repeat signatures.

## Read order
1. [rules.md](rules.md): always.
2. [workflow.md](workflow.md): always. Build, dry run, install, and what counts as done.
3. The file for the task (below). For working code, [cookbook.md](cookbook.md).

## Task routing
| Task | Read |
|---|---|
| Find a game function, field, global or enum; bind it; call a game function | [symbols.md](symbols.md), code: [cookbook.md](cookbook.md#call-a-game-function) |
| Run code every frame, on mission start, on state change, on GUI reload, on window resize | [api-map.md](api-map.md#events) |
| Change what a game function does (hook) | [api-map.md](api-map.md#hooks), then [symbols.md](symbols.md#calling-convention), code: [cookbook.md](cookbook.md#hook-a-function-with-pre-and-post) |
| Add settings for the player | [api-map.md](api-map.md#options) |
| Add or change GUI (buttons, text, menus) | [api-map.md](api-map.md#gui), code: [cookbook.md](cookbook.md#put-a-button-into-the-esc-menu), [other menus](cookbook.md#put-a-button-into-another-game-menu) |
| Draw an ImGui window | [cookbook.md](cookbook.md#imgui-window) |
| Hotkeys | [api-map.md](api-map.md#input) |
| Work from another thread | [api-map.md](api-map.md#threads-and-tasks), code: [cookbook.md](cookbook.md#worker-thread--main-thread) |
| Talk to another plugin | [api-map.md](api-map.md#interfaces), code: [cookbook.md](cookbook.md#offer-or-use-an-interface) |
| Build, dry run or install fails; plugin doesn't load; behavior is wrong in game | [troubleshooting.md](troubleshooting.md) |
| The game updated and names are missing | [troubleshooting.md](troubleshooting.md#after-a-game-update) |

## What an agent can and can't verify
| Can (no game running) | Can't (needs the human) |
|---|---|
| Compile (`.\tools\build.ps1`) | Anything in game: behavior, visuals, timing |
| Check every name against the real PDB (`symtest --find/--type/--enum`) | Whether a hook fires when expected |
| Run `DK2ML_PluginInit` against the real PDB (the dry run) | Crashes after init |
| Install to `mods_upload` while the game is closed (`.\tools\install.ps1`) | Enabling the mod in the game's Mods menu |
| Read `dk2ml.log` and crash reports after the human played | Publishing to the Steam Workshop |

The dry run is the strongest check available without the game. A change isn't done until the dry run passes. In-game
behavior is unverified until the human reports back.
