#include "postgres.h"
#include <dlfcn.h>
#include <unistd.h>
#include <stdlib.h>
#include <elfutils/libdwfl.h>
#include "janet.h"
#include "pg_scalpel.h"


static Dwfl *pgs_dwfl;
static char *pgs_debuginfo_path;

void
pgs_sym_init(void)
{
	static const Dwfl_Callbacks cb = {
		.find_elf = dwfl_linux_proc_find_elf,
		.find_debuginfo = dwfl_standard_find_debuginfo,
		.debuginfo_path = &pgs_debuginfo_path,
	};

	if (pgs_dwfl != NULL)
		return;

	unsetenv("DEBUGINFOD_URLS");
	pgs_debuginfo_path = getenv("PG_SCALPEL_DEBUGINFO_PATH");

	pgs_dwfl = dwfl_begin(&cb);
	if (pgs_dwfl == NULL)
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
						errmsg("pg_scalpel: dwfl_begin failed: %s", dwfl_errmsg(-1))));
	if (dwfl_linux_proc_report(pgs_dwfl, getpid()) != 0 ||
		dwfl_report_end(pgs_dwfl, NULL, NULL) != 0)
	{
		dwfl_end(pgs_dwfl);
		pgs_dwfl = NULL;
		ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR),
						errmsg("pg_scalpel: dwfl proc report failed: %s", dwfl_errmsg(-1))));
	}
}

typedef struct
{
	const char *name;
	void	   *addr;
} PgsResolveCtx;

static int
resolve_cb(Dwfl_Module *mod, void **u, const char *modname,
		   Dwarf_Addr start, void *arg)
{
	PgsResolveCtx *c = (PgsResolveCtx *) arg;
	int			n = dwfl_module_getsymtab(mod);

	for (int i = 0; i < n; i++)
	{
		GElf_Sym	sym;
		GElf_Addr	addr = 0;
		const char *nm = dwfl_module_getsym_info(mod, i, &sym, &addr, NULL, NULL, NULL);

		if (nm == NULL || addr == 0 || strcmp(nm, c->name) != 0)
			continue;
		if (GELF_ST_TYPE(sym.st_info) == STT_FUNC ||
			GELF_ST_TYPE(sym.st_info) == STT_GNU_IFUNC)
		{
			c->addr = (void *) (uintptr_t) addr;
			return DWARF_CB_ABORT;
		}
	}
	return DWARF_CB_OK;
}

Janet
pgs_sym_resolve(const char *name)
{
	PgsResolveCtx ctx = {name, NULL};
	void	   *a = dlsym(RTLD_DEFAULT, name);

	if (a != NULL)
		return janet_wrap_pointer(a);

	pgs_sym_init();
	dwfl_getmodules(pgs_dwfl, resolve_cb, &ctx, 0);
	return ctx.addr ? janet_wrap_pointer(ctx.addr) : janet_wrap_nil();
}
