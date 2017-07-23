#define _POSIX_C_SOURCE 200809L
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include "build.h"
#include "env.h"
#include "graph.h"
#include "util.h"

struct job {
	pid_t pid;
	struct string *cmd;
	int fd;
	struct edge *edge;
	struct buffer buf;
	bool failed;
};

static struct edge *work;
extern char **environ;

static bool
nodenewer(struct node *n1, struct node *n2)
{
	if (!n1)
		return false;
	if (n1->mtime.tv_sec > n2->mtime.tv_sec)
		return true;
	if (n1->mtime.tv_sec < n2->mtime.tv_sec)
		return false;
	return n1->mtime.tv_nsec > n2->mtime.tv_nsec;
}

/* calculate e->ready and n->dirty for n in e->out */
static void
computedirty(struct edge *e)
{
	struct node *n, *newest = NULL;
	size_t i;
	bool dirty;

	if (e->mark & MARK_STAT)
		return;
	e->mark |= MARK_STAT;
	dirty = false;
	for (i = 0; i < e->nout; ++i) {
		n = e->out[i];
		if (n->mtime.tv_nsec == MTIME_UNKNOWN)
			nodestat(n);
	}
	for (i = 0; i < e->nin; ++i) {
		n = e->in[i];

		/* record edge dependency on node */
		if (n->nuse && !n->use) {
			n->use = xmalloc(n->nuse * sizeof(*n->use));
			n->nuse = 0;
		}
		n->use[n->nuse++] = e;

		if (n->mtime.tv_nsec == MTIME_UNKNOWN) {
			nodestat(n);
			if (n->gen)
				computedirty(n->gen);
			else
				n->dirty = n->mtime.tv_nsec == MTIME_MISSING;
		}
		if (!dirty && i < e->inorderidx) {
			if (n->dirty)
				dirty = true;
			/* a node may be missing but not dirty if it a phony target */
			else if (n->mtime.tv_nsec != MTIME_MISSING && !nodenewer(newest, n))
				newest = n;
		}
	}
	/* all outputs are dirty if any are older than the newest input */
	for (i = 0; i < e->nout && !dirty; ++i) {
		n = e->out[i];
		if (e->rule == &phonyrule && e->nin > 0)
			continue;
		if (n->mtime.tv_nsec == MTIME_MISSING || (e->rule != &phonyrule && nodenewer(newest, n)))
			dirty = true;
	}
	for (i = 0; i < e->nout; ++i) {
		n = e->out[i];
		n->dirty = dirty;
	}
	if (dirty) {
		e->nblock = 0;
		for (i = 0; i < e->nin; ++i) {
			if (e->in[i]->dirty)
				++e->nblock;
		}
	}
}

static void
queue(struct edge *e)
{
	struct edge **front = &work;

	if (e->pool) {
		if (e->pool->numjobs == e->pool->maxjobs)
			front = &e->pool->work;
		else
			++e->pool->numjobs;
	}
	e->worknext = *front;
	*front = e;
}

static void
addsubtarget(struct node *n)
{
	struct edge *e;
	size_t i;

	// XXX: cycle detection
	if (!n->dirty)
		return;
	e = n->gen;
	if (!e)
		errx(1, "file is missing and not created by any action: '%s'", n->path->s);
	if (e->mark & MARK_WORK)
		return;
	e->mark |= MARK_WORK;
	if (e->nblock == 0)
		queue(e);
	for (i = 0; i < e->nin; ++i)
		addsubtarget(e->in[i]);
}

void
buildadd(struct node *n)
{
	if (n->gen) {
		computedirty(n->gen);
	} else {
		if (n->mtime.tv_nsec == MTIME_UNKNOWN)
			nodestat(n);
		n->dirty = n->mtime.tv_nsec == MTIME_MISSING;
	}
	addsubtarget(n);
}

static int
jobstart(struct job *j, struct edge *e)
{
	size_t i;
	struct node *n;
	struct string *rspfile, *content;
	int fd[2];
	posix_spawn_file_actions_t actions;
	char *argv[] = {"/bin/sh", "-c", NULL, NULL};

	for (i = 0; i < e->nout; ++i) {
		n = e->out[i];
		if (n->mtime.tv_nsec == MTIME_MISSING) {
			if (makedirs(n->path) < 0)
				goto err0;
		}
	}
	rspfile = edgevar(e, "rspfile");  // XXX: should use unescaped $out and $in
	if (rspfile) {
		content = edgevar(e, "rspfile_content");
		if (writefile(rspfile->s, content) < 0)
			goto err0;
	}

	if (pipe(fd) < 0) {
		warn("pipe");
		goto err1;
	}
	j->edge = e;
	j->cmd = edgevar(e, "command");
	if (!j->cmd) {
		warnx("rule '%s' has no command", e->rule->name);
		goto err2;
	}
	j->fd = fd[0];
	argv[2] = j->cmd->s;

	if (consolepool.numjobs == 0)
		puts(j->cmd->s);

	if ((errno = posix_spawn_file_actions_init(&actions))) {
		warn("posix_spawn_file_actions_init");
		goto err2;
	}
	if ((errno = posix_spawn_file_actions_addclose(&actions, fd[0]))) {
		warn("posix_spawn_file_actions_addclose");
		goto err3;
	}
	if (e->pool != &consolepool) {
		if ((errno = posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0))) {
			warn("posix_spawn_file_actions_addopen");
			goto err3;
		}
		if ((errno = posix_spawn_file_actions_adddup2(&actions, fd[1], 1))) {
			warn("posix_spawn_file_actions_adddup2");
			goto err3;
		}
		if ((errno = posix_spawn_file_actions_adddup2(&actions, fd[1], 2))) {
			warn("posix_spawn_file_actions_adddup2");
			goto err3;
		}
	}
	if ((errno = posix_spawn(&j->pid, argv[0], &actions, NULL, argv, environ))) {
		warn("posix_spawn %s", j->cmd->s);
		goto err3;
	}
	posix_spawn_file_actions_destroy(&actions);
	close(fd[1]);
	j->failed = false;

	return j->fd;

err3:
	posix_spawn_file_actions_destroy(&actions);
err2:
	close(fd[0]);
	close(fd[1]);
err1:
	if (rspfile)
		unlink(rspfile->s);
err0:
	return -1;
}

static void
nodedone(struct node *n)
{
	struct edge *e;
	size_t i;

	n->dirty = false;
	/* if we did not already populate n->use, we do not care about the dependent edges. */
	if (!n->use)
		return;
	for (i = 0; i < n->nuse; ++i) {
		e = n->use[i];
		if (e->nblock > 0 && --e->nblock == 0)
			queue(e);
	}
}

static void
edgedone(struct edge *e)
{
	struct edge *new;
	struct pool *p;
	size_t i;
	struct string *rspfile;

	if (e->pool) {
		p = e->pool;

		/* move edge from pool queue to main work queue */
		if (p->work) {
			new = p->work;
			p->work = p->work->worknext;
			new->worknext = work;
			work = new;
		} else {
			--p->numjobs;
		}
	}
	for (i = 0; i < e->nout; ++i)
		nodedone(e->out[i]);
	rspfile = edgevar(e, "rspfile");
	if (rspfile)
		unlink(rspfile->s);
}

static void
jobdone(struct job *j)
{
	int status;

	if (j->buf.len && consolepool.numjobs == 0)
		fwrite(j->buf.data, 1, j->buf.len, stdout);
	if (waitpid(j->pid, &status, 0) < 0) {
		warn("waitpid %d", j->pid);
		j->failed = true;
	} else if (WIFEXITED(status)) {
		if (WEXITSTATUS(status) != 0) {
			warnx("job failed: %s", j->cmd->s);
			j->failed = true;
		}
	} else if (WIFSIGNALED(status)) {
		warnx("job terminated due to signal %d: %s", WTERMSIG(status), j->cmd->s);
		j->failed = true;
	} else {
		/* cannot happen according to POSIX */
		warnx("job status unknown: %s", j->cmd->s);
		j->failed = true;
	}
	close(j->fd);
	j->buf.len = 0;
	if (!j->failed)
		edgedone(j->edge);
}

/* returns whether a job still has work to do. if not, sets j->failed */
static bool
jobwork(struct job *j)
{
	char *newdata;
	size_t newcap;
	ssize_t n;

	if (j->buf.cap - j->buf.len < BUFSIZ / 2) {
		newcap = j->buf.cap + BUFSIZ;
		newdata = realloc(j->buf.data, newcap);
		if (!newdata) {
			warn("realloc");
			goto kill;
		}
		j->buf.cap = newcap;
		j->buf.data = newdata;
	}
	n = read(j->fd, j->buf.data + j->buf.len, j->buf.cap - j->buf.len);
	if (n > 0) {
		j->buf.len += n;
		return true;
	}
	if (n == 0)
		goto done;
	warn("read");

kill:
	kill(j->pid, SIGTERM);
	j->failed = true;
done:
	jobdone(j);

	return false;
}

void
build(int maxjobs, int maxfail)
{
	struct job jobs[maxjobs];
	struct pollfd fds[maxjobs];
	int avail[maxjobs], i, next = 0, numjobs = 0, numfail = 0;
	struct edge *e;

	for (i = 0; i < maxjobs; ++i) {
		jobs[i].buf.data = NULL;
		jobs[i].buf.len = 0;
		jobs[i].buf.cap = 0;
		fds[i].fd = -1;
		fds[i].events = POLLIN;
		avail[i] = i + 1;
	}
	avail[maxjobs - 1] = -1;

	if (!work)
		puts("nothing to do");

	while (work || numjobs > 0) {
		/* start ready edges */
		while (work && numjobs < maxjobs && (!maxfail || numfail < maxfail)) {
			e = work;
			work = work->worknext;
			if (e->rule == &phonyrule) {
				edgedone(e);
				continue;
			}
			fds[next].fd = jobstart(&jobs[next], e);
			if (fds[next].fd < 0) {
				warnx("job failed to start");
				++numfail;
			} else {
				next = avail[next];
				++numjobs;
			}
		}
		if (numjobs == 0)
			break;

		/* wait for job to finish */
		do {
			if (poll(fds, maxjobs, -1) < 0)
				err(1, "poll");
			for (i = 0; i < maxjobs; ++i) {
				if (!fds[i].revents || jobwork(&jobs[i]))
					continue;
				--numjobs;
				avail[i] = next;
				fds[i].fd = -1;
				next = i;
				if (jobs[i].failed)
					++numfail;
			}
		} while (numjobs == maxjobs);
	}
	for (i = 0; i < maxjobs; ++i)
		free(jobs[i].buf.data);
	if (numfail > 0) {
		if (numfail < maxfail)
			errx(1, "cannot make progress due to previous errors");
		else if (numfail > 1)
			errx(1, "subcommands failed");
		else
			errx(1, "subcommand failed");
	}
}
