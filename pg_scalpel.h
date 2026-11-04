#ifndef PG_SCALPEL_H
#define PG_SCALPEL_H

typedef Datum (*PgsThunk) (void *arg);

typedef struct PgsError
{
	char	   *sqlstate;
	char	   *message;
} PgsError;

extern bool pgs_guarded_call(PgsThunk thunk, void *arg, Datum *result, PgsError *err);
extern Janet pgs_ok(Janet value);
extern Janet pgs_err_value(PgsError *e);

extern JanetTable *pgs_env;
extern JanetTable *pgs_vm_ensure(void);
extern int	pgs_run_interruptible(const char *src, const char *where, Janet *out);

extern void pgs_sym_init(void);
extern Janet pgs_sym_resolve(const char *name);
extern Janet pgs_sym_struct(const char *name);
extern Janet pgs_sym_signature(const char *name);
extern Janet pgs_image_range(const char *module_or_null);
extern Janet pgs_disasm(void *addr, int len);

#endif
