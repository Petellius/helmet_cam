# Game symbols: find, bind, call

Every game name a plugin uses comes from `DoorKickers2.pdb` and is checked with `symtest` first (R3).

## symtest explorer
```powershell
tools\symtest.exe "<game folder>" --find  "<mask>"   # functions and globals
tools\symtest.exe "<game folder>" --types "<mask>"   # struct/class/union/enum names
tools\symtest.exe "<game folder>" --type  "<name>"   # one type's layout
tools\symtest.exe "<game folder>" --enum  "<name>"   # one enum's values
```
- Masks use `*` and `?` and are case-insensitive. Names are fully qualified (`GameClient::UpdateCamera`, `GUI::Item::FindChild`).
- Each run first prints a few `[hh:mm:ss.mmm] [dk2ml] ...` lines (symbol loading). Skip lines starting with `[`.
- Exit code 0 means found, 1 means no match (the message suggests a mask), 2 means an unknown option.

### `--find` output
```
function  <Name>
global    <Name>
function  <Name>  OVERLOAD: ResolveSymbol("<decorated name>")
function  <Name>  FOLDED with <N> other function(s): a hook runs for all
```
- At most 300 lines, then `... more than 300 matches: narrow the mask`.
- `OVERLOAD`: several functions share the name, and there's one line per overload. Bind with the decorated name from the line you need: `dk2ml::Fn<...> f{"?FindChild@Item@GUI@@QEAAPEAV12@PEBD@Z"}`. The plain name resolves to an arbitrary one, and the log warns.
- `FOLDED`: the linker merged identical bodies. A hook there runs for all of them, so filter on `this` (R6).
- `public`: a symbol that is neither a function nor a global (rare).

### `--types` output
One name per line. Enums are prefixed `enum `. The same enum can be listed with and without its class scope (`enum GameClient::eCGameState`, `enum eCGameState`). Use the scoped name.

### `--type` output
```
<Type>: <size> bytes
  offset  size  type                                     name
      +0     1  bool                                     <field>
      +4     4  int32_t                                  <field>  (bits 3-7)
Base class members are listed under the base: --type <base>.
```
- `(bits a-b)` marks a bitfield. The offset is its storage unit's, so shift and mask yourself.
- `(the PDB has N other layout(s) of it, from another build; GetFieldOffset uses this one)`: other copies are 32-bit layouts. The one shown is what the loader uses.
- Inherited fields aren't listed. Run `--type <base>`. Field offsets are from the start of the object.

### `--enum` output
`  <ENUMERATOR_NAME>  <value>`, one per line. Bind the name, never the value (R2).

## Binding (dk2ml.hpp)
Declare at namespace scope (R4), and resolve in init with `if (!dk2ml::ResolveAll(api)) return 1;`.

| Game thing | Binding | Use | Notes |
|---|---|---|---|
| Function `R Class::Method(A, B)` | `dk2ml::Fn<R(void* self, A, B)> f{"Class::Method"};` | `f(obj, a, b)`; `f.Address()` to hook | `this` is the first parameter. The signature isn't checked, so get it right (see [Signatures](#signatures)). Static and free functions have no `self`. |
| Variadic function | `dk2ml::Fn<R(A, ...)>` | `f(a, x, y)` | |
| Global `T g_x` | `dk2ml::Global<T> g{"g_x"};` | `*g` | The binding holds the variable's address. For `GameClient* g_pGameClient` declare `Global<void*>`, so `*g` is the GameClient or null. Dereference on every use, because the game replaces pointers. |
| Field `T Type::m_x` | `dk2ml::Field<T> f{"Type", "m_x"};` | `f(obj)` is a `T&`; `f.Offset()` | T must match the field's size (`--type`). |
| Nested field `Outer::m_inner.x` | two `Field`s | `At<T>(obj, outer.Offset() + inner.Offset())` | `dk2ml::At<T>(base, offset)` reads any offset. |
| Embedded struct as an object | `dk2ml::Field<uint8_t> f{"Outer", "m_inner"};` | `(char*)obj + f.Offset()` | Only `Offset()` is used. The template does this for `GameClient::m_camera`. |
| Type size | `dk2ml::TypeSize s{"Type"};` | `s.Get()` | |
| Enumerator | `dk2ml::Enum e{"Scope::eEnum", "ENUMERATOR"};` | `e.Get()` (int64) | The C++ enumerator name, not the GUI XML name (`ACTION_ADD_CHILD`, not `AddChild`). |
| Nice-to-have name | add `dk2ml::Optional` as the last constructor argument | check `.Resolved()` before use | Missing is logged, and init still succeeds. |

Missing names log `missing function <name>`, `missing global <name>`, `missing field <Type>::<field>`,
`missing type <Type>` or `missing enum <Enum>::<name>`. Then `ResolveAll` logs
`N required and M optional name(s) not found in this game build (listed above)`.

The C API equivalents (`ResolveSymbol`, `GetFieldOffset`, `GetTypeSize`, `GetEnumValue`) return NULL, -1, 0 and
`DK2ML_ERROR` on failure. They work from any thread.

## Calling convention
x64 Microsoft ABI, as it applies to game functions:

| Item | Where |
|---|---|
| Arguments 0–3, integer or pointer | `rcx`, `rdx`, `r8`, `r9` |
| Arguments 0–3, float or double | `xmm0`–`xmm3`, by the same position (argument 1 as a float is `xmm1`) |
| Arguments 4+ | stack. In a hook, `regs->stack[n + 1]` for argument n |
| `this` | argument 0 |
| Struct over 8 bytes, or not 1/2/4/8 bytes, passed by value | pointer to a caller-made copy. Declare it as `const T*` |
| Class or struct returned by value | hidden result pointer as argument 0, or argument 1 after `this`. The function returns that pointer. Declare `T* f(void* self, T* result, ...)` |
| Integer or pointer result | `rax` |
| Float or double result | `xmm0` |
| Arguments under 64 bits | only the low bytes are defined. `dk2ml::Arg<T>` handles it, and in C cast `(int)DK2ML_Arg(r, n)` |

Example: `Vector3 GameClient::ConvertScreenToMapCoords(float x, float y) const` binds as
`dk2ml::Fn<Vector3*(const void* self, Vector3* result, float x, float y)>`. In a hook, `x` is argument 2 (`xmm2`) and
`y` is argument 3 (`xmm3`).

## Signatures
`--find` prints a decorated name only for overloads. A decorated name encodes the full signature. Decode it with
`undname`, which ships with MSVC and is on `PATH` in a VS developer shell:
```powershell
undname "?FindChild@Item@GUI@@QEAAPEAV12@PEBD@Z"
# public: class GUI::Item * __ptr64 __cdecl GUI::Item::FindChild(char const * __ptr64) __ptr64
```
For a function that isn't overloaded, get its decorated name from a local publics dump, then run `undname`:
```powershell
llvm-pdbutil dump -publics "<game folder>\DoorKickers2.pdb" > publics.txt   # LLVM; keep the file local (R25)
Select-String -Path publics.txt -Pattern 'UpdateCamera@GameClient@@' -SimpleMatch
# ... S_PUB32 [size = 52] `?UpdateCamera@GameClient@@AEAAXH@Z`
undname "?UpdateCamera@GameClient@@AEAAXH@Z"
# private: void __cdecl GameClient::UpdateCamera(int) __ptr64   ->  dk2ml::Fn<void(void* self, int dt)>
```
`private`/`protected` don't matter to a hook or a call.
- The decorated form is `?<Method>@<Class>@<Namespace>@@...`, innermost name first.
- `tools\disasm.ps1 -Names '?UpdateCamera@GameClient'` disassembles functions by decorated-name substring, with call targets named. It needs LLVM and that `publics.txt` next to the script, or `-Publics <path>`. Use it to see which registers a function reads (its real arguments) and how callers use its result.
- Without LLVM: ask the human for the signature, or keep the hook to arguments whose meaning is clear (`this`).

## Before hooking a function: checklist
1. `--find` shows it as `function`, without `FOLDED`. If it's folded, plan a `this` filter.
2. Signature known (above). Arguments counted with `this` as argument 0.
3. Thread: does the game call it on the main thread? Render and update functions do. If unsure, don't call game functions from the callback.
4. Call frequency: per frame, per object or per render pass. A function like `Camera::SetDefaults` runs for every temporary render camera, so compare `this` with the object you want.
5. Is there an event for it? [api-map.md](api-map.md#events). Use it instead (R9).
