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

If you've never written Janet, the whole interface is Janet, so start with the
[language docs](https://janet-lang.org/docs/index.html) and the
[API reference](https://janet-lang.org/api/index.html) — it's a small Lisp and an
afternoon is plenty.

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

Every function — signature, arguments, return, and the gotchas — is written up
in [docs/reference.md](docs/reference.md). Two companion pages explain the halves
of the tool: [docs/observation.md](docs/observation.md) (reading a live backend)
and [docs/hooking.md](docs/hooking.md) (how `hook`/`hook-fn` and the trampoline
work).

`janet-lib/planner-observatory.janet` is a worked example: load it and it hooks
`add_path`/`set_cheapest` to report every candidate path the planner considered
for a query — type, costs, and which ones survived — decoded out of backend
memory, no EXPLAIN parsing. It's also what real use looks like — a `.janet`
program kept in a file and pulled in with `scalpel-load`, rather than pasted into
a query.

```sql
SELECT janet_eval($j$ (def PO (scalpel-load "janet-lib/planner-observatory")) (PO :arm) $j$);
EXPLAIN (COSTS OFF) SELECT count(*) FROM t t1 JOIN t t2 USING (a) WHERE t1.b < 500;
SELECT janet_eval($j$ (PO :dump) $j$);     -- the grouped, cost-sorted candidate report
SELECT janet_eval($j$ (PO :disarm) $j$);
```

`(PO :dump)` prints the candidates — here on a 5000-row, indexed, analyzed `t`
(exact paths and costs move with the data and stats):

```
planner candidates: 24 paths over 4 rel(s)
  legend:  * winner (cheapest_total_path)   + survived (kept in pathlist)   x rejected (dominated/pruned)   param = per-execution cost

  t  (8 considered, 3 kept, 5 rejected)
    + IndexScan        rows=5         cost=     0.28..1.31       param
    * SeqScan          rows=499       cost=     0.00..85.50
    + IndexScan        rows=499       cost=     0.28..203.73
    x IndexOnlyScan    rows=50        cost=     0.28..1.21       param
    x BitmapHeapScan   rows=50        cost=     0.73..5.35       param
    x BitmapHeapScan   rows=5         cost=     0.66..5.41       param
    x SeqScan          rows=5000      cost=     0.00..73.00
    x IndexOnlyScan    rows=5000      cost=     0.28..99.28

  {1,2}  (12 considered, 1 kept, 11 rejected)
    * HashJoin         rows=24950     cost=   135.50..502.94
    x NestLoop         rows=24950     cost=     0.29..524.61
    x NestLoop         rows=24950     cost=     0.29..579.92
    x MergeJoin        rows=24950     cost=   108.14..593.89
    x NestLoop         rows=24950     cost=     0.57..606.20
    x NestLoop         rows=24950     cost=     0.57..642.83
    x MergeJoin        rows=24950     cost=     0.56..678.51
    x MergeJoin        rows=24950     cost=     0.56..689.76
    x HashJoin         rows=24950     cost=    91.74..739.24
    x MergeJoin        rows=24950     cost=   488.06..864.80
    x MergeJoin        rows=24950     cost=   488.06..887.31
    x NestLoop         rows=24950     cost=     0.28..940.59

  {1}  (1 considered, 0 kept, 1 rejected)
    x Result           rows=1         cost=     0.00..0.01

  {}  (3 considered, 2 kept, 1 rejected)
    * Agg              rows=1         cost=   565.31..565.32
    + Agg              rows=1         cost=   565.31..565.32
    x Result           rows=1         cost=     0.00..0.01
```

EXPLAIN shows you the winner; this shows the eleven join paths it rejected, with
their costs — read out of the `RelOptInfo` and `Path` structs mid-plan.

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

## License

pg_scalpel is under the PostgreSQL License — see [LICENSE](LICENSE).

It links a handful of other people's work, all under licenses that allow it:
funchook (GPLv2+ with a linking exception), capstone (BSD), Janet (MIT), libffi
(MIT-style), and elfutils (LGPL, linked dynamically). Their license texts ride
along with the vendored submodules; carry them with any binary you ship.
