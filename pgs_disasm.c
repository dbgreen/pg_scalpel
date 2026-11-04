/*
 * pgs_disasm.c — live disassembly (scalpel/disasm) via capstone.
 *
 * The one "read the machine code" primitive that actually benefits from being
 * in-process: disassemble the LIVE bytes at an address — a patched prologue, a
 * funchook trampoline, or JIT-generated code that isn't on disk — rather than
 * what a static tool would see. Uses the capstone already linked into
 * pg_scalpel.so (funchook's bundled copy; header pinned to the same version).
 *
 * INSTRUMENT-only.
 *
 * CAUTION: reads process memory directly at ADDR. Pass a valid, readable code
 * address (e.g. from scalpel/resolve); an unmapped range faults the backend, the
 * same contract as scalpel/scan.
 */
#include "postgres.h"

#include <capstone/capstone.h>

#include "janet.h"
#include "pg_scalpel.h"

#define CK(s) janet_ckeywordv(s)

/* addr,len -> @[{:addr :size :bytes "48 89 e5" :text "mov rbp, rsp"} ...].
 * Disassembles up to LEN bytes of x86-64 starting at ADDR. */
Janet
pgs_disasm(void *addr, int len)
{
	csh			handle;
	cs_insn    *insn;
	size_t		count,
				i;
	JanetArray *out;

	if (len <= 0)
		return janet_wrap_array(janet_array(0));
	if (cs_open(CS_ARCH_X86, CS_MODE_64, &handle) != CS_ERR_OK)
		janet_panic("scalpel/disasm: cs_open failed");

	out = janet_array(0);
	count = cs_disasm(handle, (const uint8_t *) addr, (size_t) len,
					  (uint64_t) (uintptr_t) addr, 0, &insn);
	for (i = 0; i < count; i++)
	{
		JanetTable *t = janet_table(4);
		char		hexbuf[64];
		char		textbuf[200];
		int			off = 0;
		uint16_t	k;

		for (k = 0; k < insn[i].size && k < 16; k++)
			off += snprintf(hexbuf + off, sizeof(hexbuf) - off,
							k ? " %02x" : "%02x", insn[i].bytes[k]);

		if (insn[i].op_str[0] != '\0')
			snprintf(textbuf, sizeof(textbuf), "%s %s",
					 insn[i].mnemonic, insn[i].op_str);
		else
			snprintf(textbuf, sizeof(textbuf), "%s", insn[i].mnemonic);

		janet_table_put(t, CK("addr"),
						janet_wrap_pointer((void *) (uintptr_t) insn[i].address));
		janet_table_put(t, CK("size"), janet_wrap_integer((int32_t) insn[i].size));
		janet_table_put(t, CK("bytes"), janet_cstringv(hexbuf));
		janet_table_put(t, CK("text"), janet_cstringv(textbuf));
		janet_array_push(out, janet_wrap_table(t));
	}
	if (count > 0)
		cs_free(insn, count);
	cs_close(&handle);
	return janet_wrap_array(out);
}
