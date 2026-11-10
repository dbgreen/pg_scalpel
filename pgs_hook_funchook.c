/*
 * pgs_hook_funchook.c
 *		funchook backend for pgs_hook.h.
 *
 * Wraps funchook (https://github.com/kubo/funchook): one funchook_t per hooked
 * target. funchook_prepare() rewrites our trampoline pointer in place to the
 * address that runs the original; funchook_install() arms the inline patch.
 */
#include "postgres.h"
#include "pgs_hook.h"

#include <funchook.h>

#define PGS_HOOK_MAX 64

typedef struct PgsHookEntry
{
	void	   *target;			/* NULL == free slot */
	funchook_t *fh;
} PgsHookEntry;

static PgsHookEntry pgs_hooks[PGS_HOOK_MAX];

bool
pgs_hook_install(void *target, void *detour, void **trampoline,
				 const char **errmsg)
{
	funchook_t *fh;
	void	   *tramp = target;	/* in/out: funchook rewrites this to the trampoline */
	int			slot = -1;
	int			i;
	int			rv;

	for (i = 0; i < PGS_HOOK_MAX; i++)
	{
		if (pgs_hooks[i].target == target)
		{
			*errmsg = "target is already hooked";
			return false;
		}
		if (slot < 0 && pgs_hooks[i].target == NULL)
			slot = i;
	}
	if (slot < 0)
	{
		*errmsg = "hook table full";
		return false;
	}

	fh = funchook_create();
	if (fh == NULL)
	{
		*errmsg = "funchook_create failed";
		return false;
	}

	rv = funchook_prepare(fh, &tramp, detour);
	if (rv != 0)
	{
		*errmsg = funchook_error_message(fh);
		funchook_destroy(fh);
		return false;
	}

	rv = funchook_install(fh, 0);
	if (rv != 0)
	{
		*errmsg = funchook_error_message(fh);
		funchook_destroy(fh);
		return false;
	}

	pgs_hooks[slot].target = target;
	pgs_hooks[slot].fh = fh;
	*trampoline = tramp;
	return true;
}

bool
pgs_hook_remove(void *target, const char **errmsg)
{
	int			i;

	for (i = 0; i < PGS_HOOK_MAX; i++)
	{
		if (pgs_hooks[i].target == target)
		{
			funchook_t *fh = pgs_hooks[i].fh;
			int			rv = funchook_uninstall(fh, 0);

			if (rv != 0)
			{
				*errmsg = funchook_error_message(fh);
				return false;
			}
			funchook_destroy(fh);
			pgs_hooks[i].target = NULL;
			pgs_hooks[i].fh = NULL;
			return true;
		}
	}
	*errmsg = "target is not hooked";
	return false;
}
