# Observation: reading a running backend

The tool has two halves. [hooking.md](hooking.md) covers changing what the
backend does; this covers reading what's there — symbols, type layouts,
machine code, memory, and stacks — from inside the live process.

## Reading the binary instead of a header

The three introspection primitives answer "where is it, what shape is it, how do
I call it" by reading the binary the backend is running, not source:

- **`scalpel/resolve name`** turns a name into an address. Exported symbols go
  through `dlsym`; static/file-local ones come from the symbol table (and DWARF),
  which is why those need a debug build or a `-dbgsym` / `postgres.debug` file.
- **`scalpel/struct name`** reads a struct's layout — field offsets, sizes, and a
  type keyword per field — out of DWARF.
- **`scalpel/sig name`** reads a function's signature out of DWARF, and hands back
  a `:call` entry already shaped for `scalpel/call`.

The point is that this is the layout *this* binary was compiled with, not what a
header says it should be. A struct that gained a field between minor releases, a
`#ifdef`'d member, 32- vs 64-bit padding — `scalpel/struct` reports what's
actually in the running image. You get it with no source tree on the box, and
you never hand-transcribe a signature and hope (which, for `scalpel/call`, is the
difference between a correct call and a backend-crashing one — see the ABI note
in [hooking.md](hooking.md)).

What it can't see: anything the compiler erased. At `-O2` a small static function
is inlined into its caller and has no address to resolve. The *types* survive in
DWARF even then; the function doesn't.

## Symbolizing addresses

The reverse direction — an address back to a name:

- **`scalpel/sym-from-addr addr`** → the nearest preceding function symbol and the
  byte offset into it. Use it to make a raw pointer legible.
- **`scalpel/backtrace`** → the current C stack, each frame run through
  `sym-from-addr`. Capture it from inside a hook's `on-enter` to see who called
  the function you're watching.

## Reading live machine code and memory

- **`scalpel/disasm addr len`** disassembles the actual bytes at `addr` (x86-64,
  via capstone). Because it reads live memory, it shows a *patched* prologue, a
  funchook trampoline, or JIT-generated code — the real instructions, not what a
  static tool sees on disk.
- **`scalpel/image-range`** gives the base and `.text` extent of the loaded image,
  which is the readable range you hand to `scan`.
- **`scalpel/scan start size pattern &opt mask`** searches a byte range for a
  pattern (a `0` in the same-length `mask` is a wildcard) and returns the matching
  addresses.

Both `disasm` and `scan` read process memory directly. Pass a valid, readable
range — a resolved code address, or `image-range`'s `:text`. An unmapped range
faults the backend; there's no guard against a bad pointer here.

## Watching a family of calls: `trace`

`trace` is the observability workflow built on top of the hook machinery. It
hooks every symbol matching a glob with one shared signature and logs each call —
function, arguments, return, optionally a backtrace — into `trace-log`:

```janet
(trace-clear)
(def h (trace "exec_*" :void [:pointer]))   # one signature for every match
# ... run queries ...
(length trace-log)                           # calls captured
(untrace h)
```

Because the one signature is applied to every match, only trace functions that
genuinely share it (all fmgr builtins are `:uint64 [:pointer]`, for instance).
It's the same `hook-fn` on-leave path from [hooking.md](hooking.md), fanned out
across a name pattern — the frida-trace analog.

## What needs what

- `resolve` of an **exported** symbol works on any build.
- `resolve` of a **static**, and all of `struct` / `sig`, need debug info (DWARF).
- `sym-from-addr` / `backtrace` degrade gracefully — an address with no symbol
  comes back with just `:addr`.
- `disasm` / `scan` need only a readable address; no debug info required.
