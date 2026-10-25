# Workflow

The loop: edit → build → dry run → install → the human tests in game → read the log. Run the commands from the
template root, the folder with `CMakeLists.txt`, in PowerShell.

## 0. One-time setup (new project)
1. Rename `project(my_plugin ...)` in `CMakeLists.txt` to a distinctive name (R17). It names `build\<name>.dll`, the installed mod folder and the settings folder.
2. Fill in `mod\mod.xml` (`title`, `description`, `author`, `gameVersion`) and the `DK2ML_PLUGIN_MANIFEST(...)` line at the top of `src\Plugin.cpp`.
3. Toolchain: Visual Studio 2022 or its Build Tools with the C++ workload (MSVC x64, CMake, Ninja). `tools\build.ps1` finds it through `vswhere`.
4. Game folder: default `C:\Program Files (x86)\Steam\steamapps\common\DoorKickers2`. If it's elsewhere, ask the human and pass it as shown below.

## 1. Build
```powershell
.\tools\build.ps1                           # Release (default)
.\tools\build.ps1 -Config RelWithDebInfo    # with a PDB, for attaching a debugger
```
- Output: `build\<name>.dll`.
- Success: exit code 0. Failure: the script throws `build failed (<code>)` after the compiler output.
- `MSVC x64 build tools not found`: the toolchain is missing. The human has to install it.
- CMake caches absolute paths. If the project folder moved, delete `build\` and build again.

## 2. Dry run
```powershell
tools\symtest.exe "<game folder>" build\<name>.dll
```
It loads the real `DoorKickers2.pdb` and runs `DK2ML_PluginInit` with real lookups and stubbed hooks. Nothing is
hooked, and no game code runs.

Output, in order:
1. Lines starting with `[hh:mm:ss.mmm] [dk2ml]`: symbol loading, and lookup warnings such as overloads and bitfields.
2. The loader's own name checks (`loader menu symbols: ok`, `events: ...`, `GUI kit: ok; ...`, `game version: 112`, ...). These are about the loader, not the plugin. Ignore them unless they say `MISSING`.
3. The plugin's part: `  [dry run] manifest: ...`, the plugin's own `Log` lines as `  [plugin] ...`, `  [dry run] PROBLEM: ...`, `  [dry run] N event subscription(s)`.
4. `plugin init returned <N>`.

| Exit code | Meaning | Action |
|---|---|---|
| 0 | init returned 0, nothing refused | Pass. |
| 100 | init returned 0, but calls were made that the real loader refuses (`PROBLEM` lines) | Fix every `PROBLEM`. |
| 1 | `cannot load <path>` (not a loadable DLL, or no `DK2ML_PluginInit` export), `cannot map exe` (wrong game folder), or init returned 1 | Read the last lines. |
| other | init's own return value | Find that `return` in `DK2ML_PluginInit`. The lines above name the failed step (`missing field ...` and so on). |

**Done = `plugin init returned 0` and exit code 0.** A `[dry run] N GUI kit call(s) during init` line is a bug even
at exit 0 (R12). Fixes for each line are in [troubleshooting.md](troubleshooting.md#dry-run-output).

## 3. Install (dev)
```powershell
.\tools\install.ps1                                         # default game folder
.\tools\install.ps1 -GameDir "D:\Games\DoorKickers2"        # other game folder
```
- Preconditions: `build\<name>.dll` exists, and the game is **not running**. Otherwise it throws `Close Door Kickers 2 first (it locks the DLL).` Ask the human to close the game, and never kill it (R23).
- It runs the dry run first and stops on a non-zero exit (`symtest dry run failed (exit code N)`).
- It copies `mod\*` and the DLL to `%LOCALAPPDATA%\KillHouseGames\DoorKickers2\mods_upload\<name>\` (`native\<name>.dll`). Files from earlier installs stay, so delete stale ones there by hand.
- Mods in `mods_upload` load without the Workshop permission prompt.

## 4. In game (the human)
Ask the human to:
1. Enable the mod once in the game's Mods menu. A mod enabled after the game started loads its code only at the next start, so restart the game through Steam after enabling it.
2. Reproduce the behavior. The main menu's **Modloader** screen shows whether each plugin loaded, and why not.
3. Close the game and share the relevant lines of `<game folder>\dk2ml.log`, or the whole file.

## 5. Logs and reports
| File | Contents |
|---|---|
| `<game folder>\dk2ml.log` | This session's loader log. Plugin `Log` lines read `[hh:mm:ss.mmm] [<name>.dll] message`. |
| `<game folder>\dk2ml.prev.log` | The previous session's log. |
| `<game folder>\dk2ml-crash-<date>-<time>.log` | Unhandled-crash report: the faulting mod, the stack, registers, the mod list. Plugin frames show as `<name>.dll+0x1234`. The 10 newest are kept. |
| `%LOCALAPPDATA%\KillHouseGames\DoorKickers2\log.txt` | The game's own log. GUI XML errors appear here. |
| `symtest.log` (current directory) | symtest's log, the same `[dk2ml]` lines it prints. |

Loader lines about a plugin, from `dk2ml.log`:
- `loading <path>`, then `loaded <path> (...)`: success.
- `<path> init returned N; its hooks are switched off`
- `<path> crashed during init (exception 0x...)`
- `<path> looked up N name(s) this game build doesn't have...`
- `<name>.dll crashed in its <where> (exception 0x...)` or `safe hook ...: ... callback crashed`, then `... switched off for this session`. The plugin is off until the game restarts.

## 6. Publishing (the human)
Agents prepare and the human publishes (R24):
1. `.\tools\package.ps1` builds Release and writes `dist\<name>\` plus `dist\<name>.sha256.txt`.
2. Keep `gameVersion` in `mod\mod.xml` and in the manifest equal to the game version symtest prints (`game version: N`).
3. The human copies `dist\<name>` to `%LOCALAPPDATA%\KillHouseGames\DoorKickers2\mods_upload\<name>` and uploads it from the game's Mods menu. The Workshop page must say that the mod needs the Door Kickers 2 Modloader.

Checklist:
- A Release build, a clean dry run (exit 0), tested in game from `mods_upload`.
- The mod folder: `mod.xml` (`title`, `description`, `gameVersion`), `native\<name>.dll` and its dependencies, an optional `gui\`, and usually a `mod_image.jpg` preview.
- `DK2ML_PLUGIN_MANIFEST(minApi, name, version, author, url, gameVersion)` filled in. `minApi` is `DK2ML_API_VERSION` of the `dk2ml.h` built with. The loader reads it without running code and shows it on the Modloader screen, in `dk2ml.log` and in crash reports.
- The Workshop page says the mod needs the Door Kickers 2 Modloader and links it. Without the loader, a plugin silently does nothing.
