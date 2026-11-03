#include "postgres.h"
#include "fmgr.h"
#include "utils/builtins.h"
#include "janet.h"

PG_MODULE_MAGIC;

JanetTable *pgs_env = NULL;
static bool started = false;

JanetTable *
pgs_vm_ensure(void)
{
    if (started)
        return pgs_env;
    janet_init();
    pgs_env = janet_core_env(NULL);
    started = true;
    return pgs_env;
}

PG_FUNCTION_INFO_V1(pgs_eval);

Datum
pgs_eval(PG_FUNCTION_ARGS)
{
    char *src = text_to_cstring(PG_GETARG_TEXT_PP(0));
    Janet result;

    janet_dostring(pgs_vm_ensure(), src, "pg_scalpel", &result);

    const uint8_t *s = janet_to_string(result);
    // Janet result lives in Janet's gc'ed heap.  We copy it to get it into pg's arena.
    return PointerGetDatum(cstring_to_text_with_len((const char*) s, janet_string_length(s)));
}
