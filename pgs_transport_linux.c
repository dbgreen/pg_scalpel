/*
 * pgs_transport_linux.c — the REPL transport: a per-backend Unix-domain socket
 * at /tmp/pg_scalpel-<pid>.sock. Access is gated by SO_PEERCRED, so only a
 * process whose uid matches this backend's own euid may connect, and the socket
 * is created 0600. Every wait goes through WaitLatchOrSocket with a
 * CHECK_FOR_INTERRUPTS at each wake, so a backend parked in the REPL still
 * honors statement_timeout, pg_cancel_backend, and shutdown.
 */
#include "postgres.h"

#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "miscadmin.h"
#include "storage/latch.h"
#include "utils/wait_classes.h"

#include "janet.h"
#include "pg_scalpel.h"

struct PgsConn
{
	int			listen_fd;
	int			fd;				/* accepted client */
	char		path[108];		/* sun_path max */
};

/* Build the per-pid socket, then interruptibly accept ONE same-uid client. */
PgsConn *
pgs_transport_accept(char **endpoint_out)
{
	struct sockaddr_un sa;
	PgsConn    *c;
	int			lfd;

	lfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (lfd < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("pg_scalpel: socket() failed: %m")));

	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "/tmp/pg_scalpel-%d.sock", (int) MyProcPid);
	unlink(sa.sun_path);		/* clear any stale socket from a prior crash */

	if (bind(lfd, (struct sockaddr *) &sa, sizeof(sa)) != 0)
	{
		int			e = errno;

		close(lfd);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("pg_scalpel: bind(%s) failed: %s", sa.sun_path, strerror(e))));
	}
	(void) chmod(sa.sun_path, 0600);
	if (listen(lfd, 1) != 0)
	{
		int			e = errno;

		close(lfd);
		unlink(sa.sun_path);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("pg_scalpel: listen() failed: %s", strerror(e))));
	}

	for (;;)
	{
		int			rc = WaitLatchOrSocket(MyLatch,
										   WL_LATCH_SET | WL_SOCKET_READABLE | WL_EXIT_ON_PM_DEATH,
										   lfd, -1, PG_WAIT_EXTENSION);

		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();	/* may longjmp out — scalpel_repl's PG_CATCH cleans up */

		if (rc & WL_SOCKET_READABLE)
		{
			struct ucred cred;
			socklen_t	clen = sizeof(cred);
			int			cfd = accept(lfd, NULL, NULL);

			if (cfd < 0)
			{
				if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
					continue;
				break;
			}
			/* same-uid gate */
			if (getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cred, &clen) != 0 ||
				cred.uid != geteuid())
			{
				const char *msg = "pg_scalpel: connection refused (uid mismatch)\n";

				(void) send(cfd, msg, strlen(msg), MSG_NOSIGNAL);
				close(cfd);
				continue;		/* keep listening for the right client */
			}
			c = (PgsConn *) palloc0(sizeof(PgsConn));
			c->listen_fd = lfd;
			c->fd = cfd;
			strlcpy(c->path, sa.sun_path, sizeof(c->path));
			if (endpoint_out)
				*endpoint_out = pstrdup(sa.sun_path);
			return c;
		}
	}

	close(lfd);
	unlink(sa.sun_path);
	return NULL;
}

/* Interruptible read; returns bytes read, 0 on disconnect/error. */
int
pgs_transport_read(PgsConn *c, char *buf, int cap)
{
	if (c == NULL || c->fd < 0)
		return 0;
	for (;;)
	{
		int			rc = WaitLatchOrSocket(MyLatch,
										   WL_LATCH_SET | WL_SOCKET_READABLE | WL_EXIT_ON_PM_DEATH,
										   c->fd, -1, PG_WAIT_EXTENSION);

		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();

		if (rc & WL_SOCKET_READABLE)
		{
			ssize_t		n = recv(c->fd, buf, cap, 0);

			if (n < 0)
			{
				if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
					continue;
				return 0;
			}
			return (int) n;		/* 0 == peer closed */
		}
	}
}

/* Best-effort write of the whole buffer. */
void
pgs_transport_write(PgsConn *c, const char *buf, int len)
{
	int			off = 0;

	if (c == NULL || c->fd < 0)
		return;
	while (off < len)
	{
		ssize_t		n = send(c->fd, buf + off, len - off, MSG_NOSIGNAL);

		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			break;				/* peer gone / broken pipe: give up */
		}
		off += (int) n;
	}
}

/* Disconnect + release + unlink the socket. NULL-safe. */
void
pgs_transport_close(PgsConn *c)
{
	if (c == NULL)
		return;
	if (c->fd >= 0)
		close(c->fd);
	if (c->listen_fd >= 0)
		close(c->listen_fd);
	if (c->path[0])
		unlink(c->path);
	pfree(c);
}
