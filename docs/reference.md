# Function reference

Everything here runs through `janet_eval(text)` or the REPL. Both are
superuser-only. The VM belongs to the connection and keeps its state between
statements.

Calls that can hit a Postgres error return a tagged tuple, either `(:ok value)`
or `(:error @{:message "..." :sqlstate "..."})`.

## Type keywords

`call`, `closure`, and `getarg` take these type keywords:

`:void :int :uint :int64 :uint64 :char :uchar :short :ushort :bool :pointer :datum
:cstring :bytes :double :float`

- `:char` and `:short` (and their unsigned versions) are C's 8- and 16-bit
  integers. The names follow C, not Postgres, where `int8` means 8 bytes.
- Aliases: `:int32` is `:int`, `:uint32` is `:uint`, `:ptr` is `:pointer`,
  `:string` is `:cstring`, and `:buf` is `:bytes`.
- `:int64` and `:uint64` are exact 64-bit integers.
- `:cstring` converts between a Janet string and a `char *`.
- `:bytes` passes the data pointer of a string (input only) or a buffer (input
  or output).
- As arguments, `:cstring` and `:bytes` also accept a pointer, or nil for NULL.
  A closure receives a `char *` as a pointer, so a hook can pass it straight
  through.
- `:datum` is a Postgres `Datum`. `getarg` and a closure's return value treat it
  as a 64-bit integer. A closure argument or a `call` return gives you a
  `<pointer>`. You can convert either way.
- `:void` is only valid as a return type.

## Symbols and introspection

[observation.md](observation.md) covers this part in more detail.

**`(scalpel/resolve name)`** returns `<pointer>` or nil.
The address of a symbol. Exported names resolve on any build. Statics come from
the ELF symbol table, so they need an unstripped binary or its debug file. If a
static name exists in more than one module, you get the one in postgres itself.

**`(scalpel/syms pattern &opt limit)`** returns `@[{:name :addr} ...]`.
Function symbols matching a glob (`*` and `?`), up to `limit` (default 1000).

**`(scalpel/sym-from-addr addr)`** returns `{:name :addr :displacement}` or nil.
The nearest function symbol at or before a code address, and the offset into it.

**`(scalpel/sig name)`** returns `{:name :addr :ret :args :call}` or nil.
A function's signature from DWARF. `:call` is `(ret (args...))`, in the form
`call` takes. Some functions can't be called correctly through `call`: variadic
functions, and functions that pass or return a struct, union, `long double`, or
128-bit integer by value. Those have no `:call`. Instead `:unsupported` says why,
and `:ret` and `:args` name the types that caused it.

**`(scalpel/struct name)`** returns
`{:name :size :fields [{:name :offset :size :call} ...]}` or nil.
A struct's layout from DWARF. `name` can be the struct tag or a typedef for one.
A field that is itself a struct, union, or array has `:call` set to `:struct`,
`:union`, or `:array`. A bitfield has `:call :bitfield`, `:offset` is the byte
that holds its first bit, and it also has `:bit-offset` (0 to 7 within that
byte) and `:bit-size`. `pack-struct` won't write bitfields and `read-struct`
skips them.

**`(scalpel/image-range &opt module)`** returns `{:base :size :text :textsize}`
or nil.
Where a loaded module sits in memory (the main executable by default) and where
its `.text` is.

**`(scalpel/disasm addr &opt len)`** returns `@[{:addr :size :bytes :text} ...]`.
Disassembles up to `len` bytes (default 16) of the x86-64 code at `addr`.

**`(scalpel/scan start size pattern &opt mask)`** returns `@[<pointer> ...]`.
Finds `pattern` between `start` and `start + size`. `mask` is the same length as
`pattern`, and a `0` in it matches any byte. The range has to be readable or the
backend crashes.

**`(scalpel/backtrace &opt maxframes)`** returns `@[{:addr :name :displacement} ...]`.
The C stack with symbol names, 32 frames by default.

## Calling C

**`(scalpel/call addr ret-type [arg-types] args...)`** returns `(:ok v)` or
`(:error {...})`.
Calls the C function at `addr` through libffi, inside a subtransaction. If the
function raises an `ereport`, you get `(:error …)` back. A cancel or
`statement_timeout` isn't caught and ends the statement like it normally would.
The signature has to match the real one. A wrong signature crashes the backend
with a SIGSEGV, and the subtransaction can't catch that. Use `sig` to get the
real signature.

**`(scalpel/call-raw addr ret-type [arg-types] args...)`** returns `(:ok v)`.
`call` without the subtransaction. It's cheaper, but an `ereport` from the
function isn't caught. The error ends the statement, or in the REPL, the line.

**`(scalpel/closure ret-type [arg-types] fn)`** returns `<pointer>`.
A C function pointer that runs `fn` when called. It's never freed, so it lasts
as long as the backend. Use it as a hook detour or anywhere C wants a callback.

**`(scalpel/sql query &opt limit)`** returns `(:ok rows)`, `(:ok n)`, or
`(:error {...})`.
Runs SQL in this backend through SPI, the same way the PLs do, inside a
subtransaction like `call`. A query that returns rows gives
`@[{:col "val" ...} ...]` with every value as text. A NULL column is left out of
its row. If a column name repeats, the later ones get their position appended
(`:a`, `:a_2`). `limit` caps the rows returned, and 0 means all of them. Other
commands return the number of rows processed. Since the query runs in this
process, any hook you've installed fires during it. It runs inside the caller's
transaction, so you can't commit or roll back the outer transaction from here.

## Replacing fmgr functions

A closure that replaces a builtin SQL function gets one `:pointer` argument, the
`FunctionCallInfo`.

**`(scalpel/getarg fcinfo n type)`** returns a value or nil.
Reads argument `n` as `type`. Returns nil for SQL NULL. It doesn't detoast
varlenas.

**`(scalpel/setresult-isnull fcinfo bool)`** returns nil.
Makes the replacement function return SQL NULL.

**`(scalpel/recv type-name bytes)`** returns `(:ok text)` or `(:error {...})`.
Decodes `bytes` with the type's binary input function, in a subtransaction, and
returns the value as text.

## Changing behavior

[hooking.md](hooking.md) explains how `hook`, `hook-fn`, and the trampoline work.

**`(scalpel/hook target-addr detour-ptr)`** returns `<trampoline>`.
Patches `target-addr` to jump to `detour-ptr`. The trampoline it returns runs
the original function. Call it with the target's signature. It panics if the
target is already hooked or too short to patch.

**`(scalpel/unhook target-addr)`** returns nil.
Removes a hook and puts the original bytes back.

**`(scalpel/intercept-rows f)`** returns nil.
Sets a filter on output rows for the rest of the session. Pass nil to clear it.
`f` gets the row's columns as text (nil for NULL) and returns an array of the
same length. A string replaces the value and is parsed with the column's input
function. nil makes it NULL. Anything else leaves it as it was.

**`(scalpel/page-protect addr len writable?)`** returns nil.
Makes a range read/write/exec (`true`) or read/exec (`false`). The range is
rounded out to pages. It errors if `mprotect` fails.

**`(scalpel/flush-icache addr len)`** returns nil.
Flushes the instruction cache after you write code. It does nothing on x86-64
but is needed on arm64.

## Errors

**`(scalpel/raise msg)`** returns `(:error {...})`.
Raises a test error inside the subtransaction, so it always returns `(:error …)`.

## The REPL

**`scalpel_repl()`** returns `integer`.
This one is a SQL function, not something you call from Janet, and it's
superuser-only. It takes over the backend that calls it and serves a Janet REPL
to one client on the Unix socket `/tmp/pg_scalpel-<pid>.sock`. The socket is
`0600`, and `SO_PEERCRED` limits it to clients running as the same OS user.

Lines run in the connection's VM, so hooks, patches, and a row filter stay in
place across lines. Results are printed `%q`-formatted. A form can span several
lines. A Janet error prints and you get the prompt back. Each line also runs in
its own subtransaction, so a Postgres error prints too and only rolls back that
line. A cancel ends the REPL, including a cancel of a form that's stuck in a
loop.

`:quit` or disconnecting ends it. It waits with `WaitLatchOrSocket`, so the
backend still responds to `pg_cancel_backend` and shutdown while it waits.
Connect with `socat - UNIX-CONNECT:<socket>` or `nc -U <socket>`. You can only
reach a backend that called `scalpel_repl()` itself.

# The prelude

These are defined in `janet-lib/` and loaded automatically.

## Structs

**`(make-struct name values)`** returns `@buffer`.
Packs `values`, a table keyed by field name, into a buffer the size of
`(scalpel/struct name)`.

**`(pack-struct layout values)`** returns `@buffer`.
Same as `make-struct`, but takes a `layout` from `scalpel/struct`.

**`(read-struct layout buf)`** returns `@{...}`.
Decodes a buffer (from `peek-bytes`, for example) into a table of field names
and values. It reads integer, pointer, bool, float, and double fields. Bitfields,
int128, long double, and aggregate (struct/union/array) fields have no single
number value and are skipped.

**`(scalpel-load relpath)`** returns a value.
Loads a `.janet` file relative to `scalpel-root` and returns its last value.
`scalpel-root` defaults to `$HOME/pg_scalpel`. If your checkout is somewhere
else, `(set scalpel-root "/path/to/pg_scalpel")`.

## Hooking and patching

`target` can be a name, an address, or a pointer.

**`(hook target detour)`** returns `<trampoline>`, and **`(unhook target)`**
removes it. These are `scalpel/hook` and `scalpel/unhook` but they take a name.

**`(hook-fn target ret-type arg-types &named on-enter on-leave)`** returns a
handle.
Wraps `target` and still calls the original. `on-enter (fn [args])` sees the
arguments. `on-leave (fn [args ret])` returns the result, which can be different
from the original's. Remove it with `(unhook target)`.

**`(replace-fn target ret-type arg-types fn)`** returns a handle.
Replaces `target` with `fn`. The original never runs. Remove it with
`(unhook target)`.

**`(trace pattern ret-type arg-types &named limit stack)`** returns a handle.
Hooks every function matching `pattern`, all with the same signature, and logs
`@{:fn :args :ret :stack}` to `trace-log` on each call. `:stack true` adds a
backtrace. Remove it with `(untrace handle)`.

**`(untrace handle)`** returns a count. Removes the trace's hooks.

**`(trace-clear)`** empties `trace-log`. **`trace-log`** is the array the log
goes into.

**`(patch-jump target dest)`** returns a handle.
Sends `target` to `dest` with a plain jump, with no trampoline. Undo it with
`(unpatch h)`.

**`(patch-return-int target n)`** and **`(patch-return-zero target)`** return a
handle.
Overwrite the start of `target` so it ignores its arguments and returns `n`, or
0. Undo it with `(unpatch h)`.

**`(install target stub)`** returns a handle. Overwrites the start of `target`
with the bytes in `stub` and saves what was there. **`(unpatch handle)`** puts
them back.

**`(peek-bytes addr n)`** returns `@buffer`. Reads `n` bytes.

**`(poke-bytes addr bytes)`** returns addr. Writes bytes. It makes the page
writable, copies, flushes the icache, and restores the protection.

**`(read-f64 addr)`** / **`(read-f32 addr)`** return a number. Read an IEEE-754
double or single from memory at `addr`. **`(dec-f64 buf off)`** / **`(dec-f32 buf
off)`** are the byte-buffer decoders behind them: this Janet writes floats
natively but can't read them back, so these reconstruct the value (a Janet number
is itself an f64, so it round-trips exactly).

**`(ptr->int p)`** returns a number, the address in a pointer.
