/*-
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Based on "Spinlocks and Read-Write Locks" by Dr. Steven Fuerst:
 *	http://locklessinc.com/articles/locks/
 *
 * Dr. Fuerst further credits:
 *	There exists a form of the ticket lock that is designed for read-write
 * locks. An example written in assembly was posted to the Linux kernel mailing
 * list in 2002 by David Howells from RedHat. This was a highly optimized
 * version of a read-write ticket lock developed at IBM in the early 90's by
 * Joseph Seigh. Note that a similar (but not identical) algorithm was published
 * by John Mellor-Crummey and Michael Scott in their landmark paper "Scalable
 * Reader-Writer Synchronization for Shared-Memory Multiprocessors".
 */

#include "wt_internal.h"

/*
 * __wt_rwlock_alloc --
 *	Allocate and initialize a read/write lock.
 */
int
__wt_rwlock_alloc(
    WT_SESSION_IMPL *session, WT_RWLOCK **rwlockp, const char *name)
{
	WT_RWLOCK *rwlock;

	WT_RET(__wt_verbose(session, WT_VERB_MUTEX, "rwlock: alloc %s", name));

	WT_RET(__wt_calloc(session, 1, sizeof(WT_RWLOCK), &rwlock));

	rwlock->name = name;

	*rwlockp = rwlock;
	return (0);
}

/*
 * __wt_try_readlock --
 *	Try to get a read lock, fail immediately if unavailable.
 */
int
__wt_try_readlock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	wt_rwlock_t *l;
	uint32_t cmp, cmpnew, me, writers;
	uint8_t menew;

	WT_RET(__wt_verbose(
	    session, WT_VERB_MUTEX, "rwlock: try_readlock %s", rwlock->name));
	WT_STAT_FAST_CONN_INCR(session, rwlock_read);

	l = &rwlock->rwlock;
	me = l->s.users;
	menew = (uint8_t)(me + 1);
	writers = l->s.writers;
	cmp = (me << 16) + (me << 8) + writers;
	cmpnew = ((uint32_t)menew << 16) + (menew << 8) + writers;
	return (WT_ATOMIC_CAS_VAL4(l->u, cmp, cmpnew) == cmp ? 0 : EBUSY);
}

/*
 * __wt_readlock --
 *	Read lock.
 */
int
__wt_readlock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	wt_rwlock_t *l;
	uint32_t me;
	uint8_t val;

	WT_RET(__wt_verbose(
	    session, WT_VERB_MUTEX, "rwlock: readlock %s", rwlock->name));
	WT_STAT_FAST_CONN_INCR(session, rwlock_read);

	l = &rwlock->rwlock;
	me = WT_ATOMIC_FETCH_ADD4(l->u, 1 << 16);
	val = (uint8_t)(me >> 16);
	while (val != l->s.readers)
		WT_PAUSE();

	++l->s.readers;

	return (0);
}

/*
 * __readunlock --
 *	Release a read lock.
 */
static int
__readunlock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	wt_rwlock_t *l;

	WT_RET(__wt_verbose(
	    session, WT_VERB_MUTEX, "rwlock: read unlock %s", rwlock->name));

	l = &rwlock->rwlock;
	WT_ATOMIC_ADD1(l->s.writers, 1);

	return (0);
}

/*
 * __wt_try_writelock --
 *	Try to get an exclusive lock, fail immediately if unavailable.
 */
int
__wt_try_writelock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	wt_rwlock_t *l;
	uint32_t cmp, cmpnew, me, readers;
	uint8_t menew;

	WT_RET(__wt_verbose(
	    session, WT_VERB_MUTEX, "rwlock: try_writelock %s", rwlock->name));
	WT_STAT_FAST_CONN_INCR(session, rwlock_write);

	l = &rwlock->rwlock;
	me = l->s.users;
	menew = (uint8_t)(me + 1);
	readers = l->s.readers << 8;
	cmp = (me << 16) + readers + me;
	cmpnew = (menew << 16) + readers + me;
	if (WT_ATOMIC_CAS_VAL4(l->u, cmp, cmpnew) != cmp)
		return (EBUSY);

	rwlock->exclusive_locked = 1;
	return (0);
}

/*
 * __wt_writelock --
 *	Wait to get an exclusive lock.
 */
int
__wt_writelock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	wt_rwlock_t *l;
	uint32_t me;
	uint8_t val;

	WT_RET(__wt_verbose(
	    session, WT_VERB_MUTEX, "rwlock: writelock %s", rwlock->name));
	WT_STAT_FAST_CONN_INCR(session, rwlock_write);

	l = &rwlock->rwlock;
	me = WT_ATOMIC_FETCH_ADD4(l->u, 1 << 16);
	val = (uint8_t)(me >> 16);
	while (val != l->s.writers)
		WT_PAUSE();

	rwlock->exclusive_locked = 1;
	return (0);
}

/*
 * __writeunlock --
 *	Release a write lock.
 */
static int
__writeunlock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	wt_rwlock_t *l, copy;

	WT_RET(__wt_verbose(
	    session, WT_VERB_MUTEX, "rwlock: writeunlock %s", rwlock->name));

	l = &rwlock->rwlock;

	copy = *l;
	rwlock->exclusive_locked = 0;

	/*
	 * Use a full barrier, not just a memory barrier because the exclusive-
	 * locked flag has to be cleared before a subsequent writer gets the
	 * lock and sets it.
	 */
	WT_FULL_BARRIER();

	++copy.s.writers;
	++copy.s.readers;

	l->us = copy.us;
	return (0);
}

/*
 * __wt_rwunlock --
 *	Release a read/write lock.
 */
int
__wt_rwunlock(WT_SESSION_IMPL *session, WT_RWLOCK *rwlock)
{
	if (rwlock->exclusive_locked == 0)
		return (__readunlock(session, rwlock));
	else
		return (__writeunlock(session, rwlock));
}

/*
 * __wt_rwlock_destroy --
 *	Destroy a mutex.
 */
int
__wt_rwlock_destroy(WT_SESSION_IMPL *session, WT_RWLOCK **rwlockp)
{
	WT_RWLOCK *rwlock;

	rwlock = *rwlockp;		/* Clear our caller's reference. */
	if (rwlock == NULL)
		return (0);
	*rwlockp = NULL;

	WT_RET(__wt_verbose(
	    session, WT_VERB_MUTEX, "rwlock: destroy %s", rwlock->name));

	__wt_free(session, rwlock);
	return (0);
}
