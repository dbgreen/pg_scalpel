#include "postgres.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "libpq/pqformat.h"
#include "lib/stringinfo.h"
#include "executor/executor.h"
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

PG_MODULE_MAGIC;

typedef enum
{
	PGS_T_VOID, PGS_T_INT, PGS_T_UINT, PGS_T_INT64, PGS_T_UINT64,
	PGS_T_BOOL, PGS_T_POINTER, PGS_T_DATUM, PGS_T_CSTRING,
	PGS_T_BYTES, PGS_T_DOUBLE, PGS_T_FLOAT
} PgsCType;

typedef union
{
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
static int	pgs_icept_col = -1;
static bool pgs_icept_isnull = false;
static int32 pgs_icept_value = 0;
static ExecutorRun_hook_type prev_ExecutorRun_hook = NULL;


JanetTable *pgs_env = NULL;
static bool started = false;

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
	const uint8_t *k = janet_unwrap_keyword(v);

	if (!janet_cstrcmp(k, "void"))
		return PGS_T_VOID;
	if (!janet_cstrcmp(k, "int"))
		return PGS_T_INT;
	if (!janet_cstrcmp(k, "uint64"))
		return PGS_T_UINT64;
	if (!janet_cstrcmp(k, "pointer") || !janet_cstrcmp(k, "ptr"))
		return PGS_T_POINTER;
	if (!janet_cstrcmp(k, "cstring"))
		return PGS_T_CSTRING;
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
		case PGS_T_UINT64:
			return &ffi_type_uint64;
		case PGS_T_POINTER:
		case PGS_T_DATUM:
		case PGS_T_CSTRING:
		case PGS_T_BYTES:
			return &ffi_type_pointer;
		case PGS_T_DOUBLE:
			return &ffi_type_double;
		default:
			return &ffi_type_void;
	}
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
		case PGS_T_UINT64:
			s->u64 = (uint64_t) pgs_as_int(v);
			break;
		case PGS_T_POINTER:
		case PGS_T_DATUM:
			s->p = pgs_as_ptr(v);
			break;
		case PGS_T_CSTRING:
			s->p = (void *) janet_unwrap_string(v);
			break;
		case PGS_T_DOUBLE:
			s->d = janet_unwrap_number(v);
			break;

		default:
			janet_panicf("scalpel/call: cannot pass type #%d", (int) t);
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
		case PGS_T_INT64:
			return janet_wrap_s64((int64_t) r->sarg);
		case PGS_T_UINT64:
			return janet_wrap_u64((uint64_t) r->uarg);
		case PGS_T_POINTER:
		case PGS_T_DATUM:
			return janet_wrap_pointer((void *) (uintptr_t) r->uarg);
		case PGS_T_CSTRING:
			{
				char	   *cs = (char *) (uintptr_t) r->uarg;

				return cs ? janet_cstringv(cs) : janet_wrap_nil();
			}

		default:
			return janet_wrap_nil();
	}
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
	void	   *fnaddr = pgs_as_ptr(argv[0]);
	PgsCType	rtype = pgs_parse_type(argv[1]);
	JanetView	at = janet_getindexed(argv, 2);
	int32_t		nargs = at.len;

	PgsCType   *etypes = janet_smalloc(sizeof(*etypes) * nargs);
	ffi_type  **ftypes = janet_smalloc(sizeof(*ftypes) * nargs);
	void	  **avalues = janet_smalloc(sizeof(*avalues) * nargs);
	PgsVal	   *slots = janet_smalloc(sizeof(*slots) * nargs);
	ffi_cif		cif;
	PgsRet		ret;
	PgsCallCtx	ctx;
	PgsError	err;
	Datum		dummy;
	bool		ok;

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

	janet_sfree(etypes);
	janet_sfree(ftypes);
	janet_sfree(avalues);
	janet_sfree(slots);

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
	}
}

static void
pgs_closure_dispatch(ffi_cif *cif, void *ret, void **args, void *user)
{
	PgsClosure *c = (PgsClosure *) user;
	Janet		jargs[PGS_CLOSURE_MAXARGS];
	Janet		result = janet_wrap_nil();
	JanetFiber *fiber = NULL;
	JanetSignal sig;
	int32_t		i;

	(void) cif;
	{
		int			gclock = janet_gclock();

		for (i = 0; i < c->nargs; i++)
			jargs[i] = pgs_unmarshal(c->atypes[i], args[i]);
		janet_gcunlock(gclock);
	}

	sig = janet_pcall(c->fn, c->nargs, jargs, &result, &fiber);
	if (sig != JANET_SIGNAL_OK)
	{
		JanetString s = janet_formatc("%q", result);

		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("pg_scalpel closure: janet error: %s", (const char *) s)));
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
	int			col;
	bool		isnull;
	int32		value;
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
	JanetFiber *fiber = NULL;
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
	if (janet_pcall(janet_unwrap_function(pgs_row_filter), 1, args, &out, &fiber)
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
	else if (d->col >= 0 && d->col < slot->tts_tupleDescriptor->natts)
	{
		slot_getallattrs(slot);
		if (d->isnull)
			slot->tts_isnull[d->col] = true;
		else if (TupleDescAttr(slot->tts_tupleDescriptor, d->col)->atttypid == INT4OID)
		{
			slot->tts_values[d->col] = Int32GetDatum(d->value);
			slot->tts_isnull[d->col] = false;
		}
	}
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
	d->col = pgs_icept_col;
	d->isnull = pgs_icept_isnull;
	d->value = pgs_icept_value;
	return (DestReceiver *) d;
}

static void
pgs_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count)
{
	DestReceiver *orig = queryDesc->dest;
	bool		wrapped = false;

	if ((pgs_icept_col >= 0 || pgs_row_filter_set) && orig != NULL)
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
