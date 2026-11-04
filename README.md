# pg_scalpel

Load this into a Postgres 19 backend and you get one SQL function, `janet_eval`,
that runs Janet code inside the server process. From there you can look up any
function or struct in the running binary by name, call into the server's own C,
drop your own code in where one of its functions used to be, rewrite rows on
their way out to the client, and disassemble or patch the machine code of the
process you're sitting in. Nothing gets recompiled and the server never
restarts. It's a debugger you drive from SQL.

Which is exactly as dangerous as that sounds, so it's superuser-only and won't
let you make it otherwise. If that's the part you care about, skip to
[the sharp end](#the-sharp-end).

Linux, x86-64, Postgres 19. The symbol and struct machinery is elfutils and
DWARF, the disassembler is capstone, the hooking is funchook.

## Building it

You need a C compiler, make, and cmake — cmake is here because funchook builds
with it. You need Postgres 19's server headers (the `-dev`/`-devel` package, or a
from-source `install/`). And you need libffi and elfutils:

```bash
sudo apt-get install build-essential cmake libffi-dev libdw-dev libelf-dev
```

Janet and funchook ride along as submodules, so pull them when you clone:

```bash
git clone --recurse-submodules https://github.com/dbgreen/pg_scalpel.git
cd pg_scalpel
```

Build the vendored libraries once. This is the step that wants a network
connection — cmake pulls capstone down for funchook:

```bash
make vendor
```

Now build and install against your Postgres. The Makefile's `PG_CONFIG` points at
a path on my machine; aim it at yours:

```bash
make PG_CONFIG=$(which pg_config)
sudo make install
```

Then, connected as a superuser:

```sql
CREATE EXTENSION pg_scalpel;
```

No `shared_preload_libraries` line, no restart. The Janet VM comes up the first
time you call `janet_eval`.

One note about debug info, because it decides what half the tool can see.
Exported functions resolve on any build. Static functions, struct layouts, and
function signatures are read out of DWARF, so for those you need a debug build, a
distro `-dbgsym` package, or a `postgres.debug` file next to the server binary.
Without it those calls return nil — they don't fail, they just can't see.

## Using it

`janet_eval(text)` evaluates Janet and gives you back the last value as text. The
primitives are named `scalpel/...`, and a small Janet prelude (`hook-fn`,
`make-struct`, `trace`, the rest of it) is already loaded by the time you get a
prompt.

The VM stays alive for the life of your connection, so whatever you set up in one
statement is still there in the next — as long as it's the same psql session. The
multi-statement examples here count on that.

The obligatory warm-up:

```sql
SELECT janet_eval('(+ 40 2)');
--  42
```

Find something and look at it. Here's `int4pl`'s address, what the planner's
`add_path` actually takes, and how big a `Path` is in this exact binary:

```sql
SELECT janet_eval($j$ (scalpel/resolve "int4pl") $j$);
--  <pointer 0x...>
SELECT janet_eval($j$ (string/format "%q" (get (scalpel/sig "add_path") :call)) $j$);
--  (:void (:pointer :pointer))
SELECT janet_eval($j$ (get (scalpel/struct "Path") :size) $j$);
--  80
```

Call into the server. This reaches `strlen` and hands back the answer:

```sql
SELECT janet_eval($j$ (string/format "%q"
  (scalpel/call (scalpel/resolve "strlen") :uint64 [:cstring] "hello, scalpel")) $j$);
--  (:ok <core/u64 14>)
```

That `(:ok ...)` is the guard talking. The call runs inside a subtransaction, so
when the C you called throws an `ereport` you get `(:error {...})` back instead
of a dropped connection.

Watch the planner think. Hook `add_path`, which the planner calls once per
candidate path it considers, count the calls, then plan a join:

```sql
SELECT janet_eval($j$
  (def n @[0])
  (hook-fn "add_path" :void [:pointer :pointer]
           :on-enter (fn [args] (put n 0 (+ 1 (n 0)))))
  "watching add_path" $j$);

EXPLAIN (COSTS OFF) SELECT count(*) FROM t t1 JOIN t t2 USING (a) WHERE t1.b < 5;

SELECT janet_eval($j$ (n 0) $j$);          -- how many paths it built
SELECT janet_eval($j$ (unhook "add_path") $j$);
```

Now the one that makes the point. Stand in front of `int4pl` — the C behind
integer `+` — with a Janet function, and make it add 100:

```sql
SELECT janet_eval($j$
  (def d (scalpel/closure :uint64 [:pointer]
    (fn [fc] (+ (scalpel/getarg fc 0 :int) (scalpel/getarg fc 1 :int) 100))))
  (scalpel/hook (scalpel/resolve "int4pl") d) "installed" $j$);

SELECT 2 + 3;    --  105

SELECT janet_eval($j$ (scalpel/unhook (scalpel/resolve "int4pl")) $j$);
SELECT 2 + 3;    --  5, back to normal
```

Rewrite rows on their way to the client. This upper-cases every text column in
the output, then clears the filter:

```sql
SELECT janet_eval($j$ (scalpel/intercept-rows
  (fn [cols] (map (fn [c] (if (string? c) (string/ascii-upper c) c)) cols))) $j$);

SELECT 'hello' AS greeting;    --  HELLO

SELECT janet_eval($j$ (scalpel/intercept-rows nil) $j$);
```

## What's here

The C primitives, by what you're doing with them:

- Finding things: `resolve`, `syms`, `sym-from-addr`, and the DWARF readers `sig`
  and `struct`.
- Calling C: `call` (guarded), `call-raw` (not), and `closure`, which turns a
  Janet function into a real C function pointer you can hand to anything that
  wants one.
- Hooking: `hook` and `unhook`, on top of funchook.
- Machine code: `disasm`, `scan`, `page-protect`, `flush-icache`, `image-range`.
- Standing in for fmgr functions: `getarg`, `setresult-isnull`, `recv`.
- Query output: `intercept-rows`.
- Looking around: `backtrace`, `raise`.

On top of those is the prelude in `janet-lib/` — `hook-fn` and `unhook` by name,
`trace`/`untrace`, `replace-fn`, the `make-struct`/`pack-struct`/`read-struct`
trio, `patch-jump`, `patch-return-int`. `(scalpel/syms "*" 50)` dumps backend
symbols; the prelude you can just read in the source.

## The sharp end

`janet_eval` runs native code as whatever OS user Postgres runs as. Janet by
itself already has `os/execute`, `os/shell`, and file I/O; the scalpel primitives
add reading and writing arbitrary memory and patching code. Hand that to a role
you don't trust and you haven't given them a database feature, you've given them
the host.

So it's nailed to superuser in three places, because any one of them is a typo
away from nothing:

- `pg_scalpel.control` sets `superuser = true` and `trusted = false`, so it can't
  be installed for or handed off to a non-superuser.
- The install script runs `REVOKE ALL ON FUNCTION janet_eval(text) FROM PUBLIC`.
  Superusers ignore ACLs, so the revoke only ever bites the people it's meant to.
- `janet_eval` checks `superuser()` itself and throws if you're not, so a stray
  `GRANT` later still doesn't open the door.

That's locking the door, not making the room safe. The sharp instruments are the
whole reason the thing exists; there's nothing to sandbox. Don't put it on a box
where people you don't trust can connect.

## The files

```
pg_scalpel.c          the SQL entry point, the scalpel/* primitives, the guard, the VM
pgs_sym_linux.c       symbols, structs, signatures, backtraces (elfutils + DWARF)
pgs_hook_funchook.c   the hooking backend (funchook)
pgs_disasm.c          live disassembly (capstone)
pgs_timer.c           the interrupt timer that keeps a runaway script cancelable
janet-lib/            the Janet prelude source, baked into pgs_prelude.h at build
tools/                embed-prelude.janet, which does that baking
vendor/               Janet and funchook, as submodules
```

Janet is MIT, capstone is BSD, funchook carries its own license; see each
vendored repo.
