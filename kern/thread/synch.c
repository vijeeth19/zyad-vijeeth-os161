/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Synchronization primitives.
 * The specifications of the functions are in synch.h.
 */

#include <types.h>
#include <lib.h>
#include <spinlock.h>
#include <wchan.h>
#include <thread.h>
#include <current.h>
#include <synch.h>

////////////////////////////////////////////////////////////
//
// Semaphore.

struct semaphore *
sem_create(const char *name, unsigned initial_count)
{
        struct semaphore *sem;

        sem = kmalloc(sizeof(struct semaphore));
        if (sem == NULL) {
                return NULL;
        }

        sem->sem_name = kstrdup(name);
        if (sem->sem_name == NULL) {
                kfree(sem);
                return NULL;
        }

	sem->sem_wchan = wchan_create(sem->sem_name);
	if (sem->sem_wchan == NULL) {
		kfree(sem->sem_name);
		kfree(sem);
		return NULL;
	}

	spinlock_init(&sem->sem_lock);
        sem->sem_count = initial_count;

        return sem;
}

void
sem_destroy(struct semaphore *sem)
{
        KASSERT(sem != NULL);

	/* wchan_cleanup will assert if anyone's waiting on it */
	spinlock_cleanup(&sem->sem_lock);
	wchan_destroy(sem->sem_wchan);
        kfree(sem->sem_name);
        kfree(sem);
}

void
P(struct semaphore *sem)
{
        KASSERT(sem != NULL);

        /*
         * May not block in an interrupt handler.
         *
         * For robustness, always check, even if we can actually
         * complete the P without blocking.
         */
        KASSERT(curthread->t_in_interrupt == false);

	/* Use the semaphore spinlock to protect the wchan as well. */
	spinlock_acquire(&sem->sem_lock);
        while (sem->sem_count == 0) {
		/*
		 *
		 * Note that we don't maintain strict FIFO ordering of
		 * threads going through the semaphore; that is, we
		 * might "get" it on the first try even if other
		 * threads are waiting. Apparently according to some
		 * textbooks semaphores must for some reason have
		 * strict ordering. Too bad. :-)
		 *
		 * Exercise: how would you implement strict FIFO
		 * ordering?
		 */
		wchan_sleep(sem->sem_wchan, &sem->sem_lock);
        }
        KASSERT(sem->sem_count > 0);
        sem->sem_count--;
	spinlock_release(&sem->sem_lock);
}

void
V(struct semaphore *sem)
{
        KASSERT(sem != NULL);

	spinlock_acquire(&sem->sem_lock);

        sem->sem_count++;
        KASSERT(sem->sem_count > 0);
	wchan_wakeone(sem->sem_wchan, &sem->sem_lock);

	spinlock_release(&sem->sem_lock);
}

////////////////////////////////////////////////////////////
//
// Lock.

/*
 * Constructor for 'struct lock'
 */
struct lock *
lock_create(const char *name)
{
        struct lock *lock;

        lock = kmalloc(sizeof(struct lock));
        if (lock == NULL) {
                return NULL;
        }

        lock->lk_name = kstrdup(name);
        if (lock->lk_name == NULL) {
                kfree(lock);
                return NULL;
        }

	lock->lk_wchan = wchan_create(lock->lk_name);
	if (lock->lk_wchan == NULL) {
		kfree(lock->lk_name);
		kfree(lock);
		return NULL;
	}

	spinlock_init(&lock->lk_lock);
        lock->locked = false;
        lock->locked_thread = NULL;

        return lock;
}

/*
 * Destructor for 'struct lock'
 */
void
lock_destroy(struct lock *lock)
{
        KASSERT(lock != NULL);

        spinlock_cleanup(&lock->lk_lock);
        wchan_destroy(lock->lk_wchan);
        kfree(lock->lk_name);
        kfree(lock);
}

/*
 * Get the lock.
 * 
 * Only one thread can hold the lock at the same time.
 * Uses a spinlock to make the operation atomic.
 * 
 * A thread trying to acquire the lock when the lock is already held will result in the thread being placed in a wait channel,
 * and will change state to S_SLEEP.
 * 
 * A thread will be released from the wait channel when lock_release() is called and the thread is at the head of the wait channel.
 */
void
lock_acquire(struct lock *lock)
{
	spinlock_acquire(&lock->lk_lock);
        /*
         * May not block in an interrupt handler.
         *
         * For robustness, always check, even if we can actually
         * complete lock_acquire() without blocking.
         */
        KASSERT(lock != NULL && curthread->t_in_interrupt == false);

        /* panic if deadlock occurs */
        if(lock->locked_thread == curthread){
                panic("Deadlock on lock %s in thread %d\n", lock->lk_name, (int) curthread);
        }
        while (lock->locked == true){
                /* Store current thread in wait channel, then release spinlock */
		wchan_sleep(lock->lk_wchan, &lock->lk_lock);
        }

        /* Lock must have been released at this point */
        KASSERT(lock->locked == false && lock->locked_thread == NULL);

        lock->locked_thread = curthread;
        lock->locked = true; 
	spinlock_release(&lock->lk_lock);

}

/*
 * Free the lock.
 * 
 * Only the thread holding the lock may do this.
 * Uses a spinlock to make the operation atomic.
 * Wakes the thread that is placed at the top of the wait channel in trying to acquire the lock. 
 */
void
lock_release(struct lock *lock)
{
        KASSERT(lock != NULL);


        spinlock_acquire(&lock->lk_lock);

        /* Ensure the thread releasing the lock also holds the lock */
        KASSERT(lock_do_i_hold(lock) == true);


        lock->locked = false;
        lock->locked_thread = NULL;

        /* make the thread at the top of the wait channel runnable */
        wchan_wakeone(lock->lk_wchan, &lock->lk_lock);
        spinlock_release(&lock->lk_lock);
}

/*
 * Returns true if the current thread holds the lock, false otherwise.
 */
bool
lock_do_i_hold(struct lock *lock)
{
	/* Assume we can read locked_thread atomically enough for this to work */
        return lock->locked_thread == curthread;
}

////////////////////////////////////////////////////////////
//
// CV


/*
 * Constructor for 'struct cv'
 */
struct cv *
cv_create(const char *name)
{
        struct cv *cv;

        cv = kmalloc(sizeof(struct cv));
        if (cv == NULL) {
                return NULL;
        }

        cv->cv_name = kstrdup(name);
        if (cv->cv_name==NULL) {
                kfree(cv);
                return NULL;
        }

        cv->cv_wchan = wchan_create(cv->cv_name);
        if(cv->cv_wchan == NULL){
                kfree(cv->cv_name);
                kfree(cv);
                return NULL;
        }

        spinlock_init(&cv->cv_lk);
        return cv;
}

/*
 * Destructor for 'struct cv'
 */
void
cv_destroy(struct cv *cv)
{
        KASSERT(cv != NULL);

        spinlock_cleanup(&cv->cv_lk);
        wchan_destroy(cv->cv_wchan);
        kfree(cv->cv_name);
        kfree(cv);
}

/*
 * Waits on a signal or broadcast of this CV.
 * 
 * Uses a spinlock to make the operation atomic.
 * Releases the supplied lock, goes to sleep, then acquires the lock once signaled. 
 */
void
cv_wait(struct cv *cv, struct lock *lock)
{
        spinlock_acquire(&cv->cv_lk);

        /* release lock before blocking the thread, this also checks if this current thread holds the lock implicitly */
        lock_release(lock);

        /* sleep until awaken by cv_signal() or cv_broadcast() */
        wchan_sleep(cv->cv_wchan, &cv->cv_lk);
        spinlock_release(&cv->cv_lk);
        lock_acquire(lock);
}

/*
 * Wakes up one thread that is sleeping on this CV.
 * 
 * Uses a spinlock to make the operation atomic.
 */
void
cv_signal(struct cv *cv, struct lock *lock)
{
        spinlock_acquire(&cv->cv_lk);
        KASSERT(lock_do_i_hold(lock) == true);

        /* make the thread at the head of the wait channel runnable */
        wchan_wakeone(cv->cv_wchan, &cv->cv_lk);
        spinlock_release(&cv->cv_lk);
}

/*
 * Wakes up all threads sleeping on this CV.
 * 
 * Uses a spinlock to make the operation atomic.
 */
void
cv_broadcast(struct cv *cv, struct lock *lock)
{
        spinlock_acquire(&cv->cv_lk);
        KASSERT(lock_do_i_hold(lock) == true);

        /* make all threads in the wait channel runnable */
        wchan_wakeall(cv->cv_wchan, &cv->cv_lk);
        spinlock_release(&cv->cv_lk);
}
