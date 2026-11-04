# Function reference

All calls go through `janet_eval(text)` (superuser only; the VM is per-connection
and persists across statements in a session).

Calls that can hit a Postgres error return a tagged tuple: `(:ok value)` or
`(:error @{:message "..." :sqlstate "..."})`.

**Type keywords** (for `call`, `closure`, `getarg`):
`:void :int :uint :int64 :uint64 :bool :pointer :datum :cstring :bytes :double :float`.
Aliases: `:int32`→`:int`, `:uint32`→`:uint`, `:ptr`→`:pointer`, `:string`→`:cstring`,
`:buf`→`:bytes`. `:int64`/`:uint64` are exact 64-bit integers; `:cstring` is a
Janet string ↔ `char *`; `:bytes` is the data pointer of a string (in) or buffer
(in/out); `:void` is a return type only.

---

## Symbols & introspection

**`(scalpel/resolve name)`** → `<pointer>` | nil
Address of a symbol. Exported names resolve without debug info; statics need it.

**`(scalpel/syms pattern &opt limit)`** → `@[{:name :addr} ...]`
Function symbols matching a glob (`*`/`?`), up to `limit` (default 1000).

**`(scalpel/sym-from-addr addr)`** → `{:name :addr :displacement}` | nil
Nearest preceding function symbol for a code address, and the offset into it.

**`(scalpel/sig name)`** → `{:name :addr :ret :args :call}` | nil
Function signature from DWARF. `:call` is `(ret (args...))`, ready for `call`.

**`(scalpel/struct name)`** → `{:name :size :fields [{:name :offset :size :call} ...]}` | nil
Struct layout from DWARF (tag name or a typedef that resolves to one).

**`(scalpel/image-range &opt module)`** → `{:base :size :text :textsize}` | nil
Base/size of a loaded module (default: main executable) and its `.text`.

**`(scalpel/disasm addr &opt len)`** → `@[{:addr :size :bytes :text} ...]`
Disassemble up to `len` bytes (default 16) of live x86-64 code at `addr`.

**`(scalpel/scan start size pattern &opt mask)`** → `@[<pointer> ...]`
Find `pattern` in `[start, start+size)`. A `0` in the same-length `mask` is a
wildcard. The range must be readable or the backend faults.

**`(scalpel/backtrace &opt maxframes)`** → `@[{:addr :name :displacement} ...]`
Symbolized C stack (default 32 frames).

## Calling C

**`(scalpel/call addr ret-type [arg-types] args...)`** → `(:ok v)` | `(:error {...})`
Call the C function at `addr` via libffi, guarded (a callee `ereport` becomes
`(:error …)`). The signature must match the ABI — a wrong one crashes the backend
(a SIGSEGV the guard cannot catch); read it with `sig`.

**`(scalpel/call-raw addr ret-type [arg-types] args...)`** → `(:ok v)` | `(:error {...})`
Unguarded `call`. Only for leaf functions that cannot `ereport`.

**`(scalpel/closure ret-type [arg-types] fn)`** → `<pointer>`
A C function pointer whose body runs `fn`. Lives for the backend's life (never
freed). Use it as a hook detour or any C callback.

## fmgr replacement

A closure standing in for a builtin gets one `:pointer` — the `FunctionCallInfo`.

**`(scalpel/getarg fcinfo n type)`** → value | nil
Read the n-th fmgr argument as `type`; `nil` for SQL NULL. No varlena detoasting.

**`(scalpel/setresult-isnull fcinfo bool)`** → nil
Make the replacement return SQL NULL.

**`(scalpel/recv type-name bytes)`** → `(:ok text)` | `(:error {...})`
Decode `bytes` with a type's binary-input function (guarded); returns the value's
text rendering.

## Changing behavior

> How `hook`/`hook-fn` and the trampoline actually work: [hooking.md](hooking.md).

**`(scalpel/hook target-addr detour-ptr)`** → `<trampoline>`
Inline-hook `target-addr` to `detour-ptr`. Returns a trampoline — call it with the
target's signature to run the original. Panics if already hooked or too short.

**`(scalpel/unhook target-addr)`** → nil
Remove a hook; restore the original bytes.

**`(scalpel/intercept-rows f | nil)`** → nil
Per-row output filter for later queries this session (`nil` clears). `f` gets the
row's columns as text (`nil` = NULL) and returns the same-length array: a string
replaces (reparsed via the column's input fn), `nil` forces NULL, else unchanged.

**`(scalpel/page-protect addr len writable?)`** → nil
Make a range writable+exec (`true`) or read+exec (`false`); page-aligned. Errors
if `mprotect` fails.

**`(scalpel/flush-icache addr len)`** → nil
Flush the icache after writing code. No-op on x86-64, real on arm64.

## Errors

**`(scalpel/raise msg)`** → `(:error {...})`
Raise a test error through the guard; always returns `(:error …)`.

---

# Prelude (`janet-lib/`, auto-loaded)

## Structs

**`(make-struct name values)`** → `@buffer`
Pack `values` (table keyed by field name) into a buffer sized by `(struct name)`.

**`(pack-struct layout values)`** → `@buffer`
As `make-struct`, given a `layout` from `scalpel/struct`.

**`(read-struct layout ptr)`** → `@{...}`
Decode the bytes at `ptr` into field name → value.

**`(scalpel-load relpath)`** → value
Load a `.janet` file relative to `$HOME/pg_scalpel` and return its last value.

## Hooking & patching

`target` here accepts a name, address, or pointer.

**`(hook target detour)`** → `<trampoline>` / **`(unhook target)`**
Name-aware wrappers over `scalpel/hook`/`scalpel/unhook`.

**`(hook-fn target ret-type arg-types &named on-enter on-leave)`** → handle
Wrap `target`, still calling the original. `on-enter (fn [args])` observes;
`on-leave (fn [args ret])` returns the (possibly replaced) result. `(unhook target)`.

**`(replace-fn target ret-type arg-types fn)`** → handle
Replace `target` with `fn`; the original never runs. `(unhook target)`.

**`(trace pattern ret-type arg-types &named limit stack)`** → handle
Hook every match (one shared signature) and log `@{:fn :args :ret :stack}` to
`trace-log`; `:stack true` adds a backtrace. `(untrace handle)`.

**`(untrace handle)`** → count — remove a trace's hooks.
**`(trace-clear)`** — empty `trace-log`.   **`trace-log`** — the log array.

**`(patch-jump target dest)`** → handle
Reroute `target` to `dest` with a static jump stub (no trampoline). `(unpatch h)`.

**`(patch-return-int target n)`** / **`(patch-return-zero target)`** → handle
Overwrite the prologue to ignore args and return `n` (or 0). `(unpatch h)`.

**`(install target stub)`** → handle — overwrite the prologue with `stub` bytes,
saving the original. **`(unpatch handle)`** — restore them.

**`(peek-bytes addr n)`** → `@buffer` — read `n` bytes.
**`(poke-bytes addr bytes)`** → addr — write bytes (flip page, copy, flush, restore).
**`(ptr->int p)`** → number — numeric address of a pointer.
