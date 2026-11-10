# pg_scalpel

pg_scalpel is a Postgres 19 extension that runs Janet inside the backend. You get
two SQL functions: `janet_eval`, which runs a string of Janet and returns the
result, and `scalpel_repl`, which gives you a Janet prompt in the backend.

From Janet you can look up any function or struct in the running binary by name,
call the server's own C functions, replace a function with your own code, rewrite
rows on their way to the client, and disassemble or patch machine code in the
process. You don't recompile anything and you don't restart the server.

That's as dangerous as it sounds. It's superuser-only, and you can't change that.
See [the sharp end](#the-sharp-end) for why.

It runs on Linux, x86-64, Postgres 19. Symbols and structs come from elfutils and
DWARF, disassembly from capstone, and hooking from funchook.

## Building it

You need a C compiler, make, and cmake (funchook builds with cmake). You need the
Postgres 19 server headers, either the `-dev`/`-devel` package or a from-source
install. You also need libffi and elfutils:

```bash
sudo apt-get install build-essential cmake libffi-dev libdw-dev libelf-dev
```

Janet and funchook are submodules, so clone with them:

```bash
git clone --recurse-submodules https://github.com/dbgreen/pg_scalpel.git
cd pg_scalpel
```

Build the vendored libraries once. This step needs a network connection, because
cmake downloads capstone for funchook:

```bash
make vendor
```

Then build and install. The Makefile uses the first `pg_config` on your `PATH`.
If that's the wrong server, pass the right one:

```bash
make PG_CONFIG=$(which pg_config)
sudo make install
```

Then, as a superuser:

```sql
CREATE EXTENSION pg_scalpel;
```

You don't need `shared_preload_libraries` or a restart. The Janet VM starts the
first time you call `janet_eval`.

### Debug info

How much you can see depends on debug info. Exported functions resolve on any
build. Static functions need a binary that isn't stripped, or its `-dbgsym` /
`postgres.debug` file. Struct layouts and function signatures come from DWARF, so
those need a debug build, a distro `-dbgsym` package, or a `postgres.debug` next
to the server binary. Without it, those calls return nil instead of failing.

## Using it

`janet_eval(text)` runs Janet and returns the last value as text. The C primitives
are named `scalpel/...`. A small Janet prelude (`hook-fn`, `make-struct`, `trace`,
and so on) is loaded before you run anything.

Everything is Janet. If you haven't used it, read the
[language docs](https://janet-lang.org/docs/index.html) and keep the
[API reference](https://janet-lang.org/api/index.html) handy. It's a small Lisp.

The VM lives as long as your connection, so anything you define in one statement
is still there in the next one in the same psql session. The multi-statement
examples below depend on that.

```sql
SELECT janet_eval('(+ 40 2)');
--  42
```

Look things up. This is `int4pl`'s address, the signature of the planner's
`add_path`, and the size of a `Path` in this binary:

```sql
SELECT janet_eval($j$ (scalpel/resolve "int4pl") $j$);
--  <pointer 0x...>
SELECT janet_eval($j$ (string/format "%q" (get (scalpel/sig "add_path") :call)) $j$);
--  (:void (:pointer :pointer))
SELECT janet_eval($j$ (get (scalpel/struct "Path") :size) $j$);
--  80
```

Call into the server. This calls `strlen`:

```sql
SELECT janet_eval($j$ (string/format "%q"
  (scalpel/call (scalpel/resolve "strlen") :uint64 [:cstring] "hello, scalpel")) $j$);
--  (:ok <core/u64 14>)
```

`scalpel/call` runs the call in a subtransaction. If the C function throws an
`ereport`, you get `(:error {...})` back and the connection stays up.

Hook `add_path`, which the planner calls for every candidate path, and count the
calls while it plans a join:

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

Replace `int4pl`, the C function behind integer `+`, with a Janet function that
adds 100:

```sql
SELECT janet_eval($j$
  (def d (scalpel/closure :uint64 [:pointer]
    (fn [fc] (+ (scalpel/getarg fc 0 :int) (scalpel/getarg fc 1 :int) 100))))
  (scalpel/hook (scalpel/resolve "int4pl") d) "installed" $j$);

SELECT 2 + 3;    --  105

SELECT janet_eval($j$ (scalpel/unhook (scalpel/resolve "int4pl")) $j$);
SELECT 2 + 3;    --  5
```

Rewrite rows on their way to the client. This upper-cases every text column, then
removes the filter:

```sql
SELECT janet_eval($j$ (scalpel/intercept-rows
  (fn [cols] (map (fn [c] (if (string? c) (string/ascii-upper c) c)) cols))) $j$);

SELECT 'hello' AS greeting;    --  HELLO

SELECT janet_eval($j$ (scalpel/intercept-rows nil) $j$);
```

## The REPL

Writing Janet inside `SELECT janet_eval($j$ … $j$)` gets old fast.
`scalpel_repl()` gives you a Janet prompt in the backend instead.

Call it in the session you want to work in. It prints the socket it's listening
on, then blocks:

```sql
SELECT scalpel_repl();
--  INFO:  pg_scalpel REPL listening on /tmp/pg_scalpel-681740.sock (pid 681740)
```

Connect to that socket with `socat` or `nc -U`:

```console
$ socat - UNIX-CONNECT:/tmp/pg_scalpel-681740.sock
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

It's the same VM for the whole connection, so `p` is still defined after the
error. Values come back `%q`-formatted. A form can span lines: the prompt changes
to `...>` until the outermost paren closes.

An error prints and you get the prompt back. Each line runs in its own
subtransaction, so a Postgres error (from a `call-raw` into something that throws,
for example) rolls back only that line. A cancel ends the REPL.

`scalpel/sql` runs SQL in the backend through SPI, the same way PL/pgSQL does. A
hook you set at the prompt fires when that SQL runs:

```console
scalpel> (def d (scalpel/closure :uint64 [:pointer]
    ...>          (fn [fc] (+ (scalpel/getarg fc 0 :int) (scalpel/getarg fc 1 :int) 100))))
<pointer 0x742E7F160FC8>
scalpel> (scalpel/hook (scalpel/resolve "int4pl") d)
<pointer 0x629F9C2EF058>
scalpel> (scalpel/sql "SELECT 2 + 3 AS sum")
(:ok @[@{:sum "105"}])
scalpel> (scalpel/unhook (scalpel/resolve "int4pl"))
nil
scalpel> (scalpel/sql "SELECT 2 + 3 AS sum")
(:ok @[@{:sum "5"}])
```

`scalpel/sql` runs in the caller's transaction. It returns rows as tables of text,
or `(:error …)` if the query fails.

The REPL is superuser-only, like `janet_eval`. The socket is `0600` and checked
with `SO_PEERCRED`, so only a client running as the backend's OS user can
connect. It goes away on `:quit` or disconnect. The backend waits on the socket
with `WaitLatchOrSocket`, so it still responds to `pg_cancel_backend`,
`statement_timeout`, and shutdown while it's sitting at the prompt.

## What's here

The C primitives:

- Finding things: `resolve`, `syms`, `sym-from-addr`, and the DWARF readers `sig`
  and `struct`.
- Calling C: `call` (guarded), `call-raw` (not guarded), and `closure`, which turns
  a Janet function into a C function pointer.
- Running SQL: `sql`, through SPI.
- Hooking: `hook` and `unhook`, built on funchook.
- Machine code: `disasm`, `scan`, `page-protect`, `flush-icache`, `image-range`.
- Replacing fmgr functions: `getarg`, `setresult-isnull`, `recv`.
- Query output: `intercept-rows`.
- Other: `backtrace`, `raise`.

The prelude in `janet-lib/` adds `hook-fn` and `unhook` by name, `trace` and
`untrace`, `replace-fn`, `make-struct`/`pack-struct`/`read-struct`, `patch-jump`,
and `patch-return-int`.

[docs/reference.md](docs/reference.md) covers every function: signature,
arguments, return value, and gotchas. [docs/observation.md](docs/observation.md)
covers reading a live backend, and [docs/hooking.md](docs/hooking.md) covers how
`hook`/`hook-fn` and the trampoline work.

### Example: the planner observatory

`janet-lib/planner-observatory.janet` hooks `add_path` and `set_cheapest` and
reports every candidate path the planner considered for a query: its type, its
costs, whether it survived, and for joins, which outer and inner paths it was
built from. It reads all of that out of backend memory. It's also an example of
keeping a Janet program in a file and loading it with `scalpel-load`.

Set up a table for the planner to work with. Autovacuum is off so the visibility
map stays the way you leave it:

```sql
CREATE TABLE t(a int, b int) WITH (autovacuum_enabled = off);
INSERT INTO t SELECT g % 100, g FROM generate_series(1, 5000) g;
CREATE INDEX ON t(a);
ANALYZE t;
```

Then, in a REPL on that backend:

```console
scalpel> (os/setenv "SCALPEL_NODETAGS"
    ...>   "/your/pg/install/include/postgresql/server/nodes/nodetags.h")
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

Node names need `nodetags.h`. Set `SCALPEL_NODETAGS` in the server's environment
(`SCALPEL_NODETAGS=$(pg_config --includedir-server)/nodes/nodetags.h`), or set it
from the prompt before loading, as above. Without it, types show as `Tag#N` and
joins don't list their inputs. Base rels are labeled by name and range-table
index, so the two sides of a self-join are `t{1}` and `t{2}`. Your paths and costs
will differ with your data and stats.

EXPLAIN only shows the hash join. Here you can also see what it beat. The
runner-up is a nested loop that probes `t2`'s index through a Memoize. Those
probes cost 1.76 each because the table has never been vacuumed. Run `VACUUM t`,
then `(obs :clear)`, and plan it again: the nested loop drops to 325.91 and beats
the 333.44 hash join.

## The sharp end

`janet_eval` runs native code as the OS user Postgres runs as. Janet already has
`os/execute`, `os/shell`, and file I/O, and pg_scalpel adds reading and writing
arbitrary memory and patching code. A role that can call it effectively owns the
host.

So it's restricted to superusers in three places:

- `pg_scalpel.control` sets `superuser = true` and `trusted = false`, so a
  non-superuser can't install it.
- The install script runs `REVOKE ALL ON FUNCTION janet_eval(text) FROM PUBLIC`.
- `janet_eval` checks `superuser()` itself and errors if you aren't one, so a
  later `GRANT` doesn't open it up.

None of that makes it safe. It just keeps it to superusers. Don't install it on a
server where people you don't trust can connect.

## License

pg_scalpel is under the PostgreSQL License. See [LICENSE](LICENSE).

It links funchook (GPLv2+ with a linking exception), capstone (BSD), Janet (MIT),
libffi (MIT-style), and elfutils (LGPL, linked dynamically). Their license texts
are in the vendored submodules. Include them with any binary you ship.
