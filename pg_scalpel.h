#ifndef PG_SCALPEL_H
#define PG_SCALPEL_H

extern JanetTable *pgs_env;
extern JanetTable *pgs_vm_ensure(void);
extern int pgs_run_interruptible(const char *src, const char *where, Janet *out);

#endif
