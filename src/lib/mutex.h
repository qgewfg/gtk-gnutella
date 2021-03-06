/*
 * Copyright (c) 2011, Raphael Manfredi
 *
 *----------------------------------------------------------------------
 * This file is part of gtk-gnutella.
 *
 *  gtk-gnutella is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  gtk-gnutella is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with gtk-gnutella; if not, write to the Free Software
 *  Foundation, Inc.:
 *      59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *----------------------------------------------------------------------
 */

/**
 * @ingroup lib
 * @file
 *
 * Mutual thread exclusion locks.
 *
 * @author Raphael Manfredi
 * @date 2011
 */

#ifndef _mutex_h_
#define _mutex_h_

#include "spinlock.h"
#include "thread.h"

enum mutex_magic {
	MUTEX_MAGIC = 0x1a35dfeb,
	MUTEX_DESTROYED = 0x24fd2039
};

/**
 * A mutex is just a memory location holding an integer value (the lock)
 * a thread owner and an ownership depth.
 *
 * When the integer is 0, the lock is available, when the integer is 1
 * the lock is busy.
 */
typedef struct mutex {
	enum mutex_magic magic;
	unsigned long owner;
	size_t depth;
	spinlock_t lock;
} mutex_t;

/**
 * Static initialization value for a mutex structure.
 */
#define MUTEX_INIT	{ MUTEX_MAGIC, 0L, 0L, SPINLOCK_INIT }

/*
 * These should not be called directly by user code to allow debugging.
 */

void mutex_grab(mutex_t *m, bool hidden);
bool mutex_grab_try(mutex_t *m, bool hidden);
void mutex_ungrab(mutex_t *m, bool hidden);

/*
 * Public interface.
 */

#ifdef SPINLOCK_DEBUG
void mutex_grab_from(mutex_t *m, bool hidden, const char *file, unsigned line);
bool mutex_grab_try_from(mutex_t *m, bool hidden, const char *f, unsigned l);

#define mutex_lock(x)			mutex_grab_from((x), FALSE, _WHERE_, __LINE__)
#define mutex_lock_hidden(x)	mutex_grab_from((x), TRUE, _WHERE_, __LINE__)
#define mutex_trylock(x)		mutex_grab_try_from((x), FALSE, \
									_WHERE_, __LINE__)
#define mutex_trylock_hidden(x)	mutex_grab_try_from((x), TRUE, \
									_WHERE_, __LINE__)

#define mutex_lock_const(x)	\
	mutex_grab_from(deconstify_pointer(x), FALSE, _WHERE_, __LINE__)

#else
#define mutex_lock(x)			mutex_grab((x), FALSE)
#define mutex_lock_hidden(x)	mutex_grab((x), TRUE)
#define mutex_trylock(x)		mutex_grab_try((x), FALSE)
#define mutex_trylock_hidden(x)	mutex_grab_try((x), TRUE)
#define mutex_lock_const(x)		mutex_grab(deconstify_pointer(x), FALSE)
#endif	/* SPINLOCK_DEBUG */

#define mutex_unlock(x)			mutex_ungrab((x), FALSE)
#define mutex_unlock_hidden(x)	mutex_ungrab((x), TRUE)

void mutex_crash_mode(void);

void mutex_init(mutex_t *m);
void mutex_destroy(mutex_t *m);
void mutex_unlock_const(const mutex_t *m);
bool mutex_is_owned(const mutex_t *m);
bool mutex_is_owned_by(const mutex_t *m, const thread_t t);
size_t mutex_held_depth(const mutex_t *m);

#endif /* _mutex_h_ */

/* vi: set ts=4 sw=4 cindent: */
