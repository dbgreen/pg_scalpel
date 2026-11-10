#!/usr/bin/env bash
# Smoke test for pg_scalpel against a running cluster (as a superuser).
# Expects psql on PATH and a cluster at $PGHOST:$PGPORT. Exits non-zero on any
# failed assertion.
set -euo pipefail

HOST="${PGHOST:-127.0.0.1}"
PORT="${PGPORT:-5455}"
SUPER="${PGSUPER:-postgres}"

Q() { psql -h "$HOST" -p "$PORT" -U "$SUPER" -Atc "$1"; }
fail() { echo "FAIL: $1"; exit 1; }

psql -h "$HOST" -p "$PORT" -U "$SUPER" -v ON_ERROR_STOP=1 \
  -c "CREATE EXTENSION IF NOT EXISTS pg_scalpel;"

# janet evaluates
[ "$(Q "select janet_eval('(+ 40 2)')")" = "42" ] || fail "janet_eval arithmetic"

# call an exported libc function (no debug info needed)
[ "$(Q 'select janet_eval($j$ (string/format "%q" (scalpel/call (scalpel/resolve "strlen") :uint64 [:cstring] "hello, scalpel")) $j$)')" \
  = "(:ok <core/u64 14>)" ] || fail "scalpel/call strlen"

# DWARF struct layout (needs a debug build of the server)
[ "$(Q 'select janet_eval($j$ (get (scalpel/struct "Path") :size) $j$)')" = "80" ] \
  || fail "scalpel/struct Path size"

# closure + hook: replace int4pl with Janet, all in one session (multiple -c
# share a connection), then unhook and confirm it is restored.
out=$(psql -h "$HOST" -p "$PORT" -U "$SUPER" -At \
  -c 'select janet_eval($j$ (def d (scalpel/closure :uint64 [:pointer] (fn [fc] (+ (scalpel/getarg fc 0 :int) (scalpel/getarg fc 1 :int) 100)))) (scalpel/hook (scalpel/resolve "int4pl") d) "ok" $j$)' \
  -c 'SELECT 2 + 3' \
  -c 'select janet_eval($j$ (scalpel/unhook (scalpel/resolve "int4pl")) $j$)' \
  -c 'SELECT 2 + 3')
grep -qx 105 <<<"$out" || fail "closure-replaced int4pl (expected 105); got: $out"
grep -qx 5   <<<"$out" || fail "int4pl restored after unhook (expected 5); got: $out"

# superuser lockdown: a non-superuser is refused
psql -h "$HOST" -p "$PORT" -U "$SUPER" -q -c "DROP ROLE IF EXISTS pgs_ci_alice;" -c "CREATE ROLE pgs_ci_alice LOGIN;"
err=$(psql -h "$HOST" -p "$PORT" -U pgs_ci_alice -d postgres -Atc "select janet_eval('(+ 1 1)')" 2>&1 || true)
grep -q "permission denied for function janet_eval" <<<"$err" \
  || fail "non-superuser should be refused; got: $err"
psql -h "$HOST" -p "$PORT" -U "$SUPER" -q -c "DROP ROLE IF EXISTS pgs_ci_alice;"

echo "all smoke tests passed"
