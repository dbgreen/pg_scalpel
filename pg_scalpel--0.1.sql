\echo Use "CREATE EXTENSION pg_scalpel" to load this file. \quit
CREATE FUNCTION janet_eval(text) RETURNS text
AS 'MODULE_PATHNAME', 'pgs_eval'
LANGUAGE C STRICT;

-- Belt to the C-level superuser() guard's suspenders: superusers bypass ACLs,
-- so revoking the default PUBLIC EXECUTE locks janet_eval to superusers only.
REVOKE ALL ON FUNCTION janet_eval(text) FROM PUBLIC;
