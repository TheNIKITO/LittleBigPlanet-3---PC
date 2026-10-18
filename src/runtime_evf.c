/* Kernel event flags: a 64-bit pattern that threads wait on (all or any of some bits, then
 * optionally clear them). Waiters are served in arrival order; priority-ordered flags
 * (TH_PRIO) are approximated by it, as semaphores are. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define ATTR_SINGLE 0x10
#define WAIT_AND 0x01
#define WAIT_OR 0x02
#define WAIT_CLEAR_ALL 0x10
#define WAIT_CLEAR_PAT 0x20

typedef struct Waiter {
    uint64_t bits, result;
    uint32_t mode;
    int32_t status;
    int done;
    pthread_cond_t event;
    struct Waiter *next;
} Waiter;
typedef struct EventFlag {
    uint32_t attr;
    uint64_t pattern;
    char name[32];
    unsigned active;
    int deleted;
    Waiter *first;
    struct EventFlag *next;
} EventFlag;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static EventFlag *registry;
static size_t created, deleted, satisfied_waits, timed_out;

static void host_check(int e) {
    if (e) { fprintf(stderr,"STOP: event flag host error %d\n",e); exit(21); }
}
/* As runtime_sema.c: winpthreads condition variables only take CLOCK_REALTIME. */
#ifdef _WIN32
#define WAIT_CLOCK CLOCK_REALTIME
#else
#define WAIT_CLOCK CLOCK_MONOTONIC
#endif
static uint64_t now_ns(void) {
    struct timespec t;
    if (clock_gettime(WAIT_CLOCK,&t)) { perror("clock_gettime"); exit(1); }
    return (uint64_t)t.tv_sec*1000000000+(uint64_t)t.tv_nsec;
}
/* The guest handle is the EventFlag pointer, checked against the registry. */
static EventFlag *find(const EventFlag *handle) {
    for (EventFlag *f=registry;f;f=f->next) if (f==handle) return f;
    return NULL;
}
static int valid_mode(uint64_t bits,uint32_t mode) {
    uint32_t kind=mode&3, clear=mode&(WAIT_CLEAR_ALL|WAIT_CLEAR_PAT);
    return bits && (kind==WAIT_AND || kind==WAIT_OR) && clear!=(WAIT_CLEAR_ALL|WAIT_CLEAR_PAT) && !(mode&~UINT32_C(0x33));
}
static int satisfied(uint64_t pattern,uint64_t bits,uint32_t mode) {
    return (mode&WAIT_AND) ? (pattern&bits)==bits : (pattern&bits)!=0;
}
/* The pattern the waiter sees, then the clear it asked for. */
static uint64_t consume(EventFlag *f,uint64_t bits,uint32_t mode) {
    uint64_t seen=f->pattern;
    if (mode&WAIT_CLEAR_ALL) f->pattern=0;
    else if (mode&WAIT_CLEAR_PAT) f->pattern&=~bits;
    return seen;
}

/* The options (SceKernelEventFlagOptParam) hold only their own size. */
static ABI int32_t flag_create(EventFlag **out,const char *name,uint32_t attr,uint64_t pattern,const void *options) {
    (void)options;
    if (!out || !name || (attr&~UINT32_C(0x33))) return ERR(22);
    if (strlen(name)>=sizeof(((EventFlag *)0)->name)) return ERR(63); /* ENAMETOOLONG */
    EventFlag *f=calloc(1,sizeof(*f));
    if (!f) return ERR(12);
    f->attr=attr; f->pattern=pattern;
    strcpy(f->name,name);
    pthread_mutex_lock(&lock);
    f->next=registry; registry=f; ++created;
    pthread_mutex_unlock(&lock);
    *out=f;
    printf("Runtime: event flag '%s' attr=0x%x pattern=0x%llx\n",name,attr,(unsigned long long)pattern);
    return 0;
}
static int32_t wait_flag(EventFlag *handle,uint64_t bits,uint32_t mode,uint64_t *result,uint32_t *timeout,int block) {
    if (!valid_mode(bits,mode)) return ERR(22);
    pthread_mutex_lock(&lock);
    EventFlag *f=find(handle);
    int32_t status=0;
    if (!f) status=ERR(3);
    else if ((f->attr&ATTR_SINGLE) && f->first) status=ERR(1); /* EPERM: one waiter at a time */
    else if (satisfied(f->pattern,bits,mode)) {
        uint64_t seen=consume(f,bits,mode);
        if (result) *result=seen;
        ++satisfied_waits;
    } else if (!block) {
        status=ERR(16);
        if (result) *result=f->pattern;
    } else if (timeout && !*timeout) {
        status=ERR(60); ++timed_out;
        if (result) *result=f->pattern;
    } else {
        Waiter w={.bits=bits,.mode=mode};
        pthread_condattr_t attr;
        host_check(pthread_condattr_init(&attr));
        host_check(pthread_condattr_setclock(&attr,WAIT_CLOCK));
        host_check(pthread_cond_init(&w.event,&attr));
        host_check(pthread_condattr_destroy(&attr));
        Waiter **tail=&f->first;
        while (*tail) tail=&(*tail)->next;
        *tail=&w; ++f->active;
        uint64_t deadline=timeout ? now_ns()+(uint64_t)*timeout*1000 : 0;
        struct timespec end={.tv_sec=(time_t)(deadline/1000000000),.tv_nsec=(long)(deadline%1000000000)};
        while (!w.done) {
            int e=timeout ? pthread_cond_timedwait(&w.event,&lock,&end) : pthread_cond_wait(&w.event,&lock);
            if (e==ETIMEDOUT && !w.done) {
                Waiter **p=&f->first;
                while (*p!=&w) p=&(*p)->next;
                *p=w.next; w.status=ERR(60); w.result=f->pattern; w.done=1; ++timed_out;
            } else if (e!=ETIMEDOUT) host_check(e);
        }
        status=w.status;
        if (!status) ++satisfied_waits;
        if (result) *result=w.result;
        if (timeout) {
            uint64_t now=now_ns();
            *timeout=status ? 0 : (uint32_t)(now>=deadline ? 0 : (deadline-now)/1000);
        }
        host_check(pthread_cond_destroy(&w.event));
        if (!--f->active && f->deleted) free(f);
    }
    pthread_mutex_unlock(&lock);
    return status;
}
static ABI int32_t flag_wait(EventFlag *f,uint64_t bits,uint32_t mode,uint64_t *result,uint32_t *timeout) {
    return wait_flag(f,bits,mode,result,timeout,1);
}
static ABI int32_t flag_poll(EventFlag *f,uint64_t bits,uint32_t mode,uint64_t *result) {
    return wait_flag(f,bits,mode,result,NULL,0);
}
/* Waiters are checked in arrival order against the pattern as each earlier one left it. */
static ABI int32_t flag_set(EventFlag *handle,uint64_t bits) {
    pthread_mutex_lock(&lock);
    EventFlag *f=find(handle);
    if (!f) { pthread_mutex_unlock(&lock); return ERR(3); }
    f->pattern|=bits;
    Waiter **p=&f->first;
    while (*p) {
        Waiter *w=*p;
        if (!satisfied(f->pattern,w->bits,w->mode)) { p=&w->next; continue; }
        w->result=consume(f,w->bits,w->mode); w->status=0; w->done=1; *p=w->next;
        host_check(pthread_cond_signal(&w->event));
    }
    pthread_mutex_unlock(&lock);
    return 0;
}
/* The pattern keeps only the bits given (an AND, as on the PS4). */
static ABI int32_t flag_clear(EventFlag *handle,uint64_t bits) {
    pthread_mutex_lock(&lock);
    EventFlag *f=find(handle);
    if (f) f->pattern&=bits;
    pthread_mutex_unlock(&lock);
    return f ? 0 : ERR(3);
}
static int32_t wake_all(EventFlag *f,int32_t status) {
    int32_t count=0;
    while (f->first) {
        Waiter *w=f->first; f->first=w->next;
        w->status=status; w->result=f->pattern; w->done=1; ++count;
        host_check(pthread_cond_signal(&w->event));
    }
    return count;
}
static ABI int32_t flag_cancel(EventFlag *handle,uint64_t pattern,int32_t *waiters) {
    pthread_mutex_lock(&lock);
    EventFlag *f=find(handle);
    if (f) {
        f->pattern=pattern;
        int32_t n=wake_all(f,ERR(85)); /* ECANCELED */
        if (waiters) *waiters=n;
    }
    pthread_mutex_unlock(&lock);
    return f ? 0 : ERR(3);
}
static ABI int32_t flag_delete(EventFlag *handle) {
    pthread_mutex_lock(&lock);
    EventFlag **p=&registry;
    while (*p && *p!=handle) p=&(*p)->next;
    if (!*p) { pthread_mutex_unlock(&lock); return ERR(3); }
    EventFlag *f=*p; *p=f->next; f->deleted=1; ++deleted;
    wake_all(f,ERR(13)); /* EACCES */
    if (!f->active) free(f);
    pthread_mutex_unlock(&lock);
    return 0;
}
uintptr_t runtime_evf_resolve(const char *name) {
    if (!strcmp(name,"BpFoboUJoZU#libkernel")) return (uintptr_t)flag_create;
    if (!strcmp(name,"JTvBflhYazQ#libkernel")) return (uintptr_t)flag_wait;
    if (!strcmp(name,"9lvj5DjHZiA#libkernel")) return (uintptr_t)flag_poll;
    if (!strcmp(name,"IOnSvHzqu6A#libkernel")) return (uintptr_t)flag_set;
    if (!strcmp(name,"7uhBFWRAS60#libkernel")) return (uintptr_t)flag_clear;
    if (!strcmp(name,"PZku4ZrXJqg#libkernel")) return (uintptr_t)flag_cancel;
    if (!strcmp(name,"8mql9OcQnd4#libkernel")) return (uintptr_t)flag_delete;
    return 0;
}
void runtime_evf_report(void) {
    pthread_mutex_lock(&lock);
    printf("Runtime: event flags created=%zu, deleted=%zu, waits satisfied=%zu, timeouts=%zu\n",
           created,deleted,satisfied_waits,timed_out);
    pthread_mutex_unlock(&lock);
}
