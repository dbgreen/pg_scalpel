#include "postgres.h"
#include <dlfcn.h>
#include <unistd.h>
#include <stdlib.h>
#include <elfutils/libdwfl.h>
#include <dwarf.h>
#include "janet.h"
#include "pg_scalpel.h"

#define CK(s) janet_ckeywordv(s)
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

static bool
die_type(Dwarf_Die *die, Dwarf_Die *out)
{
    Dwarf_Attribute a;

    if (dwarf_attr_integrate(die, DW_AT_type, &a) == NULL)
        return false;
    return dwarf_formref_die(&a, out) != NULL;
}

static Dwarf_Word
die_udata(Dwarf_Die *die, int attr)
{
    Dwarf_Attribute a;
    Dwarf_Word  v = 0;

    if (dwarf_attr(die, attr, &a) != NULL)
        dwarf_formudata(&a, &v);
    return v;
}
static const char *
dw_call_kw(Dwarf_Die *t)
{
    Dwarf_Die   under;
    int         tag;

    if (t == NULL)
        return "void";
    tag = dwarf_tag(t);
    switch (tag)
    {
        case DW_TAG_base_type:
            {
                Dwarf_Word  enc = die_udata(t, DW_AT_encoding);
                Dwarf_Word  size = die_udata(t, DW_AT_byte_size);

                switch (enc)
                {
                    case DW_ATE_boolean:
                        return "bool";
                    case DW_ATE_float:
                        return (size == 4) ? "float" : "double";
                    case DW_ATE_unsigned:
                    case DW_ATE_unsigned_char:
                        return (size >= 8) ? "uint64" : "uint";
                    default:    /* signed, signed_char, ... */
                        return (size >= 8) ? "int64" : "int";
                }
            }
        case DW_TAG_pointer_type:
            {
                /* char* -> cstring (strip qualifiers on the pointee) */
                if (die_type(t, &under))
                {
                    Dwarf_Die   p = under;

                    while (dwarf_tag(&p) == DW_TAG_const_type ||
                           dwarf_tag(&p) == DW_TAG_volatile_type ||
                           dwarf_tag(&p) == DW_TAG_typedef)
                        if (!die_type(&p, &p))
                            break;
                    if (dwarf_tag(&p) == DW_TAG_base_type &&
                        die_udata(&p, DW_AT_byte_size) == 1)
                    {
                        Dwarf_Word  e = die_udata(&p, DW_AT_encoding);

                        if (e == DW_ATE_signed_char || e == DW_ATE_unsigned_char)
                            return "cstring";
                    }
                }
                return "pointer";
            }
        case DW_TAG_typedef:
        case DW_TAG_const_type:
        case DW_TAG_volatile_type:
        case DW_TAG_restrict_type:
            return die_type(t, &under) ? dw_call_kw(&under) : "pointer";
        case DW_TAG_enumeration_type:
            return "int";
        default:                /* struct/union by value, subroutine, array, ... */
            return "pointer";
    }
}
static bool
die_find(Dwarf_Die *parent, int tag, const char *name, Dwarf_Die *out)
{
    Dwarf_Die   child;

    if (dwarf_child(parent, &child) != 0)
        return false;
    do
    {
        const char *nm = dwarf_diename(&child);

        if (dwarf_tag(&child) == tag && nm != NULL && strcmp(nm, name) == 0)
        {
            *out = child;
            return true;
        }
        if (die_find(&child, tag, name, out))
            return true;
    } while (dwarf_siblingof(&child, &child) == 0);
    return false;
}

static bool
find_die_by_name(int tag, const char *name, Dwarf_Die *out)
{
    Dwarf_Die  *cu = NULL;
    Dwarf_Addr  bias;

    pgs_sym_init();                 /* the same dwfl you brought up in chapter 03 */
    while ((cu = dwfl_nextcu(pgs_dwfl, cu, &bias)) != NULL)
        if (die_find(cu, tag, name, out))
            return true;
    return false;
}

Janet
pgs_sym_struct(const char *name)
{
    Dwarf_Die   st;
    Dwarf_Die   member;
    JanetArray *fields;
    JanetTable *t;

    /* accept a struct tag name, or a typedef pointing at one */
    if (!find_die_by_name(DW_TAG_structure_type, name, &st))
    {
        Dwarf_Die   td;

        if (!find_die_by_name(DW_TAG_typedef, name, &td) ||
            !die_type(&td, &st) ||
            dwarf_tag(&st) != DW_TAG_structure_type)
            return janet_wrap_nil();
    }

    fields = janet_array(0);
    if (dwarf_child(&st, &member) == 0)
    {
        do
        {
            const char *mn;
            Dwarf_Die   mt;
            bool        havemt;
            JanetTable *f;

            if (dwarf_tag(&member) != DW_TAG_member)
                continue;
            mn = dwarf_diename(&member);
            havemt = die_type(&member, &mt);
            f = janet_table(4);
            janet_table_put(f, CK("name"), janet_cstringv(mn ? mn : "?"));
            janet_table_put(f, CK("offset"),
                            janet_wrap_integer((int32_t) die_udata(&member, DW_AT_data_member_location)));
            janet_table_put(f, CK("size"),
                            janet_wrap_integer((int32_t) (havemt ? die_udata(&mt, DW_AT_byte_size) : 0)));
            janet_table_put(f, CK("call"), CK(havemt ? dw_call_kw(&mt) : "pointer"));
            janet_array_push(fields, janet_wrap_table(f));
        } while (dwarf_siblingof(&member, &member) == 0);
    }

    t = janet_table(3);
    janet_table_put(t, CK("name"), janet_cstringv(name));
    janet_table_put(t, CK("size"),
                    janet_wrap_integer((int32_t) die_udata(&st, DW_AT_byte_size)));
    janet_table_put(t, CK("fields"), janet_wrap_array(fields));
    return janet_wrap_table(t);
}



Janet
pgs_sym_signature(const char *name)
{
	Dwarf_Die	fn;
	Dwarf_Die	rt;
	Dwarf_Die	child;
	const char *retkw;
	JanetArray *argkws;
	JanetTable *t;
	Janet		callv[2];

	if (!find_die_by_name(DW_TAG_subprogram, name, &fn))
		return janet_wrap_nil();

	retkw = die_type(&fn, &rt) ? dw_call_kw(&rt) : "void";

	argkws = janet_array(0);
	if (dwarf_child(&fn, &child) == 0)
	{
		do
		{
			Dwarf_Die	pt;

			if (dwarf_tag(&child) != DW_TAG_formal_parameter)
				continue;
			janet_array_push(argkws,
							 CK(die_type(&child, &pt) ? dw_call_kw(&pt) : "pointer"));
		} while (dwarf_siblingof(&child, &child) == 0);
	}

	t = janet_table(4);
	janet_table_put(t, CK("name"), janet_cstringv(name));
	janet_table_put(t, CK("addr"), pgs_sym_resolve(name));
	janet_table_put(t, CK("ret"), CK(retkw));
	janet_table_put(t, CK("args"), janet_wrap_array(argkws));

	callv[0] = CK(retkw);
	callv[1] = janet_wrap_tuple(janet_tuple_n(argkws->data, argkws->count));
	janet_table_put(t, CK("call"), janet_wrap_tuple(janet_tuple_n(callv, 2)));
	return janet_wrap_table(t);
}
