/* timers.c - simple timer routines
**
** Copyright (C) 1995-2015  Jef Poskanzer <jef@mail.acme.com>
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
** AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
** IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
** ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNERS OR CONTRIBUTORS BE
** LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
** CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
** SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
** INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
** CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
** ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
** THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <config.h>

#include <stdlib.h>
#include <stdio.h>
#include <syslog.h>
#include <sys/types.h>

#include "timers.h"


#define HASH_SIZE 67
static struct timer *timers[HASH_SIZE];
static struct timer *free_timers;
static int alloc_count, active_count, free_count;

arg_t noarg;

#undef HAVE_CLOCK_MONO
#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
#define HAVE_CLOCK_MONO
static int use_monotonic = 0;	/* monotonic clock runtime availability flag */
static struct timeval tv_diff;	/* system time - monotonic difference at start */
#endif

static unsigned int hash(struct timer *t)
{
	/* We can hash on the trigger time, even though it can change over
	** the life of a timer via either the periodic bit or the tmr_reset()
	** call.  This is because both of those guys call l_resort(), which
	** recomputes the hash and moves the timer to the appropriate list.
	*/
	return ((unsigned int)t->time.tv_sec ^ (unsigned int)t->time.tv_usec) % HASH_SIZE;
}


static void l_add(struct timer *t)
{
	int h = t->hash;
	struct timer *t2;
	struct timer *t2prev;

	t2 = timers[h];
	if (!t2) {
		/* The list is empty. */
		timers[h] = t;
		t->prev = t->next = NULL;
	} else {
		if ( t->time.tv_sec   < t2->time.tv_sec ||
		    (t->time.tv_sec  == t2->time.tv_sec &&
		     t->time.tv_usec <= t2->time.tv_usec)) {
			/* The new timer goes at the head of the list. */
			timers[h] = t;
			t->prev   = NULL;
			t->next   = t2;
			t2->prev  = t;
		} else {
			/* Walk the list to find the insertion point. */
			for (t2prev = t2, t2 = t2->next; t2; t2prev = t2, t2 = t2->next) {
				if (t->time.tv_sec < t2->time.tv_sec ||
				    (t->time.tv_sec  == t2->time.tv_sec &&
				     t->time.tv_usec <= t2->time.tv_usec)) {
					/* Found it. */
					t2prev->next = t;
					t->prev      = t2prev;
					t->next      = t2;
					t2->prev     = t;
					return;
				}
			}

			/* Oops, got to the end of the list.  Add to tail. */
			t2prev->next = t;
			t->prev     = t2prev;
			t->next     = NULL;
		}
	}
}


static void l_remove(struct timer *t)
{
	if (!t)
		return;

	if (!t->prev)
		timers[t->hash] = t->next;
	else
		t->prev->next = t->next;

	if (t->next)
		t->next->prev = t->prev;
}


static void l_resort(struct timer *t)
{
	/* Remove the timer from its old list. */
	l_remove(t);

	/* Recompute the hash. */
	t->hash = hash(t);

	/* And add it back in to its new list, sorted correctly. */
	l_add(t);
}


void tmr_init(void)
{
	int h;

	for (h = 0; h < HASH_SIZE; ++h)
		timers[h] = NULL;
	free_timers = NULL;
	alloc_count = active_count = free_count = 0;

	/* Check for monotonic clock availability */
#ifdef HAVE_CLOCK_MONO
	struct timespec ts;
	struct timeval tv_start, tv;

	/* Try to get monotonic clock time */
	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
		use_monotonic = 1;

		/* Get current system time */
		gettimeofday(&tv_start, NULL);

		tv.tv_sec  = ts.tv_sec;
		tv.tv_usec = ts.tv_nsec / 1000L;
		/* Calculate and save the difference: tv_start is since
		** the Epoch, so tv_start > ts tv_diff = tv_start - tv
		*/
		timersub(&tv_start, &tv, &tv_diff);
	}
#endif
}


struct timer *tmr_create(struct timeval *now,
			 void (*cb)(arg_t, struct timeval *),
			 arg_t arg, long msecs, int periodic)
{
	struct timer *t;

	if (free_timers) {
		t           = free_timers;
		free_timers = t->next;
		--free_count;
	} else {
		t = malloc(sizeof(struct timer));
		if (!t)
			return NULL;

		++alloc_count;
	}

	t->cb       = cb;
	t->arg      = arg;
	t->msecs    = msecs;
	t->periodic = periodic;
	if (now)
		t->time = *now;
	else
		tmr_prepare_timeval(&t->time);

	t->time.tv_sec  +=  msecs / 1000L;
	t->time.tv_usec += (msecs % 1000L) * 1000L;
	if (t->time.tv_usec >= 1000000L) {
		t->time.tv_sec += t->time.tv_usec / 1000000L;
		t->time.tv_usec %= 1000000L;
	}

	t->hash = hash(t);

	/* Add the new timer to the proper active list. */
	l_add(t);
	++active_count;

	return t;
}


struct timeval *tmr_timeout(struct timeval *now)
{
	long msecs;
	static struct timeval timeout;

	msecs = tmr_mstimeout(now);
	if (msecs == INFTIM)
		return NULL;

	timeout.tv_sec  =  msecs / 1000L;
	timeout.tv_usec = (msecs % 1000L) * 1000L;

	return &timeout;
}


long tmr_mstimeout(struct timeval *now)
{
	long msecs, m;
	int gotone;
	int h;

	gotone = 0;
	msecs  = 0;

	/* Since the lists are sorted, we only need to look at
	** the first timer on each one.
	*/
	for (h = 0; h < HASH_SIZE; ++h) {
		struct timer *t;

		t = timers[h];
		if (!t)
			continue;

		m = (t->time.tv_sec - now->tv_sec) * 1000L + (t->time.tv_usec - now->tv_usec) / 1000L;
		if (!gotone) {
			msecs = m;
			gotone = 1;
		} else if (m < msecs) {
			msecs = m;
		}
	}

	if (!gotone)
		return INFTIM;

	if (msecs <= 0)
		msecs = 500; /* Was 0, but we should never poll() < 500 msec */

	return msecs;
}


void tmr_run(struct timeval *now)
{
	int h;
	struct timer *t;
	struct timer *next;

	for (h = 0; h < HASH_SIZE; ++h)
		for (t = timers[h]; t; t = next) {
			next = t->next;
			/* Since the lists are sorted, as soon as we find a timer
			** that isn't ready yet, we can go on to the next list.
			*/
			if (t->time.tv_sec > now->tv_sec || (t->time.tv_sec == now->tv_sec &&
							      t->time.tv_usec > now->tv_usec))
				break;

			(t->cb) (t->arg, now);
			if (t->periodic) {
				/* Reschedule. */
				t->time.tv_sec  +=  t->msecs / 1000L;
				t->time.tv_usec += (t->msecs % 1000L) * 1000L;
				if (t->time.tv_usec >= 1000000L) {
					t->time.tv_sec += t->time.tv_usec / 1000000L;
					t->time.tv_usec %= 1000000L;
				}
				l_resort(t);
			} else
				tmr_cancel(t);
		}
}


void tmr_reset(struct timeval *now, struct timer *t)
{
	if (!t)
		return;

	t->time = *now;
	t->time.tv_sec  +=  t->msecs / 1000L;
	t->time.tv_usec += (t->msecs % 1000L) * 1000L;
	if (t->time.tv_usec >= 1000000L) {
		t->time.tv_sec += t->time.tv_usec / 1000000L;
		t->time.tv_usec %= 1000000L;
	}
	l_resort(t);
}


void tmr_cancel(struct timer *t)
{
	if (!t)
		return;

	/* Remove it from its active list. */
	l_remove(t);
	--active_count;

	/* And put it on the free list. */
	t->prev     = NULL;
	t->next     = free_timers;
	free_timers = t;

	++free_count;
}


void tmr_cleanup(void)
{
	struct timer *t;

	while (free_timers) {
		t           = free_timers;
		free_timers = t->next;

		--free_count;
		--alloc_count;

		free(t);
	}
}


void tmr_destroy(void)
{
	int h;

	for (h = 0; h < HASH_SIZE; ++h) {
		while (timers[h])
			tmr_cancel(timers[h]);
	}
	tmr_cleanup();
}


/* Generate debugging statistics syslog message. */
void tmr_logstats(long secs)
{
	syslog(LOG_INFO, "  timers - %d allocated, %d active, %d free", alloc_count, active_count, free_count);
	if (active_count + free_count != alloc_count)
		syslog(LOG_ERR, "timer counts don't add up!");
}

/* Fill timeval structure for further usage by the package. */
void tmr_prepare_timeval(struct timeval *tv)
{
#ifdef HAVE_CLOCK_MONO
	struct timespec ts;
	struct timeval tv0;

	if (use_monotonic) {	/* use monotonic clock source ? */
		if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
			perror("clock_gettime");
			return;
		}
		tv0.tv_sec = ts.tv_sec;
		tv0.tv_usec = ts.tv_nsec / 1000L;
		/* Return system time value like it was running accurately */
		timeradd(&tv_diff, &tv0, tv);
	} else {
#endif
		gettimeofday(tv, NULL);
#ifdef HAVE_CLOCK_MONO
	}
#endif
}
