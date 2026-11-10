#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "libpq/pqformat.h"
#include "lib/stringinfo.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "catalog/pg_type.h"
#include "access/xact.h"
#include "utils/resowner.h"
#include "dlfcn.h"
#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#include "janet.h"
#include <ffi.h>

#include "pg_scalpel.h"
#include "pgs_timer.h"
#include "pgs_hook.h"
#include "pgs_prelude.h"

PG_MODULE_MAGIC;

typedef enum
{
	PGS_T_VOID, PGS_T_INT, PGS_T_UINT, PGS_T_INT64, PGS_T_UINT64,
	PGS_T_BOOL, PGS_T_POINTER, PGS_T_DATUM, PGS_T_CSTRING,
	PGS_T_BYTES, PGS_T_DOUBLE, PGS_T_FLOAT,
	PGS_T_CHAR, PGS_T_UCHAR, PGS_T_SHORT, PGS_T_USHORT
} PgsCType;

typedef union
{
	int8_t		i8;
	uint8_t		u8;
	int16_t		i16;
	uint16_t	u16;
	int32_t		i32;
	uint32_t	u32;
	int64_t		i64;
	uint64_t	u64;
	bool		b;
	void	   *p;
	double		d;
	float		f;
} PgsVal;

typedef union
{
	ffi_arg		uarg;
	ffi_sarg	sarg;
	void	   *p;
	double		d;
	float		f;
} PgsRet;

#define PGS_CLOSURE_MAXARGS 16

typedef struct PgsClosure
{
	ffi_cif		cif;
	ffi_closure *closure;
	void	   *code;
	JanetFunction *fn;
	PgsCType	rtype;
	int32_t		nargs;
	PgsCType	atypes[PGS_CLOSURE_MAXARGS];
	ffi_type   *ftypes[PGS_CLOSURE_MAXARGS];
} PgsClosure;

static Janet pgs_row_filter;
static bool pgs_row_filter_set = false;
static ExecutorRun_hook_type prev_ExecutorRun_hook = NULL;


JanetTable *pgs_env = NULL;
static bool started = false;

/*
 * The interrupt timer: one thread for the life of the backend, started with
 * the VM, ticking every 100ms. A tick interrupts the VM only while Janet code
 * is running (pgs_janet_depth > 0), and whoever is running it (pgs_run_form,
 * pgs_pcall) answers with a CHECK_FOR_INTERRUPTS and resumes. That covers
 * Janet reached from plain SQL too, a hook body or a row filter with no
 * janet_eval on the stack, which a per-eval timer never saw.
 */
static PgsTimer *pgs_timer = NULL;
static int	pgs_janet_depth = 0;

static void
pgs_interrupt_cb(void *vm)
{
	if (__atomic_load_n(&pgs_janet_depth, __ATOMIC_RELAXED) > 0)
		janet_interpreter_interrupt((JanetVM *) vm);
}

static inline void
pgs_janet_enter(void)
{
	__atomic_add_fetch(&pgs_janet_depth, 1, __ATOMIC_RELAXED);
}

static inline void
pgs_janet_leave(void)
{
	__atomic_sub_fetch(&pgs_janet_depth, 1, __ATOMIC_RELAXED);
}

/*
 * Called after a PG error longjmps out of janet_continue. When another fiber
 * is already running, Janet roots the one it continues and unroots it on the
 * way out. The longjmp skips the unroot, so the fiber and everything on its
 * stack would stay reachable for the life of the backend. Unrooting a fiber
 * that wasn't rooted is a no-op.
 *
 * This doesn't fix everything. The first escape leaves Janet's root_fiber
 * pointing at that dead fiber (nothing public resets it), which also makes
 * every later top-level fiber take the rooted path above. So one fiber is
 * leaked for the life of the backend.
 */
static void
pgs_unroot_fiber(JanetFiber *fiber)
{
	janet_gcunroot(janet_wrap_fiber(fiber));
}

static Janet
jpgs_struct(int32_t argc, Janet *argv)
{
	janet_fixarity(argc, 1);
	return pgs_sym_struct((const char *) janet_getcstring(argv, 0));
}

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
static PgsCType
pgs_parse_type(Janet v)
{
	const uint8_t *k;

	if (!janet_checktype(v, JANET_KEYWORD))
		janet_panicf("scalpel/call: expected a type keyword, got %v", v);
	k = janet_unwrap_keyword(v);

	if (!janet_cstrcmp(k, "void"))
		return PGS_T_VOID;
	if (!janet_cstrcmp(k, "int") || !janet_cstrcmp(k, "int32"))
		return PGS_T_INT;
	if (!janet_cstrcmp(k, "uint") || !janet_cstrcmp(k, "uint32"))
		return PGS_T_UINT;
	if (!janet_cstrcmp(k, "int64"))
		return PGS_T_INT64;
	if (!janet_cstrcmp(k, "uint64"))
		return PGS_T_UINT64;
	if (!janet_cstrcmp(k, "bool"))
		return PGS_T_BOOL;
	if (!janet_cstrcmp(k, "pointer") || !janet_cstrcmp(k, "ptr"))
		return PGS_T_POINTER;
	if (!janet_cstrcmp(k, "datum"))
		return PGS_T_DATUM;
	if (!janet_cstrcmp(k, "cstring") || !janet_cstrcmp(k, "string"))
		return PGS_T_CSTRING;
	if (!janet_cstrcmp(k, "bytes") || !janet_cstrcmp(k, "buf"))
		return PGS_T_BYTES;
	if (!janet_cstrcmp(k, "double"))
		return PGS_T_DOUBLE;
	if (!janet_cstrcmp(k, "float"))
		return PGS_T_FLOAT;
	/* C's names, not int8/int16: in Postgres int8 means eight bytes */
	if (!janet_cstrcmp(k, "char"))
		return PGS_T_CHAR;
	if (!janet_cstrcmp(k, "uchar"))
		return PGS_T_UCHAR;
	if (!janet_cstrcmp(k, "short"))
		return PGS_T_SHORT;
	if (!janet_cstrcmp(k, "ushort"))
		return PGS_T_USHORT;
	janet_panicf("scalpel/call: unknown type :%s", (const char *) k);
}

static ffi_type *
pgs_ffi_type(PgsCType t)
{
	switch (t)
	{
		case PGS_T_VOID:
			return &ffi_type_void;
		case PGS_T_INT:
			return &ffi_type_sint32;
		case PGS_T_UINT:
			return &ffi_type_uint32;
		case PGS_T_INT64:
			return &ffi_type_sint64;
		case PGS_T_UINT64:
			return &ffi_type_uint64;
		case PGS_T_BOOL:
			return &ffi_type_uint8;
		case PGS_T_POINTER:
		case PGS_T_DATUM:
		case PGS_T_CSTRING:
		case PGS_T_BYTES:
			return &ffi_type_pointer;
		case PGS_T_DOUBLE:
			return &ffi_type_double;
		case PGS_T_FLOAT:
			return &ffi_type_float;
		case PGS_T_CHAR:
			return &ffi_type_sint8;
		case PGS_T_UCHAR:
			return &ffi_type_uint8;
		case PGS_T_SHORT:
			return &ffi_type_sint16;
		case PGS_T_USHORT:
			return &ffi_type_uint16;
	}
	return &ffi_type_void;
}

static int64_t
pgs_as_int(Janet v)
{
	JanetIntType it = janet_is_int(v);

	if (it == JANET_INT_S64)
		return janet_unwrap_s64(v);
	if (it == JANET_INT_U64)
		return (int64_t) janet_unwrap_u64(v);
	if (janet_checktype(v, JANET_NUMBER))
		return (int64_t) janet_unwrap_number(v);
	if (janet_checktype(v, JANET_POINTER))
		return (int64_t) (intptr_t) janet_unwrap_pointer(v);
	janet_panicf("scalpel/call: expected number, got %v", v);
}

static void *
pgs_as_ptr(Janet v)
{
	JanetIntType it = janet_is_int(v);

	if (janet_checktype(v, JANET_POINTER))
		return janet_unwrap_pointer(v);
	if (janet_checktype(v, JANET_NIL))
		return NULL;
	if (it == JANET_INT_U64)
		return (void *) (uintptr_t) janet_unwrap_u64(v);
	if (it == JANET_INT_S64)
		return (void *) (uintptr_t) janet_unwrap_s64(v);
	if (janet_checktype(v, JANET_NUMBER))
		return (void *) (uintptr_t) janet_unwrap_number(v);
	janet_panicf("scalpel/call: expected pointer/number, got %v", v);
}

static void
pgs_ffi_pack_arg(PgsCType t, Janet v, PgsVal *s)
{
	switch (t)
	{
		case PGS_T_INT:
			s->i32 = (int32_t) pgs_as_int(v);
			break;
		case PGS_T_UINT:
			s->u32 = (uint32_t) pgs_as_int(v);
			break;
		case PGS_T_INT64:
			s->i64 = pgs_as_int(v);
			break;
		case PGS_T_UINT64:
			s->u64 = (uint64_t) pgs_as_int(v);
			break;
		case PGS_T_BOOL:
			s->b = janet_truthy(v) ? 1 : 0;
			break;
		case PGS_T_POINTER:
		case PGS_T_DATUM:
			s->p = pgs_as_ptr(v);
			break;
		/*
		 * :cstring and :bytes also take an address (or nil for NULL): that's
		 * what a closure hands a hook for a char * argument, and what you
		 * get back from a call that returns one.
		 */
		case PGS_T_CSTRING:
			if (janet_checktype(v, JANET_STRING))
				s->p = (void *) janet_unwrap_string(v);
			else if (janet_checktype(v, JANET_POINTER) || janet_checktype(v, JANET_NIL))
				s->p = pgs_as_ptr(v);
			else
				janet_panicf("scalpel/call: :cstring arg must be a string, pointer, or nil, got %v", v);
			break;
		case PGS_T_BYTES:
			if (janet_checktype(v, JANET_BUFFER))
				s->p = janet_unwrap_buffer(v)->data;
			else if (janet_checktype(v, JANET_STRING))
				s->p = (void *) janet_unwrap_string(v);
			else if (janet_checktype(v, JANET_POINTER) || janet_checktype(v, JANET_NIL))
				s->p = pgs_as_ptr(v);
			else
				janet_panicf("scalpel/call: :bytes arg must be a string, buffer, pointer, or nil, got %v", v);
			break;
		case PGS_T_DOUBLE:
			if (!janet_checktype(v, JANET_NUMBER))
				janet_panicf("scalpel/call: :double arg must be a number, got %v", v);
			s->d = janet_unwrap_number(v);
			break;
		case PGS_T_FLOAT:
			if (!janet_checktype(v, JANET_NUMBER))
				janet_panicf("scalpel/call: :float arg must be a number, got %v", v);
			s->f = (float) janet_unwrap_number(v);
			break;
		case PGS_T_CHAR:
			s->i8 = (int8_t) pgs_as_int(v);
			break;
		case PGS_T_UCHAR:
			s->u8 = (uint8_t) pgs_as_int(v);
			break;
		case PGS_T_SHORT:
			s->i16 = (int16_t) pgs_as_int(v);
			break;
		case PGS_T_USHORT:
			s->u16 = (uint16_t) pgs_as_int(v);
			break;
		case PGS_T_VOID:
			janet_panic("scalpel/call: :void is not a valid argument type");
	}
}

static Janet
pgs_marshal_ret(PgsCType t, PgsRet *r)
{
	switch (t)
	{
		case PGS_T_VOID:
			return janet_wrap_nil();
		case PGS_T_INT:
			return janet_wrap_integer((int32_t) r->sarg);
		case PGS_T_UINT:
			return janet_wrap_number((double) (uint32_t) r->uarg);
		case PGS_T_INT64:
			return janet_wrap_s64((int64_t) r->sarg);
		case PGS_T_UINT64:
			return janet_wrap_u64((uint64_t) r->uarg);
		case PGS_T_BOOL:
			return janet_wrap_boolean((r->uarg & 0xff) != 0);
		case PGS_T_POINTER:
		case PGS_T_DATUM:
		case PGS_T_BYTES:
			return janet_wrap_pointer((void *) (uintptr_t) r->uarg);
		case PGS_T_CSTRING:
			{
				char	   *cs = (char *) (uintptr_t) r->uarg;

				return cs ? janet_cstringv(cs) : janet_wrap_nil();
			}
		case PGS_T_DOUBLE:
			return janet_wrap_number(r->d);
		case PGS_T_FLOAT:
			return janet_wrap_number((double) r->f);
		case PGS_T_CHAR:
			return janet_wrap_integer((int8_t) r->sarg);
		case PGS_T_UCHAR:
			return janet_wrap_integer((uint8_t) r->uarg);
		case PGS_T_SHORT:
			return janet_wrap_integer((int16_t) r->sarg);
		case PGS_T_USHORT:
			return janet_wrap_integer((uint16_t) r->uarg);
	}
	return janet_wrap_nil();
}

typedef struct
{
	ffi_cif    *cif;
	void		(*fn) (void);
	void	  **avalues;
	PgsRet	   *ret;
} PgsCallCtx;

static Datum
thunk_ffi_call(void *arg)
{
	PgsCallCtx *c = (PgsCallCtx *) arg;

	ffi_call(c->cif, c->fn, c->ret, c->avalues);
	return (Datum) 0;
}

static Janet
pgs_do_call(int32_t argc, Janet *argv, bool guarded)
{
	void	   *fnaddr;
	PgsCType	rtype;
	JanetView	at;
	int32_t		nargs;

	/*
	 * On the C stack, not janet_smalloc: Janet frees all scratch memory at
	 * every collection, and a hook body the callee reaches can collect while
	 * this call is still waiting on it.
	 */
	PgsCType	etypes[PGS_CLOSURE_MAXARGS];
	ffi_type   *ftypes[PGS_CLOSURE_MAXARGS];
	void	   *avalues[PGS_CLOSURE_MAXARGS];
	PgsVal		slots[PGS_CLOSURE_MAXARGS];
	ffi_cif		cif;
	PgsRet		ret;
	PgsCallCtx	ctx;
	PgsError	err;
	Datum		dummy;
	bool		ok;

	/* every value is read against the type list, so check the counts first */
	janet_arity(argc, 3, -1);
	fnaddr = pgs_as_ptr(argv[0]);
	rtype = pgs_parse_type(argv[1]);
	at = janet_getindexed(argv, 2);
	nargs = at.len;
	if (argc - 3 != nargs)
		janet_panicf("scalpel/call: %d argument types but %d arguments", nargs, argc - 3);

	/* resolve returns nil for a misspelled name, so don't pass NULL to libffi */
	if (fnaddr == NULL)
		janet_panic("scalpel/call: function address is nil (did resolve fail?)");
	if (nargs > PGS_CLOSURE_MAXARGS)
		janet_panicf("scalpel/call: too many arguments (max %d)", PGS_CLOSURE_MAXARGS);

	for (int32_t i = 0; i < nargs; i++)
	{
		etypes[i] = pgs_parse_type(at.items[i]);
		ftypes[i] = pgs_ffi_type(etypes[i]);
		pgs_ffi_pack_arg(etypes[i], argv[3 + i], &slots[i]);
		avalues[i] = &slots[i];
	}

	if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, nargs, pgs_ffi_type(rtype), ftypes) != FFI_OK)
		janet_panic("scalpel/call: ffi_prep_cif failed");

	memset(&ret, 0, sizeof(ret));
	ctx.cif = &cif;
	ctx.fn = (void (*) (void)) fnaddr;
	ctx.avalues = avalues;
	ctx.ret = &ret;

	if (guarded)
		ok = pgs_guarded_call(thunk_ffi_call, &ctx, &dummy, &err);
	else
	{
		thunk_ffi_call(&ctx);
		ok = true;
	}

	return ok ? pgs_ok(pgs_marshal_ret(rtype, &ret)) : pgs_err_value(&err);
}

static Janet
jpgs_call(int32_t argc, Janet *argv)
{
	return pgs_do_call(argc, argv, true);
}

static Janet
jpgs_call_raw(int32_t argc, Janet *argv)
{
	return pgs_do_call(argc, argv, false);
}

static Janet
jpgs_sig(int32_t argc, Janet *argv)
{
	janet_fixarity(argc, 1);
	return pgs_sym_signature((const char *) janet_getcstring(argv, 0));
}


static Janet
pgs_unmarshal(PgsCType t, void *p)
{
	switch (t)
	{
		case PGS_T_INT:		return janet_wrap_integer(*(int32_t *) p);
		case PGS_T_UINT:	return janet_wrap_number((double) *(uint32_t *) p);
		case PGS_T_INT64:	return janet_wrap_s64(*(int64_t *) p);
		case PGS_T_UINT64:	return janet_wrap_u64(*(uint64_t *) p);
		case PGS_T_BOOL:	return janet_wrap_boolean(*(uint8_t *) p);
		case PGS_T_POINTER:
		case PGS_T_DATUM:
		case PGS_T_CSTRING:
		case PGS_T_BYTES:	return janet_wrap_pointer(*(void **) p);
		case PGS_T_DOUBLE:	return janet_wrap_number(*(double *) p);
		case PGS_T_FLOAT:	return janet_wrap_number((double) *(float *) p);
		case PGS_T_CHAR:	return janet_wrap_integer(*(int8_t *) p);
		case PGS_T_UCHAR:	return janet_wrap_integer(*(uint8_t *) p);
		case PGS_T_SHORT:	return janet_wrap_integer(*(int16_t *) p);
		case PGS_T_USHORT:	return janet_wrap_integer(*(uint16_t *) p);
		case PGS_T_VOID:	break;
	}
	return janet_wrap_nil();
}

static void
pgs_store_ret(PgsCType t, Janet v, void *ret)
{
	switch (t)
	{
		case PGS_T_VOID:	break;
		case PGS_T_INT:		*(ffi_sarg *) ret = (ffi_sarg) (int32_t) pgs_as_int(v); break;
		case PGS_T_UINT:	*(ffi_arg *) ret = (ffi_arg) (uint32_t) pgs_as_int(v); break;
		case PGS_T_INT64:	*(ffi_sarg *) ret = (ffi_sarg) pgs_as_int(v); break;
		case PGS_T_UINT64:
		case PGS_T_DATUM:	*(ffi_arg *) ret = (ffi_arg) (uint64_t) pgs_as_int(v); break;
		case PGS_T_BOOL:	*(ffi_arg *) ret = janet_truthy(v) ? 1 : 0; break;
		case PGS_T_POINTER:
		case PGS_T_CSTRING:
		case PGS_T_BYTES:	*(void **) ret = pgs_as_ptr(v); break;
		case PGS_T_DOUBLE:	*(double *) ret = janet_unwrap_number(v); break;
		case PGS_T_FLOAT:	*(float *) ret = (float) janet_unwrap_number(v); break;
		case PGS_T_CHAR:	*(ffi_sarg *) ret = (ffi_sarg) (int8_t) pgs_as_int(v); break;
		case PGS_T_UCHAR:	*(ffi_arg *) ret = (ffi_arg) (uint8_t) pgs_as_int(v); break;
		case PGS_T_SHORT:	*(ffi_sarg *) ret = (ffi_sarg) (int16_t) pgs_as_int(v); break;
		case PGS_T_USHORT:	*(ffi_arg *) ret = (ffi_arg) (uint16_t) pgs_as_int(v); break;
	}
}

/*
 * janet_pcall wrapper that handles interrupts from the timer. The tick
 * interrupts whichever fiber is running. When a hook body or row filter runs
 * under SQL that a janet_eval started (scalpel/sql, the REPL), that is this
 * call's fiber, not the top-level one. Handle it the same way pgs_run_form
 * does: honor a pending cancel, then resume.
 */
static JanetSignal
pgs_pcall(JanetFunction *fn, int32_t argc, const Janet *argv, Janet *out)
{
	JanetFiber *fiber = NULL;
	JanetSignal sig;

	pgs_janet_enter();
	PG_TRY();
	{
		sig = janet_pcall(fn, argc, argv, out, &fiber);
		while (sig == JANET_SIGNAL_INTERRUPT)
		{
			CHECK_FOR_INTERRUPTS();
			janet_interpreter_interrupt_handled(janet_local_vm());
			sig = janet_continue(fiber, janet_wrap_nil(), out);
		}
	}
	PG_CATCH();
	{
		if (fiber != NULL)
			pgs_unroot_fiber(fiber);
		pgs_janet_leave();
		PG_RE_THROW();
	}
	PG_END_TRY();
	pgs_janet_leave();
	return sig;
}

/*
 * Check whether pgs_store_ret can convert v to a C value of type t. This has
 * to be checked up front because its converters call janet_panic, and once
 * the callback's fiber has finished there is no fiber to catch it (from plain
 * SQL, Janet would exit() the backend). Returns NULL if v is fine, otherwise
 * a description of what was expected.
 */
static const char *
pgs_ret_mismatch(PgsCType t, Janet v)
{
	bool		is_num = janet_checktype(v, JANET_NUMBER) ||
		janet_is_int(v) != JANET_INT_NONE;

	switch (t)
	{
		case PGS_T_VOID:
		case PGS_T_BOOL:
			return NULL;
		case PGS_T_INT:
		case PGS_T_UINT:
		case PGS_T_INT64:
		case PGS_T_UINT64:
		case PGS_T_DATUM:
		case PGS_T_CHAR:
		case PGS_T_UCHAR:
		case PGS_T_SHORT:
		case PGS_T_USHORT:
			return (is_num || janet_checktype(v, JANET_POINTER)) ? NULL : "a number";
		case PGS_T_POINTER:
		case PGS_T_CSTRING:
		case PGS_T_BYTES:
			return (is_num || janet_checktype(v, JANET_POINTER) ||
					janet_checktype(v, JANET_NIL)) ? NULL : "a pointer, number, or nil";
		case PGS_T_DOUBLE:
		case PGS_T_FLOAT:
			return janet_checktype(v, JANET_NUMBER) ? NULL : "a number";
	}
	return NULL;
}

static void
pgs_closure_dispatch(ffi_cif *cif, void *ret, void **args, void *user)
{
	PgsClosure *c = (PgsClosure *) user;
	Janet		jargs[PGS_CLOSURE_MAXARGS];
	Janet		result = janet_wrap_nil();
	JanetSignal sig = JANET_SIGNAL_OK;
	JanetTryState jstate;
	volatile bool panicked = false;
	int32_t		i;

	(void) cif;
	{
		int			gclock = janet_gclock();

		for (i = 0; i < c->nargs; i++)
			jargs[i] = pgs_unmarshal(c->atypes[i], args[i]);
		janet_gcunlock(gclock);
	}

	/*
	 * A PG error raised under the callback (for example hook-fn's raw call to
	 * the original) unwinds straight through its fiber. Restore the VM state
	 * the way Janet's own unwinding would before letting it propagate, since
	 * this may be the only Janet frame between the error and PG's handler.
	 * janet_try is armed inside the PG_TRY, so a Janet panic landing on it
	 * still leaves through PG_END_TRY. The panic is reported after that.
	 */
	PG_TRY();
	{
		if (janet_try(&jstate))
			panicked = true;
		else
			sig = pgs_pcall(c->fn, c->nargs, jargs, &result);
	}
	PG_CATCH();
	{
		janet_restore(&jstate);
		PG_RE_THROW();
	}
	PG_END_TRY();
	janet_restore(&jstate);

	if (panicked)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("pg_scalpel closure: janet panic: %s",
						(const char *) janet_formatc("%q", jstate.payload))));

	if (sig != JANET_SIGNAL_OK)
	{
		JanetString s = janet_formatc("%q", result);

		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("pg_scalpel closure: janet error: %s", (const char *) s)));
	}
	{
		const char *want = pgs_ret_mismatch(c->rtype, result);

		if (want != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("pg_scalpel closure: callback returned %s, expected %s",
							(const char *) janet_formatc("%q", result), want)));
	}
	pgs_store_ret(c->rtype, result, ret);
}

static Janet
jpgs_closure(int32_t argc, Janet *argv)
{
	PgsClosure *c;
	JanetView	at;
	int32_t		i;
	void	   *code = NULL;

	janet_fixarity(argc, 3);
	if (!janet_checktype(argv[2], JANET_FUNCTION))
		janet_panicf("scalpel/closure: third arg must be a function, got %v", argv[2]);

	at = janet_getindexed(argv, 1);
	if (at.len > PGS_CLOSURE_MAXARGS)
		janet_panicf("scalpel/closure: too many arguments (max %d)", PGS_CLOSURE_MAXARGS);

	c = (PgsClosure *) malloc(sizeof(PgsClosure));
	if (c == NULL)
		janet_panic("scalpel/closure: out of memory");
	c->fn = janet_unwrap_function(argv[2]);
	c->rtype = pgs_parse_type(argv[0]);
	c->nargs = at.len;
	for (i = 0; i < at.len; i++)
	{
		c->atypes[i] = pgs_parse_type(at.items[i]);
		c->ftypes[i] = pgs_ffi_type(c->atypes[i]);
	}

	if (ffi_prep_cif(&c->cif, FFI_DEFAULT_ABI, (unsigned) c->nargs,
					 pgs_ffi_type(c->rtype), c->ftypes) != FFI_OK)
	{
		free(c);
		janet_panic("scalpel/closure: ffi_prep_cif failed");
	}
	c->closure = (ffi_closure *) ffi_closure_alloc(sizeof(ffi_closure), &code);
	if (c->closure == NULL)
	{
		free(c);
		janet_panic("scalpel/closure: ffi_closure_alloc failed");
	}
	if (ffi_prep_closure_loc(c->closure, &c->cif, pgs_closure_dispatch, c, code) != FFI_OK)
	{
		ffi_closure_free(c->closure);
		free(c);
		janet_panic("scalpel/closure: ffi_prep_closure_loc failed");
	}
	c->code = code;
	janet_gcroot(janet_wrap_function(c->fn));
	return janet_wrap_pointer(code);
}

static Janet
jpgs_hook(int32_t argc, Janet *argv)
{
	void	   *target;
	void	   *detour;
	void	   *tramp = NULL;
	const char *err = NULL;

	janet_fixarity(argc, 2);
	target = pgs_as_ptr(argv[0]);
	detour = pgs_as_ptr(argv[1]);
	if (target == NULL || detour == NULL)
		janet_panic("scalpel/hook: target and detour must be non-nil addresses");
	if (!pgs_hook_install(target, detour, &tramp, &err))
		janet_panicf("scalpel/hook: %s", err ? err : "install failed");
	return janet_wrap_pointer(tramp);
}

static Janet
jpgs_unhook(int32_t argc, Janet *argv)
{
	void	   *target;
	const char *err = NULL;

	janet_fixarity(argc, 1);
	target = pgs_as_ptr(argv[0]);
	if (!pgs_hook_remove(target, &err))
		janet_panicf("scalpel/unhook: %s", err ? err : "remove failed");
	return janet_wrap_nil();
}

static Janet
jpgs_page_protect(int32_t argc, Janet *argv)
{
	void	   *addr;
	size_t		len;
	bool		writable;

	janet_fixarity(argc, 3);
	addr = pgs_as_ptr(argv[0]);
	len = (size_t) janet_getnumber(argv, 1);
	writable = janet_truthy(argv[2]);
	{
		uintptr_t	pg = (uintptr_t) sysconf(_SC_PAGESIZE);
		uintptr_t	start = (uintptr_t) addr & ~(pg - 1);
		uintptr_t	end = ((uintptr_t) addr + len + pg - 1) & ~(pg - 1);

		if (mprotect((void *) start, (size_t) (end - start),
					 writable ? (PROT_READ | PROT_WRITE | PROT_EXEC)
					 : (PROT_READ | PROT_EXEC)) != 0)
			janet_panicf("scalpel/page-protect: mprotect failed (%s)", strerror(errno));
	}
	return janet_wrap_nil();
}

static Janet
jpgs_flush_icache(int32_t argc, Janet *argv)
{
	void	   *addr;
	size_t		len;

	janet_fixarity(argc, 2);
	addr = pgs_as_ptr(argv[0]);
	len = (size_t) janet_getnumber(argv, 1);
#if defined(__aarch64__) || defined(__arm__)
	__builtin___clear_cache((char *) addr, (char *) addr + len);
#else
	(void) addr;
	(void) len;
#endif
	return janet_wrap_nil();
}

static Janet
jpgs_image_range(int32_t argc, Janet *argv)
{
	const char *mod = NULL;

	janet_arity(argc, 0, 1);
	if (argc == 1 && !janet_checktype(argv[0], JANET_NIL))
		mod = (const char *) janet_getcstring(argv, 0);
	return pgs_image_range(mod);
}

static Janet
jpgs_disasm(int32_t argc, Janet *argv)
{
	int			len = 16;

	janet_arity(argc, 1, 2);
	if (argc >= 2)
		len = (int) janet_getinteger(argv, 1);
	return pgs_disasm(pgs_as_ptr(argv[0]), len);
}

static Datum
thunk_raise(void *arg)
{
	ereport(ERROR,
			(errcode(ERRCODE_RAISE_EXCEPTION),
			 errmsg("pg_scalpel test raise: %s", (char *) arg)));
	return (Datum) 0;
}

static Janet
jpgs_raise(int32_t argc, Janet *argv)
{
	const char *s;
	Datum		result;
	PgsError	err;

	janet_fixarity(argc, 1);
	s = (const char *) janet_getcstring(argv, 0);
	if (pgs_guarded_call(thunk_raise, (void *) s, &result, &err))
		return pgs_ok(janet_wrap_nil());
	return pgs_err_value(&err);
}

static Janet
jpgs_syms(int32_t argc, Janet *argv)
{
	int			limit = 1000;

	janet_arity(argc, 1, 2);
	if (argc >= 2)
		limit = (int) janet_getinteger(argv, 1);
	return pgs_sym_enumerate((const char *) janet_getcstring(argv, 0), limit);
}

static Janet
jpgs_scan(int32_t argc, Janet *argv)
{
	uint8_t    *start;
	size_t		size;
	JanetByteView pat;
	JanetByteView mask;
	bool		have_mask = false;
	JanetArray *arr;
	size_t		i;
	int32_t		j;

	janet_arity(argc, 3, 4);
	start = (uint8_t *) pgs_as_ptr(argv[0]);
	size = (size_t) janet_getnumber(argv, 1);
	pat = janet_getbytes(argv, 2);
	if (argc == 4)
	{
		mask = janet_getbytes(argv, 3);
		have_mask = true;
		if (mask.len != pat.len)
			janet_panic("scalpel/scan: mask length must equal pattern length");
	}
	if (pat.len == 0 || size < (size_t) pat.len)
		return janet_wrap_array(janet_array(0));
	arr = janet_array(0);
	for (i = 0; i + (size_t) pat.len <= size; i++)
	{
		bool		hit = true;

		for (j = 0; j < pat.len; j++)
		{
			if (have_mask && mask.bytes[j] == 0)
				continue;
			if (start[i + j] != pat.bytes[j])
			{
				hit = false;
				break;
			}
		}
		if (hit)
			janet_array_push(arr, janet_wrap_pointer(start + i));
	}
	return janet_wrap_array(arr);
}

static Janet
jpgs_sym_from_addr(int32_t argc, Janet *argv)
{
	janet_fixarity(argc, 1);
	return pgs_sym_from_addr(pgs_as_ptr(argv[0]));
}

static Janet
jpgs_backtrace(int32_t argc, Janet *argv)
{
	int			maxf = 32;

	janet_arity(argc, 0, 1);
	if (argc == 1)
		maxf = (int) janet_getinteger(argv, 0);
	return pgs_backtrace(maxf);
}

typedef struct
{
	const char *typname;
	const char *bytes;
	int			nbytes;
	char	   *out;
} PgsRecvCtx;

static Datum
thunk_recv(void *arg)
{
	PgsRecvCtx *c = (PgsRecvCtx *) arg;
	Oid			typid;
	Oid			recvfn,
				ioparam,
				outfn;
	bool		isvarlena;
	FmgrInfo	flinfo;
	StringInfoData buf;
	Datum		val;

	typid = DatumGetObjectId(DirectFunctionCall1(regtypein,
												 CStringGetDatum(c->typname)));
	getTypeBinaryInputInfo(typid, &recvfn, &ioparam);
	fmgr_info(recvfn, &flinfo);

	initStringInfo(&buf);
	appendBinaryStringInfo(&buf, c->bytes, c->nbytes);

	val = ReceiveFunctionCall(&flinfo, &buf, ioparam, -1);
	pq_getmsgend(&buf);

	getTypeOutputInfo(typid, &outfn, &isvarlena);
	c->out = OidOutputFunctionCall(outfn, val);
	return (Datum) 0;
}

static Janet
jpgs_recv(int32_t argc, Janet *argv)
{
	PgsRecvCtx	ctx;
	PgsError	err;
	Datum		dummy;
	JanetByteView bytes;

	janet_fixarity(argc, 2);
	ctx.typname = (const char *) janet_getcstring(argv, 0);
	bytes = janet_getbytes(argv, 1);
	ctx.bytes = (const char *) bytes.bytes;
	ctx.nbytes = bytes.len;
	ctx.out = NULL;

	if (pgs_guarded_call(thunk_recv, &ctx, &dummy, &err))
		return pgs_ok(ctx.out ? janet_cstringv(ctx.out) : janet_wrap_nil());
	return pgs_err_value(&err);
}

static Janet
jpgs_getarg(int32_t argc, Janet *argv)
{
	FunctionCallInfo fcinfo;
	int32_t		n;
	PgsCType	t;
	Datum		d;

	janet_fixarity(argc, 3);
	fcinfo = (FunctionCallInfo) pgs_as_ptr(argv[0]);
	if (fcinfo == NULL)
		janet_panic("scalpel/getarg: fcinfo is NULL");
	n = (int32_t) pgs_as_int(argv[1]);
	if (n < 0 || n >= fcinfo->nargs)
		janet_panicf("scalpel/getarg: arg index %d out of range (nargs=%d)",
					 (int) n, (int) fcinfo->nargs);
	t = pgs_parse_type(argv[2]);

	if (fcinfo->args[n].isnull)
		return janet_wrap_nil();

	d = fcinfo->args[n].value;
	switch (t)
	{
		case PGS_T_INT:		return janet_wrap_integer((int32_t) DatumGetInt32(d));
		case PGS_T_UINT:	return janet_wrap_number((double) DatumGetUInt32(d));
		case PGS_T_INT64:	return janet_wrap_s64(DatumGetInt64(d));
		case PGS_T_UINT64:
		case PGS_T_DATUM:	return janet_wrap_u64((uint64_t) d);
		case PGS_T_BOOL:	return janet_wrap_boolean(DatumGetBool(d));
		case PGS_T_DOUBLE:	return janet_wrap_number(DatumGetFloat8(d));
		case PGS_T_FLOAT:	return janet_wrap_number((double) DatumGetFloat4(d));
		case PGS_T_POINTER:
		case PGS_T_BYTES:	return janet_wrap_pointer((void *) (uintptr_t) d);
		case PGS_T_CSTRING:	return janet_cstringv((const char *) DatumGetPointer(d));
		case PGS_T_CHAR:	return janet_wrap_integer((int8_t) DatumGetChar(d));
		case PGS_T_UCHAR:	return janet_wrap_integer((uint8_t) DatumGetChar(d));
		case PGS_T_SHORT:	return janet_wrap_integer(DatumGetInt16(d));
		case PGS_T_USHORT:	return janet_wrap_integer(DatumGetUInt16(d));
		case PGS_T_VOID:	janet_panic("scalpel/getarg: :void is not a valid type");
	}
	janet_panic("scalpel/getarg: unknown type");
	return janet_wrap_nil();
}

static Janet
jpgs_setresult_isnull(int32_t argc, Janet *argv)
{
	FunctionCallInfo fcinfo;

	janet_fixarity(argc, 2);
	fcinfo = (FunctionCallInfo) pgs_as_ptr(argv[0]);
	if (fcinfo == NULL)
		janet_panic("scalpel/setresult-isnull: fcinfo is NULL");
	fcinfo->isnull = janet_truthy(argv[1]) ? true : false;
	return janet_wrap_nil();
}

typedef struct
{
	DestReceiver pub;
	DestReceiver *orig;
} PgsInterceptDest;

static void
icept_startup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	DestReceiver *orig = ((PgsInterceptDest *) self)->orig;

	orig->rStartup(orig, operation, typeinfo);
}

static void
pgs_apply_row_filter(TupleTableSlot *slot)
{
	TupleDesc	td = slot->tts_tupleDescriptor;
	int			n = td->natts;
	JanetArray *row = janet_array(n);
	Janet		args[1];
	Janet		out = janet_wrap_nil();
	const Janet *items;
	int32_t		olen;
	int			i;

	slot_getallattrs(slot);
	for (i = 0; i < n; i++)
	{
		if (slot->tts_isnull[i])
			janet_array_push(row, janet_wrap_nil());
		else
		{
			Oid			outfn;
			bool		isvarlena;

			getTypeOutputInfo(TupleDescAttr(td, i)->atttypid, &outfn, &isvarlena);
			janet_array_push(row,
							 janet_cstringv(OidOutputFunctionCall(outfn,
																  slot->tts_values[i])));
		}
	}

	args[0] = janet_wrap_array(row);
	if (pgs_pcall(janet_unwrap_function(pgs_row_filter), 1, args, &out)
		!= JANET_SIGNAL_OK)
		return;
	if (!janet_indexed_view(out, &items, &olen))
		return;

	for (i = 0; i < n && i < olen; i++)
	{
		Janet		cell = items[i];

		if (janet_checktype(cell, JANET_NIL))
			slot->tts_isnull[i] = true;
		else if (janet_checktype(cell, JANET_STRING))
		{
			Oid			infn,
						ioparam;

			getTypeInputInfo(TupleDescAttr(td, i)->atttypid, &infn, &ioparam);
			slot->tts_values[i] =
				OidInputFunctionCall(infn,
									 (char *) janet_unwrap_string(cell),
									 ioparam, TupleDescAttr(td, i)->atttypmod);
			slot->tts_isnull[i] = false;
		}
	}
}

static bool
icept_receive(TupleTableSlot *slot, DestReceiver *self)
{
	PgsInterceptDest *d = (PgsInterceptDest *) self;

	if (pgs_row_filter_set)
		pgs_apply_row_filter(slot);
	return d->orig->receiveSlot(slot, d->orig);
}

static void
icept_shutdown(DestReceiver *self)
{
	DestReceiver *orig = ((PgsInterceptDest *) self)->orig;

	orig->rShutdown(orig);
}

static void
icept_destroy(DestReceiver *self)
{
	DestReceiver *orig = ((PgsInterceptDest *) self)->orig;

	orig->rDestroy(orig);
	pfree(self);
}

static DestReceiver *
pgs_wrap_dest(DestReceiver *orig)
{
	PgsInterceptDest *d = (PgsInterceptDest *) palloc0(sizeof(PgsInterceptDest));

	d->pub.receiveSlot = icept_receive;
	d->pub.rStartup = icept_startup;
	d->pub.rShutdown = icept_shutdown;
	d->pub.rDestroy = icept_destroy;
	d->pub.mydest = orig->mydest;
	d->orig = orig;
	return (DestReceiver *) d;
}

static void
pgs_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count)
{
	DestReceiver *orig = queryDesc->dest;
	bool		wrapped = false;

	if (pgs_row_filter_set && orig != NULL)
	{
		queryDesc->dest = pgs_wrap_dest(orig);
		wrapped = true;
	}
	if (prev_ExecutorRun_hook)
		prev_ExecutorRun_hook(queryDesc, direction, count);
	else
		standard_ExecutorRun(queryDesc, direction, count);
	if (wrapped)
		queryDesc->dest = orig;
}

static bool pgs_icept_hook_installed = false;

static void
pgs_ensure_icept_hook(void)
{
	if (pgs_icept_hook_installed)
		return;
	prev_ExecutorRun_hook = ExecutorRun_hook;
	ExecutorRun_hook = pgs_ExecutorRun;
	pgs_icept_hook_installed = true;
}

static Janet
jscalpel_intercept_rows(int32_t argc, Janet *argv)
{
	janet_fixarity(argc, 1);
	if (pgs_row_filter_set)
	{
		janet_gcunroot(pgs_row_filter);
		pgs_row_filter_set = false;
	}
	if (!janet_checktype(argv[0], JANET_NIL))
	{
		if (!janet_checktype(argv[0], JANET_FUNCTION))
			janet_panicf("scalpel/intercept-rows: expected a function or nil, got %v", argv[0]);
		pgs_row_filter = argv[0];
		janet_gcroot(pgs_row_filter);
		pgs_row_filter_set = true;
		pgs_ensure_icept_hook();
	}
	return janet_wrap_nil();
}

/*
 * scalpel/sql: run SQL in this backend via SPI, guarded. SPI is how a C
 * function already inside query execution runs more SQL (the PLs use it too),
 * so this works from janet_eval and from the REPL. The query runs in this
 * process, so a hook installed earlier fires when it executes. The guard (a
 * subtransaction plus PG_TRY, as in scalpel/call) turns a SQL error into
 * (:error {...}) instead of unwinding the caller.
 */
typedef struct
{
	const char *query;
	long		limit;
	Janet		out;
} PgsSqlCtx;

static Datum
thunk_sql(void *arg)
{
	PgsSqlCtx  *c = (PgsSqlCtx *) arg;

	if (SPI_connect() != SPI_OK_CONNECT)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("pg_scalpel: SPI_connect failed")));

	(void) SPI_execute(c->query, false, c->limit);

	if (SPI_tuptable != NULL)
	{
		SPITupleTable *tt = SPI_tuptable;
		TupleDesc	td = tt->tupdesc;
		JanetArray *rows = janet_array((int32_t) SPI_processed);
		JanetArray *keys = janet_array(td->natts);
		JanetTable *taken = janet_table(td->natts);
		uint64		r;
		int			col;

		/*
		 * One key per column. A name already taken (SELECT 1 AS a, 2 AS a, or
		 * two unnamed ?column?s) gets the column's position appended, so no
		 * column is silently dropped from the row.
		 */
		for (col = 1; col <= td->natts; col++)
		{
			char	   *name = SPI_fname(td, col);
			Janet		k = janet_ckeywordv(name);

			while (!janet_checktype(janet_table_get(taken, k), JANET_NIL))
			{
				name = psprintf("%s_%d", name, col);
				k = janet_ckeywordv(name);
			}
			janet_table_put(taken, k, janet_wrap_true());
			janet_array_push(keys, k);
		}

		for (r = 0; r < SPI_processed; r++)
		{
			HeapTuple	tup = tt->vals[r];
			JanetTable *row = janet_table(td->natts);

			for (col = 1; col <= td->natts; col++)
			{
				char	   *val = SPI_getvalue(tup, td, col);

				janet_table_put(row, keys->data[col - 1],
								val ? janet_cstringv(val) : janet_wrap_nil());
			}
			janet_array_push(rows, janet_wrap_table(row));
		}
		c->out = janet_wrap_array(rows);
	}
	else
		c->out = janet_wrap_integer((int32_t) SPI_processed);

	SPI_finish();
	return (Datum) 0;
}

static Janet
jpgs_sql(int32_t argc, Janet *argv)
{
	PgsSqlCtx	ctx;
	Datum		dummy;
	PgsError	err;

	janet_arity(argc, 1, 2);
	ctx.query = (const char *) janet_getcstring(argv, 0);
	ctx.limit = (argc >= 2) ? (long) janet_getinteger(argv, 1) : 0;
	ctx.out = janet_wrap_nil();

	if (pgs_guarded_call(thunk_sql, &ctx, &dummy, &err))
		return pgs_ok(ctx.out);
	return pgs_err_value(&err);
}

static const JanetReg pgs_cfuns[] = {
	{"backend-pid", jpgs_backend_pid,
	"(scalpel/backend-pid)\n\nThe process id of the backend running this code."},
	{"parse-int", jpgs_parse_int,
	"(scalpel/parse-int str)\n\nParse str as an int4, the way '...'::int does."},
	{"resolve", jpgs_resolve,
	"(scalpel/resolve name)\n\nAddress of symbol name, or nil."},
	{"struct", jpgs_struct,
		"(scalpel/struct name)\n\nDecode a struct's layout from DWARF (a tag name or a "
		"typedef that resolves to one). Returns {:name :size :fields [{:name :offset "
	":size :call} ...]} or nil."},
	{"call", jpgs_call,
	"(scalpel/call addr ret [argtypes] args...)\n\nCall the C function at addr via libffi."},
	{"sql", jpgs_sql,
		"(scalpel/sql query &opt limit)\n\nRun SQL in this backend via SPI, guarded. "
		"Returns (:ok @[{:col val ...} ...]) with values as text for a result set, "
		"(:ok n) rows-processed otherwise, or (:error {...}) on a SQL error."},
	{"sig", jpgs_sig,
		"(scalpel/sig name)\n\nRead a function's signature from DWARF. Returns "
	"{:name :addr :ret :args :call} or nil, where :call is ready for scalpel/call."},
	{"closure", jpgs_closure,
	"(scalpel/closure ret [argtypes] fn)\n\nManufacture a C function pointer that runs the Janet fn."},
	{"hook", jpgs_hook,
	"(scalpel/hook target detour)\n\nInline-hook target to detour; returns a trampoline to the original."},
	{"unhook", jpgs_unhook,
	"(scalpel/unhook target)\n\nRemove a hook installed on target."},
	{"page-protect", jpgs_page_protect,
	"(scalpel/page-protect addr len writable?)\n\nMake a code range writable+exec or restore read+exec."},
	{"flush-icache", jpgs_flush_icache,
	"(scalpel/flush-icache addr len)\n\nFlush the instruction cache after writing code."},
	{"image-range", jpgs_image_range,
	"(scalpel/image-range &opt module)\n\nBase/size of a loaded module and its .text."},
	{"disasm", jpgs_disasm,
	"(scalpel/disasm addr &opt len)\n\nDisassemble live bytes at addr."},
	{"call-raw", jpgs_call_raw,
	"(scalpel/call-raw fn ret [argtypes] args...)\n\nUNGUARDED call; only for leaf fns that cannot ereport."},
	{"raise", jpgs_raise,
	"(scalpel/raise msg)\n\nRaise a test ereport; comes back as [:error {...}]."},
	{"syms", jpgs_syms,
	"(scalpel/syms pattern &opt limit)\n\nEnumerate symbols matching a glob."},
	{"scan", jpgs_scan,
	"(scalpel/scan start size pattern &opt mask)\n\nScan a byte range for a pattern."},
	{"sym-from-addr", jpgs_sym_from_addr,
	"(scalpel/sym-from-addr addr)\n\nNearest preceding symbol for a code address."},
	{"backtrace", jpgs_backtrace,
	"(scalpel/backtrace &opt maxframes)\n\nSymbolized C stack."},
	{"recv", jpgs_recv,
	"(scalpel/recv type-name bytes)\n\nDecode bytes via a type binary-input fn, guarded."},
	{"getarg", jpgs_getarg,
	"(scalpel/getarg fcinfo n type)\n\nRead the n-th fmgr arg from fcinfo as type."},
	{"setresult-isnull", jpgs_setresult_isnull,
	"(scalpel/setresult-isnull fcinfo bool)\n\nSet an fmgr replacement result to SQL NULL."},
	{"intercept-rows", jscalpel_intercept_rows,
	"(scalpel/intercept-rows f|nil)\n\nInstall a per-row output filter, or nil to clear."},
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

		/*
		 * A cancel or statement_timeout applies to the whole statement, not
		 * this call, so re-throw it. PL/pgSQL doesn't trap one either unless
		 * it is named explicitly.
		 */
		if (edata->sqlerrcode == ERRCODE_QUERY_CANCELED)
			ReThrowError(edata);

		err->sqlstate = pstrdup(unpack_sql_state(edata->sqlerrcode));
		err->message = edata->message ? pstrdup(edata->message) : NULL;
		FreeErrorData(edata);
		ok = false;
	}
	PG_END_TRY();

	return ok;
}





/*
 * Tear down a VM that failed partway through pgs_vm_ensure, so the next call
 * starts clean instead of running janet_init() over a live VM.
 */
static void
pgs_vm_abandon(void)
{
	janet_deinit();
	pgs_env = NULL;
}

JanetTable *
pgs_vm_ensure(void)
{
	if (started)
		return pgs_env;
	janet_init();
	pgs_env = janet_core_env(NULL);
	janet_cfuns_prefix(pgs_env, "scalpel", pgs_cfuns);

	/*
	 * Load the Janet helper prelude (make-struct/pack-struct, hook-fn, trace,
	 * patch helpers) so it is available in every REPL. It is compiled into
	 * pgs_prelude.h from janet-lib/ at build time.
	 */
	{
		Janet		pres;

		if (janet_dostring(pgs_env, pgs_prelude, "pg_scalpel/prelude", &pres) != 0)
		{
			pgs_vm_abandon();
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("pg_scalpel: prelude failed to load")));
		}
	}

	pgs_timer = pgs_timer_start(pgs_interrupt_cb, janet_local_vm(), 100);
	if (pgs_timer == NULL)
	{
		int			save_errno = errno;

		pgs_vm_abandon();
		errno = save_errno;
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("pg_scalpel: could not start the interrupt timer: %m")));
	}

	started = true;
	return pgs_env;
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

	PG_TRY();
	{
		for (;;)
		{
			sig = janet_continue(fiber, janet_wrap_nil(), out);
			if (sig != JANET_SIGNAL_INTERRUPT)
				break;
			CHECK_FOR_INTERRUPTS();
			janet_interpreter_interrupt_handled(vm);
		}
	}
	PG_CATCH();
	{
		pgs_unroot_fiber(fiber);
		PG_RE_THROW();
	}
	PG_END_TRY();
	return sig;
}
int
pgs_run_interruptible(const char *src, const char *where, Janet *out)
{
	JanetParser parser;
	JanetTryState jstate;
	size_t		len = strlen(src);
	volatile bool ok = true;

	*out = janet_wrap_nil();
	janet_parser_init(&parser);
	pgs_janet_enter();

	PG_TRY();
	{
		/*
		 * Save the VM's per-call state (recursion depth, current fiber, GC
		 * lock, panic target). A PG error that longjmps out of janet_continue
		 * skips Janet's own restore, so without this every such error leaks
		 * one level of the recursion guard and the connection's VM dies after
		 * ~1000 of them. The setjmp also gives a Janet panic outside any fiber
		 * somewhere to land, and it's armed inside the PG_TRY so that landing
		 * still falls out through PG_END_TRY instead of leaving PG's error
		 * stack pointing into this frame.
		 */
		if (janet_try(&jstate))
		{
			*out = jstate.payload;
			ok = false;
		}
		else
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
	}
	PG_CATCH();
	{
		janet_restore(&jstate);
		pgs_janet_leave();
		janet_parser_deinit(&parser);
		PG_RE_THROW();
	}
	PG_END_TRY();

	janet_restore(&jstate);
	pgs_janet_leave();
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

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("pg_scalpel: must be superuser")));

	pgs_vm_ensure();
	if (pgs_run_interruptible(src, "pg_scalpel", &result))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("janet error: %s", (const char *) janet_to_string(result))));

	s = janet_to_string(result);
	return PointerGetDatum(cstring_to_text_with_len((const char *) s, janet_string_length(s)));
}

/* ------------------------------------------------------------------ */
/* REPL over a per-backend Unix-domain socket (pgs_transport_linux.c).  */
/* ------------------------------------------------------------------ */

/*
 * Run one REPL line in its own subtransaction, the way PL/pgSQL runs a block
 * with an EXCEPTION clause. A PG error the line raises (call-raw into
 * something that ereports, a hook's original failing) rolls back just that
 * line and comes back as text, the same as a Janet error, instead of ending
 * the REPL. A cancel is re-thrown, so it still ends the REPL and the
 * statement. Returns 0 ok, 1 Janet error (in *out), 2 PG error (*pgerr).
 */
static int
pgs_repl_run_line(const char *src, Janet *out, char **pgerr)
{
	MemoryContext oldcontext = CurrentMemoryContext;
	ResourceOwner oldowner = CurrentResourceOwner;
	volatile int rc = 2;

	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(oldcontext);

	PG_TRY();
	{
		rc = pgs_run_interruptible(src, "scalpel-repl", out);
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(oldcontext);
		CurrentResourceOwner = oldowner;
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

		if (edata->sqlerrcode == ERRCODE_QUERY_CANCELED)
			ReThrowError(edata);
		*pgerr = psprintf("%s (SQLSTATE %s)",
						  edata->message ? edata->message : "unknown error",
						  unpack_sql_state(edata->sqlerrcode));
		FreeErrorData(edata);
		rc = 2;
	}
	PG_END_TRY();

	return rc;
}

/*
 * Serve an interactive Janet REPL to one client over the transport layer (a
 * per-pid Unix socket). Blocks the calling backend until the client sends
 * ":quit" or disconnects. Each line is evaluated on this backend thread against
 * the connection's VM, so anything installed (hooks, patches, a row filter)
 * persists after the REPL returns. The socket waits are interruptible, so the
 * waiting backend still honors cancel, terminate and shutdown.
 *
 * A runaway form such as an infinite loop can be cancelled mid-eval, because
 * the interrupt timer gets pgs_run_form to a CHECK_FOR_INTERRUPTS. Like any
 * cancel, that ends the REPL.
 */
static void
scalpel_repl_serve(void)
{
	PgsConn    *volatile c = NULL;
	char	   *endpoint = NULL;
	char	   *buf = palloc(65536);
	char		greeting[128];

	PG_TRY();
	{
		int			n;

		/*
		 * Announce the socket before blocking, so the client can find it.
		 * Both happen inside the PG_TRY: ereport(INFO) runs
		 * CHECK_FOR_INTERRUPTS, so a cancel already pending would otherwise
		 * escape with the socket open and its file left in /tmp.
		 */
		c = pgs_transport_listen(&endpoint);
		ereport(INFO,
				(errmsg("pg_scalpel REPL listening on %s (pid %d)",
						endpoint ? endpoint : "?", (int) MyProcPid)));

		if (!pgs_transport_accept(c))
			ereport(ERROR,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("pg_scalpel: accept() on %s failed: %m", endpoint)));

		snprintf(greeting, sizeof(greeting),
				 "pg_scalpel REPL - Janet %s in backend pid %d\nscalpel> ",
				 JANET_VERSION, (int) MyProcPid);
		pgs_transport_write(c, greeting, (int) strlen(greeting));

		while ((n = pgs_transport_read(c, buf, 65535)) > 0)
		{
			Janet		out = janet_wrap_nil();
			char	   *s = buf;
			Janet		outkw = janet_ckeywordv("out");
			JanetBuffer *cap;
			char	   *pgerr = NULL;
			int			rc;

			buf[n] = '\0';
			while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' ||
							 buf[n - 1] == ' ' || buf[n - 1] == '\t'))
				buf[--n] = '\0';
			while (*s == ' ' || *s == '\t')
				s++;

			if (*s == '\0')
			{
				pgs_transport_write(c, "scalpel> ", 9);
				continue;
			}
			if (strcmp(s, ":quit") == 0)
				break;

			/*
			 * Capture whatever the form prints. Janet's print/prin/pp write to
			 * (dyn :out), and the eval fiber uses pgs_env for dynamic bindings.
			 * Binding :out to a buffer here collects that output instead of
			 * sending it to the backend's stdout. It goes to the client before
			 * the value.
			 */
			cap = janet_buffer(0);
			janet_table_put(pgs_env, outkw, janet_wrap_buffer(cap));
			rc = pgs_repl_run_line(s, &out, &pgerr);
			if (cap->count > 0)
				pgs_transport_write(c, (const char *) cap->data, cap->count);
			janet_table_put(pgs_env, outkw, janet_wrap_nil());

			if (rc == 2)
			{
				char	   *reply = psprintf("error: %s\nscalpel> ", pgerr);

				pgs_transport_write(c, reply, (int) strlen(reply));
				pfree(reply);
				pfree(pgerr);
			}
			else if (rc != 0)
			{
				const char *reply = (const char *) janet_formatc(
					"error: %s\nscalpel> ", (const char *) janet_to_string(out));

				pgs_transport_write(c, reply, (int) strlen(reply));
			}
			else if (janet_checktype(out, JANET_STRING))
			{
				/* a formatted report (the observatory dump, say) prints raw */
				JanetString sv = janet_unwrap_string(out);

				pgs_transport_write(c, (const char *) sv, (int) janet_string_length(sv));
				pgs_transport_write(c, "\nscalpel> ", 10);
			}
			else if (janet_checktype(out, JANET_BUFFER))
			{
				JanetBuffer *bv = janet_unwrap_buffer(out);

				pgs_transport_write(c, (const char *) bv->data, (int) bv->count);
				pgs_transport_write(c, "\nscalpel> ", 10);
			}
			else
			{
				const char *reply = (const char *) janet_formatc("%q\nscalpel> ", out);

				pgs_transport_write(c, reply, (int) strlen(reply));
			}
		}
	}
	PG_CATCH();
	{
		/* don't leave later janet_evals printing into this line's buffer */
		janet_table_put(pgs_env, janet_ckeywordv("out"), janet_wrap_nil());
		pgs_transport_close(c);	/* NULL-safe: listen may not have got there */
		PG_RE_THROW();
	}
	PG_END_TRY();

	pgs_transport_close(c);
}

PG_FUNCTION_INFO_V1(scalpel_repl);

Datum
scalpel_repl(PG_FUNCTION_ARGS)
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("pg_scalpel: must be superuser")));
	pgs_vm_ensure();
	scalpel_repl_serve();
	PG_RETURN_INT32(0);
}
