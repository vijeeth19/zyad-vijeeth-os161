/*
 * Driver code for airballoon problem
 */
#include <types.h>
#include <lib.h>
#include <synch.h>
#include <test.h>
#include <thread.h>
#include <current.h>

#define N_LORD_FLOWERKILLER 10
#define NROPES 16

/* global variable for tracking how many ropes are not severed */
static int ropes_left = NROPES;

/* Data structures for rope mappings */

/* 'y = stake_rope[i]' maps stake number i to rope y */
/* This is the data structure that allows Marigold/FlowerKiller to access the ropes */
int stake_rope[NROPES];

/* 'y = rope_severed[i]' maps a rope i to true if its severed, false otherwise */
/* This is 'the' rope data structure */
bool rope_severed[NROPES];

/* 'y = hook_rope[i]' maps a hook i to rope y */
/* This is the data structure that allows Dandelion to access the ropes */
int hook_rope[NROPES];

/* 
 * This counts how many threads are done before the balloon thread is allowed to finish. 
 * If (2 + N_LORD_FLOWERKILLER) threads are done then the balloon thread will finish and 
 * signal the main thread to complete.
 */
static int num_done = 0;

/* This counts how many threads have started.
   Important for synchronizing the balloon thread to start before the other threads have finished. */
static int num_start = 0;

/* Synchronization primitives */

/* 'y = rope_locks[i], i < NROPES' maps rope i to its lock y */
/* a rope_lock locks a rope such that no other thread can modify the rope */
struct lock* rope_locks[NROPES];

/* 'y = stake_locks[i]' maps stake i to its lock y */
/* a stake_lock locks a stake such that no other thread can modify the stake */
struct lock* stake_locks[NROPES];

/* condition variable for synchronizing the start of the marigold/dandelion/flowerkiller/balloon threads */
struct cv* cv_start;

/* lock for the condition variable 'cv_start' 
 *
 * Additionally, this lock is used to ensure that the main thread hits the cv_wait() statement (signaled by balloon thread)
 * before the other threads complete.
 */
struct lock* start_lock;

/* condition variable for balloon thread, synchronizes the end of the marigold/dandelion/flowerkiller threads */
struct cv* cv_done;
/* lock for condition variable 'cv_done' */
struct lock* end_lock;

/* condition variable for main thread, needs to wait for balloon thread to be done */
struct cv* main_done;
/* lock for the condition variable 'main_done' */
struct lock* main_lock;

/* thread identifiers for the thread_start_synchronize() and thread_end_synchronize() functions */
enum thread_id {
    DANDELION,
    MARIGOLD,
    LORDFLOWERKILLER,
    BALLOON
};


/* 
 *
 * Description block:
 * 
 * Only one thread can modify a rope at the same time (done through rope_locks)
 * Only one thread can modify a stake at the same time (done through stake_locks)
 * The flowerkiller/marigold/dandelion/balloon threads will start together to synchronize the balloon thread. 
 * The balloon thread must reach the cv_wait(cv_done, end_lock) statement BEFORE other threads signal the cv_done condition variable.
 * The main thread must reach the cv_wait(main_done, main_lock) statement BEFORE the balloon thread signals the main_done condition variable.
 * 
 * Dandelion, Marigold, and FlowerKiller threads know they are done when ropes_left == 0.
 * Balloon thread knows it is done when signalled by one of the above threads.
 * Main thread knows it is done when signalled by the balloon thread.
 */


/* 
 * Function for synchronizing the start of marigold/dandelion/flowerkiller threads.	
 *
 * This is done so that the balloon thread is guaranteed to reach the cv_wait(cv_done, end_lock) statement BEFORE
 * the marigold/dandelion/flowerkiller threads have completed and called cv_signal(cv_done, end_lock). 
 */
static
inline
void
thread_start_synchronize(enum thread_id id)
{
    lock_acquire(start_lock);

    /* Figure out which thread called the function */
    switch (id) {
    case DANDELION:
        kprintf("Dandelion thread starting\n");
        break;
    case MARIGOLD:
        kprintf("Marigold thread starting\n");
        break;
    case LORDFLOWERKILLER:
        kprintf("Lord FlowerKiller thread starting\n");
        break;
    case BALLOON:
        kprintf("Balloon thread starting\n");
        /*
		* The balloon thread must acquire the end_lock before the other threads have
		* so that it can reach the cv_wait(cv_done, end_lock) statement.
		*/
        lock_acquire(end_lock);
        break;
    }

    num_start++;
    while (num_start != 3 + N_LORD_FLOWERKILLER) {
        /*
		 * If the number of started threads is not equal to:	
		 *
		 * =[ dandelion + balloon + marigold + N_LORD_FLOWERKILLER ] threads
		 * =[ 3 + N_LORD_FLOWERKILLER ] threads 
		 * 
		 * then wait on the condition variable cv_start which will be broadcasted
		 * when all threads have started. 
		 */
        cv_wait(cv_start, start_lock);
    }

    /* broadcast to all sleeping threads that enough threads have started */
    cv_broadcast(cv_start, start_lock);
    lock_release(start_lock);
}

/*  
 * Code block for synchronizing the end of marigold/dandelion/flowerkiller threads.
 * 
 * This is done so we can ensure that the marigold/dandelion/flowerkiller threads have finished
 * BEFORE signalling the balloon thread to finish. 
 */
static
inline
void
thread_end_synchronize(enum thread_id id)
{
    lock_acquire(end_lock);

    /* Figure out which thread called the function */
    switch (id) {
    case DANDELION:
        kprintf("Dandelion thread done\n");
        break;
    case MARIGOLD:
        kprintf("Marigold thread done\n");
        break;
    case LORDFLOWERKILLER:
        kprintf("Lord FlowerKiller thread done\n");
        break;
    case BALLOON:
        /* this function will not be ran with thread_id == BALLOON */
        panic("balloon thread should never call this function");
        break;
    }

    num_done++;
    if (num_done == 2 + N_LORD_FLOWERKILLER) {
        /* signal the balloon thread when all the marigold/dandelion/flowerkiller threads
           have completed */
        cv_signal(cv_done, end_lock);
    }
    lock_release(end_lock);
}

/*
 * This thread aims to sever ropes (through hooks) that are not already severed. 
 * Dandelion can only access the rope_severed[] array through hook_rope[].
 */
static
void
dandelion(void* p, unsigned long arg)
{
    (void)p;
    (void)arg;

    /* synchronize the start of each thread */
    thread_start_synchronize(DANDELION);

    /* 
	 * While there are ropes yet to be severed, obtain random hooks within [0,NROPES-1] and sever
	 * the ropes they connect to (if not already severed)
	 */
    while (ropes_left) {
        int hook_index = random() % NROPES;
        /* obtain the rope that connects to the random hook */
        int rope_index = hook_rope[hook_index];

		/* sever the rope atomically (if not severed) */
        lock_acquire(rope_locks[rope_index]);
        if (!rope_severed[rope_index]) {
            kprintf("Dandelion severed rope %d\n", rope_index);
            rope_severed[rope_index] = true;
            ropes_left--;
        }
        lock_release(rope_locks[rope_index]);

        thread_yield();
    }

    /* synchronize the end of each thread until they all finish then signal balloon */
    thread_end_synchronize(DANDELION);
    thread_exit();
}

/*
 * This thread aims to sever ropes (through stakes) that are not already severed by Dandelion.
 * Marigold can only access the rope_severed[] array through stake_rope[].
 */
static
void
marigold(void* p, unsigned long arg)
{
    (void)p;
    (void)arg;

    /* synchronize the start of each thread */
    thread_start_synchronize(MARIGOLD);

    /* 
	 * While there are ropes yet to be severed, obtain random stakes within [0,NROPES-1] and sever
	 * the ropes they connect to (if not already severed)
	 */
    while (ropes_left) {
        int stake_index = random() % NROPES;
        /* lock stake to avoid race condition with flowerkiller thread */
        lock_acquire(stake_locks[stake_index]);

        /* obtain rope that connects to the random stake */
        int rope_index = stake_rope[stake_index];


		/* sever the rope atomically (if not severed) */
        lock_acquire(rope_locks[rope_index]);
        if (!rope_severed[rope_index]) {
            kprintf("Marigold severed rope %d from stake %d\n", rope_index, stake_index);
            rope_severed[rope_index] = true;
            ropes_left--;
        }
        lock_release(rope_locks[rope_index]);
        lock_release(stake_locks[stake_index]);

        thread_yield();
    }

    /* synchronize the end of each thread until they all finish then signal balloon */
    thread_end_synchronize(MARIGOLD);
    thread_exit();
}

/* 
 * This thread aims to switch the ropes that connect to 2 random stakes. 
 */
static
void
flowerkiller(void* p, unsigned long arg)
{
    (void)p;
    (void)arg;

    /* synchronize the start of each thread */
    thread_start_synchronize(LORDFLOWERKILLER);

	/* 
	 * While there are at least two ropes not severed, obtain random stakes within [0,NROPES-1] and switch
	 * the ropes they connect to (if the ropes have not been severed).
	 */
    while (ropes_left >= 2) {
        /* Generate random pair of stake numbers within [0,NROPES-1] */
        int stake1 = random() % NROPES;
        int stake2 = random() % NROPES;

        /* Make sure not to generate the same stake numbers to avoid deadlock */
        while (stake2 == stake1) {
            stake2 = random() % NROPES;
        }

        /* Maintain an order of stakes locked to avoid deadlock with another flowerkiller thread */
        if (stake1 > stake2) {
            lock_acquire(stake_locks[stake1]);
            lock_acquire(stake_locks[stake2]);
        } else {
            lock_acquire(stake_locks[stake2]);
            lock_acquire(stake_locks[stake1]);
        }

        /* Obtain the ropes connected to the random stakes */
        int ropeind1 = stake_rope[stake1];
        int ropeind2 = stake_rope[stake2];

		/* switch the ropes atomically (if not severed) */
        lock_acquire(rope_locks[ropeind1]);
        lock_acquire(rope_locks[ropeind2]);
        if (!rope_severed[ropeind1] && !rope_severed[ropeind2]) {
            kprintf("Lord FlowerKiller switched rope %d from stake %d to stake %d\n", ropeind1, stake1, stake2);
            kprintf("Lord FlowerKiller switched rope %d from stake %d to stake %d\n", ropeind2, stake2, stake1);
            stake_rope[stake1] = ropeind2;
            stake_rope[stake2] = ropeind1;
        }
        lock_release(rope_locks[ropeind2]);
        lock_release(rope_locks[ropeind1]);
        lock_release(stake_locks[stake2]);
        lock_release(stake_locks[stake1]);

        thread_yield();
    }

    /* synchronize the end of each thread until they all finish then signal balloon */
	while(ropes_left){
		thread_yield();
	}
    thread_end_synchronize(LORDFLOWERKILLER);
    thread_exit();
}

static
void
balloon(void* p, unsigned long arg)
{
    (void)p;
    (void)arg;

    /* Synchronize the start of each thread */
    thread_start_synchronize(BALLOON);

    /* Wait for all the marigold/dandelion/flowerkiller threads to finish, then the balloon thread can finish */
    while (num_done != 2 + N_LORD_FLOWERKILLER) {
        cv_wait(cv_done, end_lock);
    }
    lock_release(end_lock);

	/* All other threads (except main) have finished by this point */
    lock_acquire(main_lock);
    /* Signal the main thread that all threads have finished */
    cv_signal(main_done, main_lock);
    kprintf("Balloon freed and Prince Dandelion escapes!\n");
    kprintf("Balloon thread done\n");
    lock_release(main_lock);
    thread_exit();
}

// Change this function as necessary
int
airballoon(int nargs, char** args)
{

    (void)nargs;
    (void)args;
    int i, err = 0;

    /* Initialize synchronization primitives and rope mapping data structures */
    for (int i = 0; i < NROPES; i++) {
        /* Initalize rope to stake/hook correspondance to be 1:1 */
        stake_rope[i] = i;
        hook_rope[i] = i;
        rope_severed[i] = false;
        rope_locks[i] = lock_create("rope lock");
        stake_locks[i] = lock_create("stake lock");
    }
    end_lock = lock_create("end lock");
    main_lock = lock_create("main lock");
    start_lock = lock_create("start lock");
    cv_start = cv_create("start cv");
    main_done = cv_create("main done");
    cv_done = cv_create("balloon done");

    /* Ensure that the main thread calls cv_wait(main_done, main_lock) before the balloon thread obtains main_lock */
    lock_acquire(start_lock);

    err = thread_fork("Marigold Thread",
        NULL, marigold, NULL, 0);
    if (err)
        goto panic;

    err = thread_fork("Dandelion Thread",
        NULL, dandelion, NULL, 0);
    if (err)
        goto panic;

    for (i = 0; i < N_LORD_FLOWERKILLER; i++) {
        err = thread_fork("Lord FlowerKiller Thread",
            NULL, flowerkiller, NULL, 0);
        if (err)
            goto panic;
    }

    err = thread_fork("Air Balloon",
        NULL, balloon, NULL, 0);
    if (err)
        goto panic;

    goto done;
panic:
    panic("airballoon: thread_fork failed: %s)\n",
        strerror(err));

done:
    /* Acquire main_lock to ensure main thread does not complete until signalled by balloon thread */
    lock_acquire(main_lock);
    /* Let other threads start now that the main_lock is acquired */
    lock_release(start_lock);

    /* Wait for the balloon thread to finish */
    /* NOTE: since only one thread calls this wait, no need for encompassing while loop */
    cv_wait(main_done, main_lock);
    lock_release(main_lock);

    /* Destroy dynamically allocated structs */
    for (int i = 0; i < NROPES; i++) {
        lock_destroy(rope_locks[i]);
        lock_destroy(stake_locks[i]);
    }
    lock_destroy(end_lock);
    lock_destroy(main_lock);
    lock_destroy(start_lock);
    cv_destroy(cv_start);
    cv_destroy(cv_done);
    cv_destroy(main_done);

    /* Reset static global variables */
    ropes_left = NROPES;
    num_done = 0;
    num_start = 0;

    kprintf("Main thread done\n");
    return 0;
}
