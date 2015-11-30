#include "mtrs_comm.h"
#include "mpthread.h"


#ifdef WIN32
/*** Mutexes ***/
void anc_mutex_init( anc_mutex_t *p_mutex )
{
    /* This creates a recursive mutex. This is OK as fast mutexes have
     * no defined behavior in case of recursive locking. */
    InitializeCriticalSection (&p_mutex->mutex);
    p_mutex->dynamic = true;
}

void anc_mutex_init_recursive( anc_mutex_t *p_mutex )
{
	InitializeCriticalSection (&p_mutex->mutex);
    p_mutex->dynamic = true;
}


void anc_mutex_destroy (anc_mutex_t *p_mutex)
{
    assert (p_mutex->dynamic);
    DeleteCriticalSection (&p_mutex->mutex);
}

void anc_mutex_lock (anc_mutex_t *p_mutex)
{
    EnterCriticalSection (&p_mutex->mutex);
}

int anc_mutex_trylock (anc_mutex_t *p_mutex)
{
    return TryEnterCriticalSection (&p_mutex->mutex) ? 0 : EBUSY;
}

void anc_mutex_unlock (anc_mutex_t *p_mutex)
{
    LeaveCriticalSection (&p_mutex->mutex);
}

/*** Semaphore ***/
void anc_sem_init (anc_sem_t *sem, unsigned value)
{
    *sem = CreateSemaphore (NULL, value, 0x7fffffff, NULL);
    if (*sem == NULL)
        abort ();
}

void anc_sem_destroy (anc_sem_t *sem)
{
    CloseHandle (*sem);
}

int anc_sem_post (anc_sem_t *sem)
{
    ReleaseSemaphore (*sem, 1, NULL);
    return 0;
}

void anc_sem_wait (anc_sem_t *sem)
{
    DWORD result;

	result = WaitForSingleObjectEx (*sem, INFINITE, TRUE);
}

/*** Thread-specific variables (TLS) ***/
int anc_threadvar_create (anc_threadvar_t *p_tls, void (*destr) (void *))
{
//#warning FIXME: use destr() callback and stop leaking!
    ANC_UNUSED( destr );

    *p_tls = TlsAlloc();
    return (*p_tls == TLS_OUT_OF_INDEXES) ? EAGAIN : 0;
}

void anc_threadvar_delete (anc_threadvar_t *p_tls)
{
    TlsFree (*p_tls);
}

/**
 * Sets a thread-local variable.
 * @param key thread-local variable key (created with anc_threadvar_create())
 * @param value new value for the variable for the calling thread
 * @return 0 on success, a system error code otherwise.
 */
int anc_threadvar_set (anc_threadvar_t key, void *value)
{
    return TlsSetValue (key, value) ? ENOMEM : 0;
}

/**
 * Gets the value of a thread-local variable for the calling thread.
 * This function cannot fail.
 * @return the value associated with the given variable for the calling
 * or NULL if there is no value.
 */
void *anc_threadvar_get (anc_threadvar_t key)
{
    return TlsGetValue (key);
}

//threads
struct anc_entry_data
{
    void * (*func) (void *);
    void *  data;
    anc_sem_t ready;
};

static unsigned __stdcall anc_entry (void *p)
{
    struct anc_entry_data *entry = (struct anc_entry_data *)p;
    void *(*func) (void *) = entry->func;
    void *data = entry->data;

    anc_sem_post (&entry->ready);
    func (data);
    return 0;
}

int anc_clone (anc_thread_t *p_handle, void * (*entry) (void *), void *data,
               int priority)
{
    int err = ENOMEM;
    HANDLE hThread;

    struct anc_entry_data *entry_data = (struct anc_entry_data *)malloc (sizeof (*entry_data));
    if (entry_data == NULL)
        return ENOMEM;
    entry_data->func = entry;
    entry_data->data = data;
    anc_sem_init (&entry_data->ready, 0);

    /* When using the MSVCRT C library you have to use the _beginthreadex
     * function instead of CreateThread, otherwise you'll end up with
     * memory leaks and the signal functions not working (see Microsoft
     * Knowledge Base, article 104641) */
    hThread = (HANDLE)(uintptr_t)
        _beginthreadex (NULL, 0, anc_entry, entry_data, CREATE_SUSPENDED, NULL);
    if (! hThread)
    {
        err = errno;
        goto error;
    }

    /* Thread closes the handle when exiting, duplicate it here
     * to be on the safe side when joining. */
    if (!DuplicateHandle (GetCurrentProcess (), hThread,
                          GetCurrentProcess (), p_handle, 0, FALSE,
                          DUPLICATE_SAME_ACCESS))
    {
        CloseHandle (hThread);
        goto error;
    }

	ResumeThread (hThread);
    if (priority)
        SetThreadPriority (hThread, priority);

    /* Prevent cancellation until cancel_data is initialized. */
    /* XXX: This could be postponed to anc_cancel() or avoided completely by
     * passing the "right" pointer to anc_cancel_self(). */
    anc_sem_wait (&entry_data->ready);
    anc_sem_destroy (&entry_data->ready);
    free (entry_data);

    return 0;

error:
    anc_sem_destroy (&entry_data->ready);
    free (entry_data);
    return err;
}

void anc_join (anc_thread_t handle, void **result)
{
    WaitForSingleObjectEx (handle, INFINITE, TRUE);

    CloseHandle (handle);
    assert (result == NULL); /* <- FIXME if ever needed */
}
#else

#include <signal.h>
#   define likely(p)   __builtin_expect(!!(p), 1)
#   define unlikely(p) __builtin_expect(!!(p), 0)
typedef int64_t mtime_t;

#ifndef NDEBUG
/*****************************************************************************
 * anc_thread_fatal: Report an error from the threading layer
 *****************************************************************************
 * This is mostly meant for debugging.
 *****************************************************************************/
static void
anc_thread_fatal (const char *action, int error,
                  const char *function, const char *file, unsigned line)
{
    abort ();
}

# define ANC_THREAD_ASSERT( action ) \
    if (unlikely(val)) \
        anc_thread_fatal (action, val, __func__, __FILE__, __LINE__)
#else
# define ANC_THREAD_ASSERT( action ) ((void)val)
#endif


/*****************************************************************************
 * anc_mutex_init: initialize a mutex
 *****************************************************************************/
void anc_mutex_init( anc_mutex_t *p_mutex )
{
    pthread_mutexattr_t attr;

    if (unlikely(pthread_mutexattr_init (&attr)))
        abort();
#ifdef NDEBUG
    pthread_mutexattr_settype( &attr, PTHREAD_MUTEX_NORMAL );
#else
    /* Create error-checking mutex to detect problems more easily. */
# if defined (__GLIBC__) && (__GLIBC__ == 2) && (__GLIBC_MINOR__ < 6) && !defined(__mips__)
    pthread_mutexattr_setkind_np( &attr, PTHREAD_MUTEX_ERRORCHECK_NP );
# else
    pthread_mutexattr_settype( &attr, PTHREAD_MUTEX_ERRORCHECK );
# endif
#endif
    if (unlikely(pthread_mutex_init (p_mutex, &attr)))
        abort();
    pthread_mutexattr_destroy( &attr );
}

/*****************************************************************************
 * anc_mutex_init: initialize a recursive mutex (Do not use)
 *****************************************************************************/
void anc_mutex_init_recursive( anc_mutex_t *p_mutex )
{
    pthread_mutexattr_t attr;

    if (unlikely(pthread_mutexattr_init (&attr)))
        abort();
#if defined (__GLIBC__) && (__GLIBC__ == 2) && (__GLIBC_MINOR__ < 6)&& !defined(__mips__)
    pthread_mutexattr_setkind_np( &attr, PTHREAD_MUTEX_RECURSIVE_NP );
#else
    pthread_mutexattr_settype( &attr, PTHREAD_MUTEX_RECURSIVE );
#endif
    if (unlikely(pthread_mutex_init (p_mutex, &attr)))
        abort();
    pthread_mutexattr_destroy( &attr );
}


/**
 * Destroys a mutex. The mutex must not be locked.
 *
 * @param p_mutex mutex to destroy
 * @return always succeeds
 */
void anc_mutex_destroy (anc_mutex_t *p_mutex)
{
    int val = pthread_mutex_destroy( p_mutex );
    ANC_THREAD_ASSERT ("destroying mutex");
}

#ifndef NDEBUG
# ifdef HAVE_VALGRIND_VALGRIND_H
#  include <valgrind/valgrind.h>
# else
#  define RUNNING_ON_VALGRIND (0)
# endif

void anc_assert_locked (anc_mutex_t *p_mutex)
{
    if (RUNNING_ON_VALGRIND > 0)
        return;
    assert (pthread_mutex_lock (p_mutex) == EDEADLK);
}
#endif

/**
 * Acquires a mutex. If needed, waits for any other thread to release it.
 * Beware of deadlocks when locking multiple mutexes at the same time,
 * or when using mutexes from callbacks.
 * This function is not a cancellation-point.
 *
 * @param p_mutex mutex initialized with anc_mutex_init() or
 *                anc_mutex_init_recursive()
 */
void anc_mutex_lock (anc_mutex_t *p_mutex)
{
    int val = pthread_mutex_lock( p_mutex );
    ANC_THREAD_ASSERT ("locking mutex");
}

/**
 * Acquires a mutex if and only if it is not currently held by another thread.
 * This function never sleeps and can be used in delay-critical code paths.
 * This function is not a cancellation-point.
 *
 * <b>Beware</b>: If this function fails, then the mutex is held... by another
 * thread. The calling thread must deal with the error appropriately. That
 * typically implies postponing the operations that would have required the
 * mutex. If the thread cannot defer those operations, then it must use
 * anc_mutex_lock(). If in doubt, use anc_mutex_lock() instead.
 *
 * @param p_mutex mutex initialized with anc_mutex_init() or
 *                anc_mutex_init_recursive()
 * @return 0 if the mutex could be acquired, an error code otherwise.
 */
int anc_mutex_trylock (anc_mutex_t *p_mutex)
{
    int val = pthread_mutex_trylock( p_mutex );

    if (val != EBUSY)
        ANC_THREAD_ASSERT ("locking mutex");
    return val;
}

/**
 * Releases a mutex (or crashes if the mutex is not locked by the caller).
 * @param p_mutex mutex locked with anc_mutex_lock().
 */
void anc_mutex_unlock (anc_mutex_t *p_mutex)
{
    int val = pthread_mutex_unlock( p_mutex );
    ANC_THREAD_ASSERT ("unlocking mutex");
}

/**
 * Initializes a condition variable.
 */
void anc_cond_init (anc_cond_t *p_condvar)
{
#ifdef ANDROID
    if (unlikely(pthread_cond_init (p_condvar, NULL)))
        abort ();
#else
    pthread_condattr_t attr;

    if (unlikely(pthread_condattr_init (&attr)))
        abort ();
#if !defined (_POSIX_CLOCK_SELECTION)
   /* Fairly outdated POSIX support (that was defined in 2001) */
# define _POSIX_CLOCK_SELECTION (-1)
#endif
#if (_POSIX_CLOCK_SELECTION >= 0)
    /* NOTE: This must be the same clock as the one in mtime.c */
    pthread_condattr_setclock (&attr, CLOCK_MONOTONIC);
#endif

    if (unlikely(pthread_cond_init (p_condvar, &attr)))
        abort ();
    pthread_condattr_destroy (&attr);
#endif //ANDROID
}

/**
 * Initializes a condition variable.
 * Contrary to anc_cond_init(), the wall clock will be used as a reference for
 * the anc_cond_timedwait() time-out parameter.
 */
void anc_cond_init_daytime (anc_cond_t *p_condvar)
{
    if (unlikely(pthread_cond_init (p_condvar, NULL)))
        abort ();
}

/**
 * Destroys a condition variable. No threads shall be waiting or signaling the
 * condition.
 * @param p_condvar condition variable to destroy
 */
void anc_cond_destroy (anc_cond_t *p_condvar)
{
    int val = pthread_cond_destroy( p_condvar );
    ANC_THREAD_ASSERT ("destroying condition");
}

/**
 * Wakes up one thread waiting on a condition variable, if any.
 * @param p_condvar condition variable
 */
void anc_cond_signal (anc_cond_t *p_condvar)
{
    int val = pthread_cond_signal( p_condvar );
    ANC_THREAD_ASSERT ("signaling condition variable");
}

/**
 * Wakes up all threads (if any) waiting on a condition variable.
 * @param p_cond condition variable
 */
void anc_cond_broadcast (anc_cond_t *p_condvar)
{
    pthread_cond_broadcast (p_condvar);
}

/**
 * Waits for a condition variable. The calling thread will be suspended until
 * another thread calls anc_cond_signal() or anc_cond_broadcast() on the same
 * condition variable, the thread is cancelled with anc_cancel(), or the
 * system causes a "spurious" unsolicited wake-up.
 *
 * A mutex is needed to wait on a condition variable. It must <b>not</b> be
 * a recursive mutex. Although it is possible to use the same mutex for
 * multiple condition, it is not valid to use different mutexes for the same
 * condition variable at the same time from different threads.
 *
 * In case of thread cancellation, the mutex is always locked before
 * cancellation proceeds.
 *
 * The canonical way to use a condition variable to wait for event foobar is:
 @code
   anc_mutex_lock (&lock);
   mutex_cleanup_push (&lock); // release the mutex in case of cancellation

   while (!foobar)
       anc_cond_wait (&wait, &lock);

   --- foobar is now true, do something about it here --

   anc_cleanup_run (); // release the mutex
  @endcode
 *
 * @param p_condvar condition variable to wait on
 * @param p_mutex mutex which is unlocked while waiting,
 *                then locked again when waking up.
 * @param deadline <b>absolute</b> timeout
 *
 * @return 0 if the condition was signaled, an error code in case of timeout.
 */
void anc_cond_wait (anc_cond_t *p_condvar, anc_mutex_t *p_mutex)
{
#ifdef ANDROID
    int val = pthread_cond_wait_cancel( p_condvar, p_mutex );
#else
    int val = pthread_cond_wait( p_condvar, p_mutex );
#endif
    ANC_THREAD_ASSERT ("waiting on condition");
}

/**
 * Waits for a condition variable up to a certain date.
 * This works like anc_cond_wait(), except for the additional time-out.
 *
 * If the variable was initialized with anc_cond_init(), the timeout has the
 * same arbitrary origin as mdate(). If the variable was initialized with
 * anc_cond_init_daytime(), the timeout is expressed from the Unix epoch.
 *
 * @param p_condvar condition variable to wait on
 * @param p_mutex mutex which is unlocked while waiting,
 *                then locked again when waking up.
 * @param deadline <b>absolute</b> timeout
 *
 * @return 0 if the condition was signaled, an error code in case of timeout.
 */
int anc_cond_timedwait (anc_cond_t *p_condvar, anc_mutex_t *p_mutex,
                        mtime_t deadline)
{
#if defined(__APPLE__) && !defined(__powerpc__) && !defined( __ppc__ ) && !defined( __ppc64__ )
    /* mdate() is the monotonic clock, timedwait origin is gettimeofday() which
     * isn't monotonic. Use imedwait_relative_np() instead
    */
    mtime_t base = mdate();
    deadline -= base;
    if (deadline < 0)
        deadline = 0;
    lldiv_t d = lldiv( deadline, CLOCK_FREQ );
    struct timespec ts = { d.quot, d.rem * (1000000000 / CLOCK_FREQ) };

    int val = pthread_cond_timedwait_relative_np(p_condvar, p_mutex, &ts);
    if (val != ETIMEDOUT)
        ANC_THREAD_ASSERT ("timed-waiting on condition");
    return val;
#else
    lldiv_t d = lldiv( deadline, CLOCK_FREQ );
    struct timespec ts = { d.quot, d.rem * (1000000000 / CLOCK_FREQ) };

#ifdef ANDROID
    int val = pthread_cond_timedwait_cancel (p_condvar, p_mutex, &ts);
#else
    int val = pthread_cond_timedwait (p_condvar, p_mutex, &ts);
#endif
    if (val != ETIMEDOUT)
        ANC_THREAD_ASSERT ("timed-waiting on condition");
    return val;
#endif
}

/**
 * Initializes a semaphore.
 */
void anc_sem_init (anc_sem_t *sem, unsigned value)
{
#if defined(__APPLE__)
    if (unlikely(semaphore_create(mach_task_self(), sem, SYNC_POLICY_FIFO, value) != KERN_SUCCESS))
        abort ();
#else
    if (unlikely(sem_init (sem, 0, value)))
        abort ();
#endif
}

/**
 * Destroys a semaphore.
 */
void anc_sem_destroy (anc_sem_t *sem)
{
    int val;

#if defined(__APPLE__)
    if (likely(semaphore_destroy(mach_task_self(), *sem) == KERN_SUCCESS))
        return;

    val = EINVAL;
#else
    if (likely(sem_destroy (sem) == 0))
        return;

    val = errno;
#endif

#ifdef ANDROID
    /* Bionic is so broken that it will return EBUSY on sem_destroy
     * if the semaphore has never been used...
     */
    if (likely(val == EBUSY))
        return; // It may be a real error, but there's no way to know
#endif
    ANC_THREAD_ASSERT ("destroying semaphore");
}

/**
 * Increments the value of a semaphore.
 * @return 0 on success, EOVERFLOW in case of integer overflow
 */
int anc_sem_post (anc_sem_t *sem)
{
    int val;
    
#if defined(__APPLE__)
    if (likely(semaphore_signal(*sem) == KERN_SUCCESS))
        return 0;
    
    val = EINVAL;
#else
    if (likely(sem_post (sem) == 0))
        return 0;
    
    val = errno;
#endif
    
    if (unlikely(val != EOVERFLOW))
        ANC_THREAD_ASSERT ("unlocking semaphore");
    return val;
}

/**
 * Atomically wait for the semaphore to become non-zero (if needed),
 * then decrements it.
 */
void anc_sem_wait (anc_sem_t *sem)
{
    int val;
    
#if defined(__APPLE__)
    if (likely(semaphore_wait(*sem) == KERN_SUCCESS))
        return;
    
    val = EINVAL;
#else
    do
        if (likely(sem_wait (sem) == 0))
            return;
    while ((val = errno) == EINTR);
#endif

    ANC_THREAD_ASSERT ("locking semaphore");
}

/**
 * Allocates a thread-specific variable.
 * @param key where to store the thread-specific variable handle
 * @param destr a destruction callback. It is called whenever a thread exits
 * and the thread-specific variable has a non-NULL value.
 * @return 0 on success, a system error code otherwise. This function can
 * actually fail because there is a fixed limit on the number of
 * thread-specific variable in a process on most systems.
 */
int anc_threadvar_create (anc_threadvar_t *key, void (*destr) (void *))
{
    return pthread_key_create (key, destr);
}

void anc_threadvar_delete (anc_threadvar_t *p_tls)
{
    pthread_key_delete (*p_tls);
}

/**
 * Sets a thread-specific variable.
 * @param key thread-local variable key (created with anc_threadvar_create())
 * @param value new value for the variable for the calling thread
 * @return 0 on success, a system error code otherwise.
 */
int anc_threadvar_set (anc_threadvar_t key, void *value)
{
    return pthread_setspecific (key, value);
}

/**
 * Gets the value of a thread-local variable for the calling thread.
 * This function cannot fail.
 * @return the value associated with the given variable for the calling
 * or NULL if there is no value.
 */
void *anc_threadvar_get (anc_threadvar_t key)
{
    return pthread_getspecific (key);
}

static bool rt_priorities = false;
static int rt_offset = 0;

/**
 * Creates and starts new thread.
 *
 * @param p_handle [OUT] pointer to write the handle of the created thread to
 * @param entry entry point for the thread
 * @param data data parameter given to the entry point
 * @param priority thread priority value
 * @return 0 on success, a standard error code on error.
 */
int anc_clone (anc_thread_t *p_handle, void * (*entry) (void *), void *data,
               int priority)
{
    int ret;

    pthread_attr_t attr;
    pthread_attr_init (&attr);

    /* Block the signals that signals interface plugin handles.
     * If the LibANC caller wants to handle some signals by itself, it should
     * block these before whenever invoking LibANC. And it must obviously not
     * start the ANC signals interface plugin.
     *
     * LibANC will normally ignore any interruption caused by an asynchronous
     * signal during a system call. But there may well be some buggy cases
     * where it fails to handle EINTR (bug reports welcome). Some underlying
     * libraries might also not handle EINTR properly.
     */
    sigset_t oldset;
    {
        sigset_t set;
        sigemptyset (&set);
        sigdelset (&set, SIGHUP);
        sigaddset (&set, SIGINT);
        sigaddset (&set, SIGQUIT);
        sigaddset (&set, SIGTERM);

        sigaddset (&set, SIGPIPE); /* We don't want this one, really! */
        pthread_sigmask (SIG_BLOCK, &set, &oldset);
    }

#if defined (_POSIX_PRIORITY_SCHEDULING) && (_POSIX_PRIORITY_SCHEDULING >= 0) \
 && defined (_POSIX_THREAD_PRIORITY_SCHEDULING) \
 && (_POSIX_THREAD_PRIORITY_SCHEDULING >= 0)
    if (rt_priorities)
    {
#if 0
        struct sched_param sp = { .sched_priority = priority + rt_offset, };
#else
        struct sched_param sp = { priority + rt_offset, };
#endif
        int policy;

        if (sp.sched_priority <= 0)
            sp.sched_priority += sched_get_priority_max (policy = SCHED_OTHER);
        else
            sp.sched_priority += sched_get_priority_min (policy = SCHED_RR);

        pthread_attr_setschedpolicy (&attr, policy);
        pthread_attr_setschedparam (&attr, &sp);
    }
#else
    (void) priority;
#endif

    /* The thread stack size.
     * The lower the value, the less address space per thread, the highest
     * maximum simultaneous threads per process. Too low values will cause
     * stack overflows and weird crashes. Set with caution. Also keep in mind
     * that 64-bits platforms consume more stack than 32-bits one.
     *
     * Thanks to on-demand paging, thread stack size only affects address space
     * consumption. In terms of memory, threads only use what they need
     * (rounded up to the page boundary).
     *
     * For example, on Linux i386, the default is 2 mega-bytes, which supports
     * about 320 threads per processes. */
#define ANC_STACKSIZE (128 * sizeof (void *) * 1024)

#ifdef ANC_STACKSIZE
    ret = pthread_attr_setstacksize (&attr, ANC_STACKSIZE);
    assert (ret == 0); /* fails iif ANC_STACKSIZE is invalid */
#endif

#ifndef ANDROID
    ret = pthread_create (p_handle, &attr, entry, data);
#else
    ret = pthread_create_cancel (p_handle, &attr, entry, data);
#endif
    pthread_sigmask (SIG_SETMASK, &oldset, NULL);
    pthread_attr_destroy (&attr);
    return ret;
}

/**
 * Marks a thread as cancelled. Next time the target thread reaches a
 * cancellation point (while not having disabled cancellation), it will
 * run its cancellation cleanup handler, the thread variable destructors, and
 * terminate. anc_join() must be used afterward regardless of a thread being
 * cancelled or not.
 */
void anc_cancel (anc_thread_t thread_id)
{
    pthread_cancel (thread_id);
#ifdef HAVE_MAEMO
    pthread_kill (thread_id, SIGRTMIN);
#endif
}

/**
 * Waits for a thread to complete (if needed), then destroys it.
 * This is a cancellation point; in case of cancellation, the join does _not_
 * occur.
 * @warning
 * A thread cannot join itself (normally ANC will abort if this is attempted).
 * Also, a detached thread <b>cannot</b> be joined.
 *
 * @param handle thread handle
 * @param p_result [OUT] pointer to write the thread return value or NULL
 */
void anc_join (anc_thread_t handle, void **result)
{
    int val = pthread_join (handle, result);
    ANC_THREAD_ASSERT ("joining thread");
}

/**
 * Detaches a thread. When the specified thread completes, it will be
 * automatically destroyed (in particular, its stack will be reclaimed),
 * instead of waiting for another thread to call anc_join(). If the thread has
 * already completed, it will be destroyed immediately.
 *
 * When a thread performs some work asynchronously and may complete much
 * earlier than it can be joined, detaching the thread can save memory.
 * However, care must be taken that any resources used by a detached thread
 * remains valid until the thread completes. This will typically involve some
 * kind of thread-safe signaling.
 *
 * A thread may detach itself.
 *
 * @param handle thread handle
 */
void anc_detach (anc_thread_t handle)
{
    int val = pthread_detach (handle);
    ANC_THREAD_ASSERT ("detaching thread");
}
#endif
