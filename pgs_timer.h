#ifndef PGS_TIMER_H
#define PGS_TIMER_H

typedef struct PgsTimer PgsTimer;
typedef void (*PgsTimerCallback) (void *arg);

extern PgsTimer *pgs_timer_start(PgsTimerCallback cb, void *arg, int period_ms);
extern void pgs_timer_stop(PgsTimer *timer);

#endif


