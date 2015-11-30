#ifndef MTRS_THREAD_H
# define MTRS_THREAD_H 1

#if defined( UNDER_CE )
#elif defined( WIN32 )
#   include <process.h>                                         /* Win32 API */

#else                                         /* pthreads (like Linux & BSD) */
#   define LIBANC_USE_PTHREAD 1
#   define LIBANC_USE_PTHREAD_CANCEL 1
#   define _APPLE_C_SOURCE    1 /* Proper pthread semantics on OSX */

#   include <unistd.h> /* _POSIX_SPIN_LOCKS */
#   include <pthread.h>
#   include <semaphore.h>
#ifdef __cplusplus
#	include <map>
#endif //__cplusplus
#endif


/* Thread priorities */
#ifdef __APPLE__
#   define ANC_THREAD_PRIORITY_LOW      0
#   define ANC_THREAD_PRIORITY_INPUT   22
#   define ANC_THREAD_PRIORITY_AUDIO   22
#   define ANC_THREAD_PRIORITY_VIDEO    0
#   define ANC_THREAD_PRIORITY_OUTPUT  22
#   define ANC_THREAD_PRIORITY_HIGHEST 22

#elif defined(LIBANC_USE_PTHREAD)
#   define ANC_THREAD_PRIORITY_LOW      0
#   define ANC_THREAD_PRIORITY_INPUT   10
#   define ANC_THREAD_PRIORITY_AUDIO    5
#   define ANC_THREAD_PRIORITY_VIDEO    0
#   define ANC_THREAD_PRIORITY_OUTPUT  15
#   define ANC_THREAD_PRIORITY_HIGHEST 20

#elif defined(WIN32) || defined(UNDER_CE)
/* Define different priorities for WinNT/2K/XP and Win9x/Me */
#   define ANC_THREAD_PRIORITY_LOW 0
#   define ANC_THREAD_PRIORITY_INPUT \
        THREAD_PRIORITY_ABOVE_NORMAL
#   define ANC_THREAD_PRIORITY_AUDIO \
        THREAD_PRIORITY_HIGHEST
#   define ANC_THREAD_PRIORITY_VIDEO 0
#   define ANC_THREAD_PRIORITY_OUTPUT \
        THREAD_PRIORITY_ABOVE_NORMAL
#   define ANC_THREAD_PRIORITY_HIGHEST \
        THREAD_PRIORITY_TIME_CRITICAL

#else
#   define ANC_THREAD_PRIORITY_LOW 0
#   define ANC_THREAD_PRIORITY_INPUT 0
#   define ANC_THREAD_PRIORITY_AUDIO 0
#   define ANC_THREAD_PRIORITY_VIDEO 0
#   define ANC_THREAD_PRIORITY_OUTPUT 0
#   define ANC_THREAD_PRIORITY_HIGHEST 0

#endif


/*****************************************************************************
 * Type definitions
 *****************************************************************************/

#if defined (LIBANC_USE_PTHREAD)
typedef pthread_t       anc_thread_t;
typedef pthread_mutex_t anc_mutex_t;
#define ANC_STATIC_MUTEX PTHREAD_MUTEX_INITIALIZER
typedef pthread_cond_t  anc_cond_t;
#define ANC_STATIC_COND  PTHREAD_COND_INITIALIZER

#ifdef __APPLE__
typedef semaphore_t     anc_sem_t;
#else
typedef sem_t           anc_sem_t;
#endif

#ifdef ANDROID
typedef anc_mutex_t	anc_rwlock_t;
#else
typedef pthread_rwlock_t anc_rwlock_t;
#endif
typedef pthread_key_t   anc_threadvar_t;
typedef struct anc_timer *anc_timer_t;
typedef struct anc_event anc_event_t;

#ifdef __cplusplus
typedef std::map<anc_sem_t*, anc_thread_t>	anc_event_wait;
struct anc_event_internal
{
#ifdef __APPLE__
    sem_t*          psem;
    char*           sem_name;
#else
    anc_sem_t		sem;
#endif
    anc_mutex_t		lock;
    anc_event_wait	waitlist;
};

struct anc_event
{
	anc_event_internal*	p;
};
#endif //__cplusplus

#elif defined( WIN32 )
#if !defined( UNDER_CE )
typedef HANDLE anc_thread_t;
#else
typedef struct
{
    HANDLE handle;
    HANDLE cancel_event;
} *anc_thread_t;
#endif

typedef struct
{
    bool dynamic;
    union
    {
        struct
        {
            bool locked;
            unsigned long contention;
        };
        CRITICAL_SECTION mutex;
    };
} anc_mutex_t;
#define ANC_STATIC_MUTEX { false, { { false, 0 } } }

typedef struct
{
    HANDLE   handle;
    unsigned clock;
} anc_cond_t;

typedef HANDLE  anc_sem_t;
typedef HANDLE	anc_event_t;

typedef struct
{
    anc_mutex_t   mutex;
    anc_cond_t    read_wait;
    anc_cond_t    write_wait;
    unsigned long readers;
    unsigned long writers;
    DWORD         writer;
} anc_rwlock_t;

typedef DWORD   anc_threadvar_t;
typedef struct anc_timer *anc_timer_t;
#endif

#if defined( WIN32 ) && !defined ETIMEDOUT
#  define ETIMEDOUT 10060 /* This is the value in winsock.h. */
#endif

////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//funtions
////////////////////////////////////////////////////////////////////////////////////////////////////////////////
int anc_clone(anc_thread_t *, void * (*) (void *), void *, int);
void anc_join(anc_thread_t, void **);
int anc_threadvar_create(anc_threadvar_t * , void (*) (void *) );
void anc_threadvar_delete(anc_threadvar_t *);
int anc_threadvar_set(anc_threadvar_t, void *);
void *anc_threadvar_get(anc_threadvar_t);

void anc_mutex_init( anc_mutex_t *p_mutex );
void anc_mutex_init_recursive( anc_mutex_t *p_mutex );
void anc_mutex_destroy (anc_mutex_t *p_mutex);
void anc_mutex_lock (anc_mutex_t *p_mutex);
int anc_mutex_trylock (anc_mutex_t *p_mutex);
void anc_mutex_unlock (anc_mutex_t *p_mutex);
void anc_sem_init (anc_sem_t *sem, unsigned value);
void anc_sem_destroy (anc_sem_t *sem);
int anc_sem_post (anc_sem_t *sem);
void anc_sem_wait (anc_sem_t *sem);
#endif