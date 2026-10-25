# AGENTS.md

This project is a dk2ml plugin: a mod with code (a DLL) for *Door Kickers 2: Task Force North*, loaded by the Door
Kickers 2 Modloader. C++17, MSVC x64, CMake, PowerShell scripts.

**Before writing code, read `docs\for-agents\README.md`.** It routes each task to the right file. The rules in
`docs\for-agents\rules.md` apply to every change.

## Layout
| Path | What |
|---|---|
| `src\Plugin.cpp` | the plugin. `DK2ML_PluginInit` is the entry point |
| `mod\` | the mod folder: `mod.xml`, optional `gui\*.xml` |
| `dk2ml\dk2ml.h`, `dk2ml\dk2ml.hpp` | the loader's API (reference) and the C++ layer. Don't edit them |
| `tools\build.ps1`, `install.ps1`, `package.ps1` | build, dev install (with a dry run), Workshop layout. Run them from this folder |
| `tools\symtest.exe` | game symbol explorer and plugin dry run |
| `docs\for-agents\` | docs for agents: rules, workflow, symbols, API map, cookbook (code recipes), troubleshooting |

## Commands (PowerShell, from this folder)
| Step | Command | Done when |
|---|---|---|
| Build | `.\tools\build.ps1` | exit 0, `build\<name>.dll` exists |
| Dry run | `tools\symtest.exe "<game folder>" build\<name>.dll` | prints `plugin init returned 0` and exits 0 |
| Check a name | `tools\symtest.exe "<game folder>" --find "<mask>"` (`--type`, `--enum`, `--types`) | the name is listed |
| Install (game closed) | `.\tools\install.ps1 [-GameDir "<game folder>"]` | `Installed to ...` |

The default game folder is `C:\Program Files (x86)\Steam\steamapps\common\DoorKickers2`. `<name>` is
`project(<name> ...)` in `CMakeLists.txt`.

## Top rules (full list: `docs\for-agents\rules.md`)
1. Hook game functions only with `CreateSafeHook` / `dk2ml::Hook`. Never plain detours or byte patches (R1).
2. No hardcoded addresses, offsets, sizes or enum values. Everything is resolved by name from the PDB (R2).
3. Check every game name with `symtest` before using it (R3).
4. Bindings (`dk2ml::Fn/Global/Field/TypeSize/Enum`) at namespace scope, then `dk2ml::ResolveAll(api)` in init (R4).
5. Game functions, the GUI kit and ImGui only on the main thread. No game objects or GUI during init (R5, R12).
6. `AddOption`, `Subscribe`, `SubscribeGuiEvent`, `PublishInterface`, `AddMenuButton` and `AddReplayData` only inside `DK2ML_PluginInit` (R11).
7. Save files only under `api->GetConfigDir()`, never in the mod folder (R16).
8. Prefer loader events to hooks. Filter hooks by `this` (R6, R9).
9. A change is done only when the dry run passes (R22). In-game behavior is unverified until the human tests it.
10. Never launch or kill the game, never publish, never commit PDB dumps or disassembly (R23–R25).
11. Hotkeys: a plain key does nothing while Ctrl or Alt is held; a combination checks exactly its modifiers (R27).
12. ImGui windows capture game input only while ImGui uses it (`WantCaptureMouse` / `WantCaptureKeyboard`), never just
    because they're open (R28).
