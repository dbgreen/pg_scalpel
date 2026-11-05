\echo Use "CREATE EXTENSION pg_scalpel" to load this file. \quit
CREATE FUNCTION janet_eval(text) RETURNS text
AS 'MODULE_PATHNAME', 'pgs_eval'
LANGUAGE C STRICT;

-- Belt to the C-level superuser() guard's suspenders: superusers bypass ACLs,
-- so revoking the default PUBLIC EXECUTE locks janet_eval to superusers only.
REVOKE ALL ON FUNCTION janet_eval(text) FROM PUBLIC;

-- scalpel_repl(): serve an interactive Janet REPL over a per-backend Unix socket
-- to one same-uid client (drive it with socat/nc). Superuser-only, same as
-- janet_eval: the C function checks superuser() and the default ACL is revoked.
CREATE FUNCTION scalpel_repl() RETURNS integer
AS 'MODULE_PATHNAME', 'scalpel_repl'
LANGUAGE C;

REVOKE ALL ON FUNCTION scalpel_repl() FROM PUBLIC;
