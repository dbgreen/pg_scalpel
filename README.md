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

## An interactive REPL

Everything above is one `janet_eval` at a time, typed into `psql` and wrapped in
`SELECT janet_eval($j$ … $j$)`. For actually poking around, that wears thin.
`scalpel_repl()` turns the backend you're connected to into a live Janet prompt.

Call it in one session — it takes over that backend and blocks, serving — and it
prints the per-pid socket it's listening on:

```sql
SELECT scalpel_repl();
--  INFO:  pg_scalpel REPL listening on /tmp/pg_scalpel-681740.sock (pid 681740)
```

Then connect a client to that socket and you're at a prompt inside that backend:

```console
$ socat - UNIX-CONNECT:/tmp/pg_scalpel-681740.sock      # or: nc -U <socket>
pg_scalpel REPL - Janet 1.41.3-dev in backend pid 681740
scalpel> (+ 40 2)
42
scalpel> (def p (scalpel/resolve "strlen"))
<pointer 0x742E7ED9A780>
scalpel> (scalpel/call p :uint64 [:cstring] "hello, scalpel")
(:ok <core/u64 14>)
scalpel> (get (scalpel/struct "Path") :size)
80
scalpel> (error "boom")
error: boom
scalpel> (scalpel/call p :uint64 [:cstring] "still here")
(:ok <core/u64 10>)
scalpel> :quit
```

Three things the one-shot form doesn't give you. State persists across lines —
`p` holds `strlen`'s address, so you can call through it again after the
`(error "boom")`, because it's one VM for the life of the connection.
Results come back `%q`-formatted, so no `string/format` wrapper. And a Janet error is just a line that didn't work: it prints and drops
you back at the prompt instead of dropping the connection.

It carries the same lock as `janet_eval` — superuser only — and the socket is
`0600` and `SO_PEERCRED`-gated, so only a client running as the backend's own OS
user can connect. The socket exists only while that backend is serving; `:quit`
or disconnect to end it. The waits go through `WaitLatchOrSocket`, so a backend
parked in the REPL still answers `pg_cancel_backend` and shutdown rather than
going deaf.

### Triggering SQL from the prompt

A backend parked in the REPL is serving you, not reading SQL from a client — but
it's still a live backend, and `scalpel/sql` runs SQL in it through SPI (the same
path PL/pgSQL uses). So the hook-and-trigger loop happens right at the prompt:
stand in front of `int4pl`, run the add, watch it come back changed, put it back.

```console
scalpel> (def d (scalpel/closure :uint64 [:pointer]
                  (fn [fc] (+ (scalpel/getarg fc 0 :int) (scalpel/getarg fc 1 :int) 100))))
<pointer 0x742E7F160FC8>
scalpel> (scalpel/hook (scalpel/resolve "int4pl") d)
<pointer 0x629F9C2EF058>
scalpel> (scalpel/sql "SELECT 2 + 3 AS sum")
(:ok @[@{:sum "105"}])
scalpel> (scalpel/unhook (scalpel/resolve "int4pl"))
nil
scalpel> (scalpel/sql "SELECT 2 + 3 AS sum")
(:ok @[@{:sum "5"}])
scalpel> :quit
```

`scalpel/sql` runs inside the caller's transaction and hands back rows as text —
`(:ok @[{:col val …} …])`, or `(:error …)` if the query throws, so a bad query is
one more line that didn't work, not a dropped session.

## What's here

The C primitives, by what you're doing with them:

- Finding things: `resolve`, `syms`, `sym-from-addr`, and the DWARF readers `sig`
  and `struct`.
- Calling C: `call` (guarded), `call-raw` (not), and `closure`, which turns a
  Janet function into a real C function pointer you can hand to anything that
  wants one.
- Running SQL: `sql`, through SPI — so a hook you just set fires when the query
  runs, right from the prompt.
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
for a query — type, costs, which ones survived, and for each join the outer and
inner paths it was built from — decoded out of backend memory, no EXPLAIN
parsing. It's also what real use looks like — a `.janet` program kept in a file
and pulled in with `scalpel-load`, rather than pasted into a query.

First give the planner something to choose between — a table with rows, an index,
and stats (autovacuum off so the visibility map stays where you left it; more on
that below):

```sql
CREATE TABLE t(a int, b int) WITH (autovacuum_enabled = off);
INSERT INTO t SELECT g % 100, g FROM generate_series(1, 5000) g;
CREATE INDEX ON t(a);
ANALYZE t;
```

Then, in a REPL on that backend, load and arm the observatory, fire the query
with `scalpel/sql` (so `add_path` sees every path the planner builds), and dump —
the report renders right at the prompt:

```console
scalpel> (os/setenv "PGJ_NODETAGS"
           "/your/pg/install/include/postgresql/server/nodes/nodetags.h")
nil
scalpel> (def obs (scalpel-load "janet-lib/planner-observatory"))
<function planner-observatory>
scalpel> (obs :arm)
armed: 3 hooks; nodetags=487
scalpel> (scalpel/sql "EXPLAIN SELECT count(*) FROM t t1 JOIN t t2 USING (a) WHERE t1.b < 200")
(:ok @[@{:QUERY PLAN "Aggregate  (cost=358.31..358.32 rows=1 width=8)"} … ])
scalpel> (obs :dump)
planner candidates: 17 paths over 4 rel(s)
  legend:  * winner (cheapest_total_path)   + survived (kept in pathlist)   x rejected (dominated/pruned)   param = per-execution cost

  t{1}  (4 considered, 3 kept, 1 rejected)
    + IndexScan        rows=2         cost=     0.28..1.31       param
    * SeqScan          rows=199       cost=     0.00..85.50
    + IndexScan        rows=199       cost=     0.28..203.73
    x BitmapHeapScan   rows=2         cost=     0.66..5.41       param

  t{2}  (4 considered, 3 kept, 1 rejected)
    + IndexOnlyScan    rows=50        cost=     0.28..1.76       param
    * SeqScan          rows=5000      cost=     0.00..73.00
    + IndexOnlyScan    rows=5000      cost=     0.28..191.23
    x BitmapHeapScan   rows=50        cost=     0.81..5.44       param

  {1,2}  (7 considered, 1 kept, 6 rejected)
    * HashJoin         rows=9950      cost=   135.50..333.44
          outer SeqScan t{1}  rows=199 cost=0.00..85.50
          inner SeqScan t{2}  rows=5000 cost=0.00..73.00
    x NestLoop         rows=9950      cost=     0.29..366.39
          outer SeqScan t{1}  rows=199 cost=0.00..85.50
          inner Memoize t{2}  rows=50 cost=0.29..1.77  param
              <- IndexOnlyScan t_a_idx on t{2}  rows=50 cost=0.28..1.76  param
    x NestLoop         rows=9950      cost=     0.29..392.42
          outer SeqScan t{2}  rows=5000 cost=0.00..73.00
          inner Memoize t{1}  rows=2 cost=0.29..1.32  param
              <- IndexScan t_a_idx on t{1}  rows=2 cost=0.28..1.31  param
    x MergeJoin        rows=9950      cost=    93.38..446.08
          outer IndexOnlyScan t_a_idx on t{2}  rows=5000 cost=0.28..191.23
          inner SeqScan t{1}  rows=199 cost=0.00..85.50  +sort
    x NestLoop         rows=9950      cost=     0.28..535.34
          outer SeqScan t{1}  rows=199 cost=0.00..85.50
          inner IndexOnlyScan t_a_idx on t{2}  rows=50 cost=0.28..1.76  param
    x HashJoin         rows=9950      cost=    87.99..585.49
          outer SeqScan t{2}  rows=5000 cost=0.00..73.00
          inner SeqScan t{1}  rows=199 cost=0.00..85.50
    x MergeJoin        rows=9950      cost=   473.29..623.54
          outer SeqScan t{1}  rows=199 cost=0.00..85.50  +sort
          inner SeqScan t{2}  rows=5000 cost=0.00..73.00  +sort

  {}  (2 considered, 2 kept, 0 rejected)
    * Agg              rows=1         cost=   358.31..358.32
    + Agg              rows=1         cost=   358.31..358.32
scalpel> (obs :disarm)
disarmed
```

Readable node names need `nodetags.h`: set `PGJ_NODETAGS` when you start the
server (`PGJ_NODETAGS=$(pg_config --includedir-server)/nodes/nodetags.h`), or, as
above, from the prompt with `os/setenv` before loading — without it the types
show as `Tag#N` and joins don't list their inputs. Base rels are labeled by name
and range-table index, so the two sides of a self-join show as `t{1}` and `t{2}`.
Exact paths and costs move with the data and stats.

EXPLAIN shows you the winner. This shows what it beat and what each loser was
made of: the hash join scans and hashes all of `t2` once; the runner-up probes
`t2`'s index at 1.76 a probe, with a Memoize in front that expects most of the
199 outer rows to repeat a value of `a` it already fetched (the plain nested
loop, with no Memoize, is 535.34). A probe costs that much because the table has
never been vacuumed — `relallvisible` is 0, so each index-only probe is charged
for the heap pages it would visit. Run `VACUUM t`, `(obs :clear)` (the log
accumulates across queries until you clear or re-arm), and plan it again: the
probe drops to 1.30, the nested loop drops to 325.91, and it beats the unchanged
333.44 hash join. Same data, same row estimates, different plan.

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
