\echo Use "CREATE EXTENSION pg_scalpel" to load this file. \quit
CREATE FUNCTION janet_eval(text) RETURNS text
AS 'MODULE_PATHNAME', 'pgs_eval'
LANGUAGE C STRICT;
