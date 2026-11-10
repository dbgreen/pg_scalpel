#include "postgres.h"

#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "pgs_timer.h"

struct PgsTimer
{
	pthread_t	thread;
	int			tickfd;
	int			stopfd;
	PgsTimerCallback cb;
	void	   *arg;
};

static void *
pgs_timer_loop(void *param)
{
	PgsTimer   *t = (PgsTimer *) param;
	struct pollfd fds[2];

	fds[0].fd = t->tickfd;
	fds[0].events = POLLIN;
	fds[1].fd = t->stopfd;
	fds[1].events = POLLIN;

	for (;;)
	{
		uint64_t	ticks;

		if (poll(fds, 2, -1) < 0)
		{
			if (errno == EINTR)
				continue;
			break;
		}
		if (fds[1].revents & POLLIN)
			break;
		if ((fds[0].revents & POLLIN) &&
			read(t->tickfd, &ticks, sizeof(ticks)) == sizeof(ticks))
			t->cb(t->arg);
	}
	return NULL;
}

PgsTimer *
pgs_timer_start(PgsTimerCallback cb, void *arg, int period_ms)
{
	PgsTimer   *t = malloc(sizeof(PgsTimer));
	struct itimerspec its;
	sigset_t	all,
				old;
	int			rc;

	if (t == NULL)
		return NULL;
	t->cb = cb;
	t->arg = arg;
	t->tickfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
	t->stopfd = eventfd(0, EFD_CLOEXEC);

	its.it_value.tv_sec = period_ms / 1000;
	its.it_value.tv_nsec = (long) (period_ms % 1000) * 1000000L;
	its.it_interval = its.it_value;

	if (t->tickfd < 0 || t->stopfd < 0 ||
		timerfd_settime(t->tickfd, 0, &its, NULL) != 0)
		rc = -1;
	else
	{
		/*
		 * The thread inherits the mask in effect here, and the kernel hands a
		 * process-directed signal to any thread not blocking it. Unblocked,
		 * PG's handlers (cancel, die, the timeout alarm, latch wakeups) could
		 * run on this thread, concurrently with the backend, and would be
		 * delivered here whenever the backend has them blocked. The thread
		 * only polls two fds, so block everything.
		 */
		sigfillset(&all);
		pthread_sigmask(SIG_BLOCK, &all, &old);
		rc = pthread_create(&t->thread, NULL, pgs_timer_loop, t);
		pthread_sigmask(SIG_SETMASK, &old, NULL);
	}

	if (rc != 0)
	{
		if (t->tickfd >= 0)
			close(t->tickfd);
		if (t->stopfd >= 0)
			close(t->stopfd);
		free(t);
		return NULL;
	}
	return t;
}

void
pgs_timer_stop(PgsTimer *timer)
{
	uint64_t	one = 1;

	if (timer == NULL)
		return;
	if (write(timer->stopfd, &one, sizeof(one)) != sizeof(one))
		elog(WARNING, "pg_scalpel: could not signal the interrupt timer to stop");
	pthread_join(timer->thread, NULL);
	close(timer->tickfd);
	close(timer->stopfd);
	free(timer);
}
