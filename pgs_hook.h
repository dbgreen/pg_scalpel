/*
 * pgs_hook.h
 *		Inline function hooking with a trampoline.
 *
 * Arch- and OS-neutral interface. The implementation (length disassembly,
 * trampoline construction, relocation, executable allocation) lives in a
 * backend file (pgs_hook_<engine>.c), so it can be ported to another
 * architecture without changing callers.
 *
 * Current backend: funchook (pgs_hook_funchook.c). funchook and the capstone
 * disassembler both support x86-64 and arm64, but the disassembler is currently
 * pinned to x86-64 (pgs_disasm.c) and the hand-emitted patch stubs (patch.janet)
 * are x86-64 only, so the patch/replace tier is x86-64 today.
 */
#ifndef PGS_HOOK_H
#define PGS_HOOK_H

#include "postgres.h"

/*
 * Install an inline hook: redirect calls to TARGET into DETOUR. On success,
 * *trampoline receives an address that, when called with TARGET's own
 * signature, runs the original function (the overwritten prologue, relocated,
 * plus a jump back). Returns true on success. On failure, returns false and
 * sets *errmsg to a message owned by the backend; copy it to keep it. The hook
 * affects only this backend process.
 */
extern bool pgs_hook_install(void *target, void *detour,
							  void **trampoline, const char **errmsg);

/* Remove a hook previously installed on TARGET (restores the original bytes). */
extern bool pgs_hook_remove(void *target, const char **errmsg);

#endif							/* PGS_HOOK_H */
