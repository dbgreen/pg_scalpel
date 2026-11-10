#include "postgres.h"
#include "miscadmin.h"
#include <dlfcn.h>
#include <unistd.h>
#include <stdlib.h>
#include <elfutils/libdwfl.h>
#include <gelf.h>
#include <stdio.h>
#include <string.h>
#include <execinfo.h>
#include <dwarf.h>
#include "janet.h"
#include "pg_scalpel.h"

#define CK(s) janet_ckeywordv(s)
static Dwfl *pgs_dwfl;
static char *pgs_debuginfo_path;

/*
 * A cheap fingerprint of what's mapped: how many executable file mappings
 * /proc/self/maps lists. It changes when a library is loaded (LOAD, a PL
 * coming in on first use) or unloaded.
 */
static int
pgs_exec_mappings(void)
{
	FILE	   *f = fopen("/proc/self/maps", "r");
	char		line[512];
	int			n = 0;

	if (f == NULL)
		return -1;
	while (fgets(line, sizeof(line), f) != NULL)
		if (strstr(line, " r-xp ") != NULL && strchr(line, '/') != NULL)
			n++;
	fclose(f);
	return n;
}

/*
 * Set up the dwfl session on first use, and rebuild it whenever the set of
 * mappings has changed, so a library loaded after the first lookup is visible
 * to resolve, syms, sym-from-addr, sig and struct. Nothing holds on to dwfl's
 * objects across calls, so rebuilding is safe. It costs a fresh read of the
 * debug info, but only after a library is loaded or unloaded.
 */
void
pgs_sym_init(void)
{
	static const Dwfl_Callbacks cb = {
		.find_elf = dwfl_linux_proc_find_elf,
		.find_debuginfo = dwfl_standard_find_debuginfo,
		.debuginfo_path = &pgs_debuginfo_path,
	};
	static int	seen_mappings = -1;
	int			now = pgs_exec_mappings();

	if (pgs_dwfl != NULL)
	{
		if (now == seen_mappings)
			return;
		/* rebuild rather than re-report: re-reporting duplicates modules */
		dwfl_end(pgs_dwfl);
		pgs_dwfl = NULL;
	}

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
	seen_mappings = now;
}

typedef struct
{
	const char *name;
	void	   *addr;
	Dwfl_Module *skip;			/* already searched */
} PgsResolveCtx;

/*
 * Find a function symbol named c->name in one module. Only STT_FUNC: a GNU
 * IFUNC's symbol-table address is its resolver, and calling that runs the
 * resolver, not the function (exported IFUNCs are found by dlsym first,
 * which resolves them properly).
 */
static bool
resolve_in(Dwfl_Module *mod, PgsResolveCtx *c)
{
	int			n = dwfl_module_getsymtab(mod);

	for (int i = 0; i < n; i++)
	{
		GElf_Sym	sym;
		GElf_Addr	addr = 0;
		const char *nm = dwfl_module_getsym_info(mod, i, &sym, &addr, NULL, NULL, NULL);

		if (nm == NULL || addr == 0 || strcmp(nm, c->name) != 0)
			continue;
		if (GELF_ST_TYPE(sym.st_info) == STT_FUNC)
		{
			c->addr = (void *) (uintptr_t) addr;
			return true;
		}
	}
	return false;
}

static int
resolve_cb(Dwfl_Module *mod, void **u, const char *modname,
		   Dwarf_Addr start, void *arg)
{
	PgsResolveCtx *c = (PgsResolveCtx *) arg;

	if (mod == c->skip)
		return DWARF_CB_OK;
	return resolve_in(mod, c) ? DWARF_CB_ABORT : DWARF_CB_OK;
}

Janet
pgs_sym_resolve(const char *name)
{
	PgsResolveCtx ctx = {name, NULL, NULL};
	void	   *a = dlsym(RTLD_DEFAULT, name);

	if (a != NULL)
		return janet_wrap_pointer(a);

	pgs_sym_init();

	/*
	 * A static can have the same name in postgres and in a loaded library.
	 * The postgres one is almost always the one wanted, so search that module
	 * (the one holding MyProcPid) first, then the rest.
	 */
	ctx.skip = dwfl_addrmodule(pgs_dwfl, (Dwarf_Addr) (uintptr_t) &MyProcPid);
	if (ctx.skip != NULL && resolve_in(ctx.skip, &ctx))
		return janet_wrap_pointer(ctx.addr);
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
                        return (size == 4) ? "float" : (size == 8) ? "double" : "long-double";
                    case DW_ATE_unsigned:
                    case DW_ATE_unsigned_char:
                        return (size > 8) ? "int128" : (size == 8) ? "uint64" : (size == 2) ? "ushort" :
                            (size == 1) ? "uchar" : "uint";
                    default:    /* signed, signed_char, ... */
                        return (size > 8) ? "int128" : (size == 8) ? "int64" : (size == 2) ? "short" :
                            (size == 1) ? "char" : "int";
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
        /*
         * By-value aggregates can't be passed to libffi as a :pointer. Report
         * the kind, and sig won't build a :call for the function.
         */
        case DW_TAG_structure_type:
        case DW_TAG_class_type:
            return "struct";
        case DW_TAG_union_type:
            return "union";
        case DW_TAG_array_type:
            return "array";
        default:
            return "pointer";
    }
}

/* Is kw one of the types scalpel/call can pass or return? */
static bool
dw_callable(const char *kw)
{
    return strcmp(kw, "struct") != 0 && strcmp(kw, "union") != 0 &&
        strcmp(kw, "array") != 0 && strcmp(kw, "long-double") != 0 &&
        strcmp(kw, "int128") != 0;
}
static bool
die_find(Dwarf_Die *parent, int tag, const char *name, bool defs_only, Dwarf_Die *out)
{
    Dwarf_Die   child;

    if (dwarf_child(parent, &child) != 0)
        return false;
    do
    {
        const char *nm = dwarf_diename(&child);

        if (dwarf_tag(&child) == tag && nm != NULL && strcmp(nm, name) == 0 &&
            !(defs_only && dwarf_hasattr(&child, DW_AT_declaration)))
        {
            *out = child;
            return true;
        }
        if (die_find(&child, tag, name, defs_only, out))
            return true;
    } while (dwarf_siblingof(&child, &child) == 0);
    return false;
}

static bool
find_die_by_name(int tag, const char *name, bool defs_only, Dwarf_Die *out)
{
    Dwarf_Die  *cu = NULL;
    Dwarf_Addr  bias;

    pgs_sym_init();                 /* the same dwfl session scalpel/resolve uses */
    while ((cu = dwfl_nextcu(pgs_dwfl, cu, &bias)) != NULL)
        if (die_find(cu, tag, name, defs_only, out))
            return true;
    return false;
}

/* Byte size of a type, through typedefs and qualifiers (int16 is a typedef). */
static Dwarf_Word
die_sizeof(Dwarf_Die *t)
{
    Dwarf_Die   cur = *t;

    for (int i = 0; i < 16; i++)
    {
        int         tag = dwarf_tag(&cur);

        if (dwarf_hasattr(&cur, DW_AT_byte_size))
            return die_udata(&cur, DW_AT_byte_size);
        if (tag == DW_TAG_pointer_type)
            return sizeof(void *);
        if (!die_type(&cur, &cur))
            break;
    }
    return 0;
}

Janet
pgs_sym_struct(const char *name)
{
    Dwarf_Die   st;
    Dwarf_Die   member;
    JanetArray *fields;
    JanetTable *t;

    /*
     * Accept a struct tag name, or a typedef pointing at one. Only a
     * definition will do: a unit that just forward-declares the struct has
     * a DIE for it too, with no size and no members.
     */
    if (!find_die_by_name(DW_TAG_structure_type, name, true, &st))
    {
        Dwarf_Die   td;
        const char *tag;

        if (!find_die_by_name(DW_TAG_typedef, name, false, &td) ||
            !die_type(&td, &st) ||
            dwarf_tag(&st) != DW_TAG_structure_type)
            return janet_wrap_nil();
        /* the typedef may point at a declaration; find the definition */
        if (dwarf_hasattr(&st, DW_AT_declaration) &&
            ((tag = dwarf_diename(&st)) == NULL ||
             !find_die_by_name(DW_TAG_structure_type, tag, true, &st)))
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
            if (dwarf_hasattr(&member, DW_AT_bit_size))
            {
                /*
                 * A bitfield: report the byte holding its first bit, the bit
                 * within that byte, and its width. DWARF 4+ gives the bit
                 * offset from the start of the struct; older DWARF gives a
                 * storage unit and a big-endian-style offset into it.
                 */
                Dwarf_Word  bits = die_udata(&member, DW_AT_bit_size);
                Dwarf_Word  bitoff;

                if (dwarf_hasattr(&member, DW_AT_data_bit_offset))
                    bitoff = die_udata(&member, DW_AT_data_bit_offset);
                else
                {
                    Dwarf_Word  unit = dwarf_hasattr(&member, DW_AT_byte_size) ?
                        die_udata(&member, DW_AT_byte_size) :
                        (havemt ? die_sizeof(&mt) : 0);

                    bitoff = die_udata(&member, DW_AT_data_member_location) * 8 +
                        unit * 8 - die_udata(&member, DW_AT_bit_offset) - bits;
                }
                janet_table_put(f, CK("offset"), janet_wrap_integer((int32_t) (bitoff / 8)));
                janet_table_put(f, CK("bit-offset"), janet_wrap_integer((int32_t) (bitoff % 8)));
                janet_table_put(f, CK("bit-size"), janet_wrap_integer((int32_t) bits));
                janet_table_put(f, CK("size"),
                                janet_wrap_integer((int32_t) ((bitoff % 8 + bits + 7) / 8)));
                /* not a whole-byte value: pack-struct refuses it, read-struct skips it */
                janet_table_put(f, CK("call"), CK("bitfield"));
            }
            else
            {
                janet_table_put(f, CK("offset"),
                                janet_wrap_integer((int32_t) die_udata(&member, DW_AT_data_member_location)));
                janet_table_put(f, CK("size"),
                                janet_wrap_integer((int32_t) (havemt ? die_sizeof(&mt) : 0)));
                janet_table_put(f, CK("call"), CK(havemt ? dw_call_kw(&mt) : "pointer"));
            }
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
	const char *unsupported = NULL;
	JanetArray *argkws;
	JanetTable *t;
	Janet		callv[2];

	if (!find_die_by_name(DW_TAG_subprogram, name, false, &fn))
		return janet_wrap_nil();

	retkw = die_type(&fn, &rt) ? dw_call_kw(&rt) : "void";
	if (!dw_callable(retkw))
		unsupported = "returns a by-value aggregate or a type libffi can't marshal";

	argkws = janet_array(0);
	if (dwarf_child(&fn, &child) == 0)
	{
		do
		{
			Dwarf_Die	pt;
			const char *kw;

			/* a variadic call needs ffi_prep_cif_var; scalpel/call doesn't do that */
			if (dwarf_tag(&child) == DW_TAG_unspecified_parameters)
			{
				janet_array_push(argkws, CK("..."));
				unsupported = "variadic";
				continue;
			}
			if (dwarf_tag(&child) != DW_TAG_formal_parameter)
				continue;
			kw = die_type(&child, &pt) ? dw_call_kw(&pt) : "pointer";
			if (!dw_callable(kw))
				unsupported = "takes a by-value aggregate or a type libffi can't marshal";
			janet_array_push(argkws, CK(kw));
		} while (dwarf_siblingof(&child, &child) == 0);
	}

	t = janet_table(4);
	janet_table_put(t, CK("name"), janet_cstringv(name));
	janet_table_put(t, CK("addr"), pgs_sym_resolve(name));
	janet_table_put(t, CK("ret"), CK(retkw));
	janet_table_put(t, CK("args"), janet_wrap_array(argkws));

	/* leave out :call if it would build a wrong call */
	if (unsupported != NULL)
	{
		janet_table_put(t, CK("unsupported"), janet_cstringv(unsupported));
		return janet_wrap_table(t);
	}
	callv[0] = CK(retkw);
	callv[1] = janet_wrap_tuple(janet_tuple_n(argkws->data, argkws->count));
	janet_table_put(t, CK("call"), janet_wrap_tuple(janet_tuple_n(callv, 2)));
	return janet_wrap_table(t);
}

typedef struct
{
	const char *want;
	Dwfl_Module *mod;
} PgsImgPick;

static int
img_pick_cb(Dwfl_Module *mod, void **u, const char *name,
			Dwarf_Addr start, void *arg)
{
	PgsImgPick *p = (PgsImgPick *) arg;

	(void) u;
	(void) start;
	if (p->want == NULL || (name != NULL && strstr(name, p->want) != NULL))
	{
		p->mod = mod;
		return DWARF_CB_ABORT;
	}
	return DWARF_CB_OK;
}

Janet
pgs_image_range(const char *module_or_null)
{
	PgsImgPick	pick = {module_or_null, NULL};
	Dwarf_Addr	low = 0,
				high = 0,
				bias = 0;
	Elf		   *elf;
	Elf_Scn    *scn = NULL;
	size_t		shstrndx;
	JanetTable *t;

	pgs_sym_init();
	dwfl_getmodules(pgs_dwfl, img_pick_cb, &pick, 0);
	if (pick.mod == NULL)
		return janet_wrap_nil();
	if (dwfl_module_info(pick.mod, NULL, &low, &high, NULL, NULL, NULL, NULL) == NULL)
		return janet_wrap_nil();

	t = janet_table(4);
	janet_table_put(t, CK("base"), janet_wrap_pointer((void *) (uintptr_t) low));
	janet_table_put(t, CK("size"), janet_wrap_number((double) (high - low)));

	elf = dwfl_module_getelf(pick.mod, &bias);
	if (elf != NULL && elf_getshdrstrndx(elf, &shstrndx) == 0)
	{
		while ((scn = elf_nextscn(elf, scn)) != NULL)
		{
			GElf_Shdr	sh;
			const char *sn;

			if (gelf_getshdr(scn, &sh) == NULL)
				continue;
			sn = elf_strptr(elf, shstrndx, sh.sh_name);
			if (sn != NULL && strcmp(sn, ".text") == 0)
			{
				janet_table_put(t, CK("text"),
								janet_wrap_pointer((void *) (uintptr_t) (sh.sh_addr + bias)));
				janet_table_put(t, CK("textsize"), janet_wrap_number((double) sh.sh_size));
				break;
			}
		}
	}
	return janet_wrap_table(t);
}

static bool
glob_match(const char *pat, const char *str)
{
	while (*pat)
	{
		if (*pat == '*')
		{
			pat++;
			if (!*pat)
				return true;
			for (; *str; str++)
				if (glob_match(pat, str))
					return true;
			return false;
		}
		else if (*pat == '?')
		{
			if (!*str)
				return false;
			pat++;
			str++;
		}
		else
		{
			if (*pat != *str)
				return false;
			pat++;
			str++;
		}
	}
	return !*str;
}

typedef struct
{
	const char *pat;
	int			limit;
	JanetArray *out;
} PgsEnumCtx;

static int
enum_cb(Dwfl_Module *mod, void **u, const char *modname,
		Dwarf_Addr start, void *arg)
{
	PgsEnumCtx *c = (PgsEnumCtx *) arg;
	int			n = dwfl_module_getsymtab(mod);
	int			i;

	(void) u;
	(void) modname;
	(void) start;
	for (i = 0; i < n && c->out->count < c->limit; i++)
	{
		GElf_Sym	sym;
		GElf_Addr	addr = 0;
		const char *nm = dwfl_module_getsym_info(mod, i, &sym, &addr,
												 NULL, NULL, NULL);
		JanetTable *t;

		if (nm == NULL || nm[0] == '\0' || addr == 0)
			continue;
		if (GELF_ST_TYPE(sym.st_info) != STT_FUNC &&
			GELF_ST_TYPE(sym.st_info) != STT_GNU_IFUNC)
			continue;
		if (!glob_match(c->pat, nm))
			continue;
		t = janet_table(2);
		janet_table_put(t, CK("name"), janet_cstringv(nm));
		janet_table_put(t, CK("addr"),
						janet_wrap_pointer((void *) (uintptr_t) addr));
		janet_array_push(c->out, janet_wrap_table(t));
	}
	return (c->out->count < c->limit) ? DWARF_CB_OK : DWARF_CB_ABORT;
}

Janet
pgs_sym_enumerate(const char *pattern, int limit)
{
	PgsEnumCtx	ctx;

	pgs_sym_init();
	if (limit <= 0)
		limit = 1000;
	ctx.pat = pattern;
	ctx.limit = limit;
	ctx.out = janet_array(0);
	dwfl_getmodules(pgs_dwfl, enum_cb, &ctx, 0);
	return janet_wrap_array(ctx.out);
}

Janet
pgs_sym_from_addr(void *addr)
{
	Dwfl_Module *m;
	GElf_Off	off = 0;
	GElf_Sym	sym;
	const char *nm;
	Dwarf_Addr	a = (Dwarf_Addr) (uintptr_t) addr;
	JanetTable *t;

	pgs_sym_init();
	m = dwfl_addrmodule(pgs_dwfl, a);
	if (m == NULL)
		return janet_wrap_nil();
	nm = dwfl_module_addrinfo(m, a, &off, &sym, NULL, NULL, NULL);
	if (nm == NULL)
		return janet_wrap_nil();
	t = janet_table(3);
	janet_table_put(t, CK("name"), janet_cstringv(nm));
	janet_table_put(t, CK("addr"),
					janet_wrap_pointer((void *) (uintptr_t) (a - off)));
	janet_table_put(t, CK("displacement"), janet_wrap_number((double) off));
	return janet_wrap_table(t);
}

Janet
pgs_backtrace(int maxframes)
{
	void	   *frames[128];
	int			n,
				i;
	JanetArray *arr;

	if (maxframes < 1)
		maxframes = 32;
	if (maxframes > 128)
		maxframes = 128;
	pgs_sym_init();
	n = backtrace(frames, maxframes);
	arr = janet_array(0);
	for (i = 1; i < n; i++)
	{
		Janet		sym = pgs_sym_from_addr(frames[i]);
		JanetTable *t;

		if (janet_checktype(sym, JANET_TABLE))
			t = janet_unwrap_table(sym);
		else
		{
			t = janet_table(1);
			janet_table_put(t, CK("addr"), janet_wrap_pointer(frames[i]));
		}
		janet_array_push(arr, janet_wrap_table(t));
	}
	return janet_wrap_array(arr);
}
