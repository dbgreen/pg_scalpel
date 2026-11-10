# Reading a running backend

[hooking.md](hooking.md) covers changing what the backend does. This page covers
looking at it: symbols, struct layouts, machine code, memory, and stacks, read
from inside the running process.

## Reading the binary

Three functions tell you where something is, what it looks like, and how to call
it. They read the binary the backend is running, not the source.

- `scalpel/resolve name` turns a name into an address. Exported symbols go
  through `dlsym`. Static functions come from the ELF symbol table, so they need
  a binary that isn't stripped, or its `-dbgsym` / `postgres.debug` file. This one
  doesn't need DWARF.
- `scalpel/struct name` reads a struct's layout out of DWARF: each field's
  offset, size, and a type keyword.
- `scalpel/sig name` reads a function's signature out of DWARF and gives you a
  `:call` entry you can pass straight to `scalpel/call`.

This is the layout the running binary was actually compiled with. If a struct
picked up a field in a minor release, or a member is behind an `#ifdef`, or the
padding is different, `scalpel/struct` shows you what's really there. You don't
need a source tree on the machine, and you don't have to copy a signature out of
a header by hand. With `scalpel/call` that matters, because a wrong signature
crashes the backend.

It can't see anything the compiler removed. At `-O2` a small static function is
often inlined into its callers and doesn't have an address anymore. Its types are
still in DWARF, but the function isn't.

## Turning addresses into names

- `scalpel/sym-from-addr addr` gives you the nearest function symbol at or before
  the address, and the offset into it.
- `scalpel/backtrace` gives you the current C stack, with each frame run through
  `sym-from-addr`. Call it from a hook's `on-enter` to see who called the
  function you're watching.

## Reading machine code and memory

- `scalpel/disasm addr len` disassembles the bytes at `addr` (x86-64, using
  capstone). It reads memory, not the file on disk, so you see a patched
  function, a funchook trampoline, or JIT code as it actually is.
- `scalpel/image-range` gives you where the loaded image starts and where its
  `.text` section is. That's a safe range to give `scan`.
- `scalpel/scan start size pattern &opt mask` searches a range of memory for a
  byte pattern and returns the addresses where it matches. A `0` in `mask`
  matches any byte.

`disasm` and `scan` read memory directly. Give them a range you know is mapped
and readable, like an address from `resolve` or the `:text` range from
`image-range`. A bad range crashes the backend. Nothing catches it.

## Tracing calls

`trace` is built on the same hooking as `hook-fn`. It hooks every function
matching a glob, all with one signature, and logs each call (the function, its
arguments, its return value, and optionally a backtrace) to `trace-log`:

```janet
(trace-clear)
(def h (trace "exec_*" :void [:pointer]))   # one signature for every match
# ... run queries ...
(length trace-log)                           # calls captured
(untrace h)
```

Since every match gets the same signature, only trace functions that really do
share it. All the fmgr builtins are `:uint64 [:pointer]`, for example. If you've
used frida-trace, it's the same idea.

## What needs debug info

- `resolve` on an exported symbol works on any build.
- `resolve` on a static needs an unstripped binary or its debug file.
- `struct` and `sig` need DWARF.
- `sym-from-addr` and `backtrace` still work without symbols. You just get
  `:addr` and no name.
- `disasm` and `scan` only need a readable address.
