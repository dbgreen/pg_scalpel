#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "janet.h"

#include "pg_scalpel.h"
#include "pgs_timer.h"

PG_MODULE_MAGIC;

JanetTable *pgs_env = NULL;
static bool started = false;

static Janet
jpgs_backend_pid(int32_t argc, Janet *argv)
{
    janet_fixarity(argc, 0);
    return janet_wrap_integer(MyProcPid);
}

static const JanetReg pgs_cfuns[] = {
    {"backend-pid", jpgs_backend_pid,
     "(scalpel/backend-pid)\n\nThe process id of the backend running this code."},
    {NULL, NULL, NULL}
};






JanetTable *
pgs_vm_ensure(void)
{
    if (started)
        return pgs_env;
    janet_init();
    pgs_env = janet_core_env(NULL);
    janet_cfuns_prefix(pgs_env, "scalpel", pgs_cfuns);
    started = true;
    return pgs_env;
}

static void
pgs_interrupt_cb(void *vm)
{
    janet_interpreter_interrupt((JanetVM *) vm);
}

static JanetSignal
pgs_run_form(Janet form, const char *where, Janet *out)
{
    JanetVM *vm = janet_local_vm();
    int gclock;
    JanetCompileResult cr;
    JanetFiber *fiber;
    JanetSignal sig;

    gclock = janet_gclock();
    cr = janet_compile(form, pgs_env, janet_cstring(where));
    if (cr.status == JANET_COMPILE_ERROR)
    {
        janet_gcunlock(gclock);
	*out = janet_wrap_string(cr.error);
	return JANET_SIGNAL_ERROR;
    }
    fiber = janet_fiber(janet_thunk(cr.funcdef), 64, 0, NULL);
    fiber->env = pgs_env;
    janet_gcunlock(gclock);

    for (;;)
    {
        sig = janet_continue(fiber, janet_wrap_nil(), out);
	if (sig != JANET_SIGNAL_INTERRUPT)
	    return sig;
	CHECK_FOR_INTERRUPTS();
	janet_interpreter_interrupt_handled(vm);
    }
}
int
pgs_run_interruptible(const char *src, const char *where, Janet *out)
{
    PgsTimer   *timer;
    JanetParser parser;
    size_t      len = strlen(src);
    bool        ok = true;

    *out = janet_wrap_nil();
    janet_parser_init(&parser);
    timer = pgs_timer_start(pgs_interrupt_cb, janet_local_vm(), 100);

    PG_TRY();
    {
        for (size_t i = 0; i <= len && ok; i++)
        {
            if (i < len)
                janet_parser_consume(&parser, (uint8_t) src[i]);
            else
                janet_parser_eof(&parser);

            while (ok && janet_parser_has_more(&parser))
                ok = pgs_run_form(janet_parser_produce(&parser), where, out) == JANET_SIGNAL_OK;

            if (ok && janet_parser_status(&parser) == JANET_PARSE_ERROR)
            {
                *out = janet_cstringv(janet_parser_error(&parser));
                ok = false;
            }
        }
    }
    PG_CATCH();
    {
        pgs_timer_stop(timer);
        janet_parser_deinit(&parser);
        PG_RE_THROW();
    }
    PG_END_TRY();

    pgs_timer_stop(timer);
    janet_parser_deinit(&parser);
    return ok ? 0 : 1;
}



PG_FUNCTION_INFO_V1(pgs_eval);

Datum
pgs_eval(PG_FUNCTION_ARGS)
{
    char       *src = text_to_cstring(PG_GETARG_TEXT_PP(0));
    Janet       result;
    const uint8_t *s;

    pgs_vm_ensure();
    if (pgs_run_interruptible(src, "pg_scalpel", &result))
        ereport(ERROR,
                (errcode(ERRCODE_DATA_EXCEPTION),
                 errmsg("janet error: %s", (const char *) janet_to_string(result))));

    s = janet_to_string(result);
    return PointerGetDatum(cstring_to_text_with_len((const char *) s, janet_string_length(s)));
}

