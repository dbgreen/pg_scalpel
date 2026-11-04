#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "access/xact.h"
#include "utils/resowner.h"
#include "dlfcn.h"
#include "janet.h"

#include "pg_scalpel.h"
#include "pgs_timer.h"

PG_MODULE_MAGIC;

JanetTable *pgs_env = NULL;
static bool started = false;

static Janet
jpgs_resolve(int32_t argc, Janet *argv)
{
	janet_fixarity(argc, 1);
	return pgs_sym_resolve((const char *) janet_getcstring(argv, 0));
}

static Janet
jpgs_backend_pid(int32_t argc, Janet *argv)
{
	janet_fixarity(argc, 0);
	return janet_wrap_integer(MyProcPid);
}
static Datum
thunk_parse_int(void *arg)
{
	return DirectFunctionCall1(int4in, CStringGetDatum((char *) arg));
}

static Janet
jpgs_parse_int(int32_t argc, Janet *argv)
{
	const char *s = (const char *) janet_getcstring(argv, 0);
	Datum		result;
	PgsError	err;

	if (pgs_guarded_call(thunk_parse_int, (void *) s, &result, &err))
		return pgs_ok(janet_wrap_integer(DatumGetInt32(result)));
	return pgs_err_value(&err);
}
static const JanetReg pgs_cfuns[] = {
	{"backend-pid", jpgs_backend_pid,
	"(scalpel/backend-pid)\n\nThe process id of the backend running this code."},
	{"parse-int", jpgs_parse_int,
	"(scalpel/parse-int str)\n\nParse str as an int4, the way '...'::int does."},
	{"resolve", jpgs_resolve,
	"(scalpel/resolve name)\n\nAddress of symbol name, or nil."},
	{NULL, NULL, NULL}
};

Janet
pgs_ok(Janet value)
{
	Janet	   *tup = janet_tuple_begin(2);

	tup[0] = janet_ckeywordv("ok");
	tup[1] = value;
	return janet_wrap_tuple(janet_tuple_end(tup));
}

Janet
pgs_err_value(PgsError *e)
{
	JanetTable *t = janet_table(2);
	Janet	   *tup;

	janet_table_put(t, janet_ckeywordv("sqlstate"), janet_cstringv(e->sqlstate));
	if (e->message)
		janet_table_put(t, janet_ckeywordv("message"), janet_cstringv(e->message));

	tup = janet_tuple_begin(2);
	tup[0] = janet_ckeywordv("error");
	tup[1] = janet_wrap_table(t);
	return janet_wrap_tuple(janet_tuple_end(tup));
}

bool
pgs_guarded_call(PgsThunk thunk, void *arg, Datum *result, PgsError *err)
{
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;
	volatile bool ok = false;

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(oldcontext);

	PG_TRY();
	{
		Datum		d = thunk(arg);

		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
		*result = d;
		ok = true;
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(oldcontext);
		edata = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;

		err->sqlstate = pstrdup(unpack_sql_state(edata->sqlerrcode));
		err->message = edata->message ? pstrdup(edata->message) : NULL;
		FreeErrorData(edata);
		ok = false;
	}
	PG_END_TRY();

	return ok;
}





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
	JanetVM    *vm = janet_local_vm();
	int			gclock;
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
	size_t		len = strlen(src);
	bool		ok = true;

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
	char	   *src = text_to_cstring(PG_GETARG_TEXT_PP(0));
	Janet		result;
	const uint8_t *s;

	pgs_vm_ensure();
	if (pgs_run_interruptible(src, "pg_scalpel", &result))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("janet error: %s", (const char *) janet_to_string(result))));

	s = janet_to_string(result);
	return PointerGetDatum(cstring_to_text_with_len((const char *) s, janet_string_length(s)));
}
