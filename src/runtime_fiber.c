/* libSceFiber: cooperative fibers. sceFiberRun runs a fiber on the calling thread until the fiber
 * calls sceFiberReturnToThread; the next Run, from any thread, resumes it right there. A fiber
 * made without context memory runs on its caller's stack on the PS4 and cannot be resumed: its
 * next Run starts the entry again, and so it does here.
 * Every fiber runs on a host stack: a Windows fiber (it keeps the stack bounds in the TEB right,
 * which exception dispatch checks) or a ucontext elsewhere. The game's context memory is left
 * unused, and the host stack is larger than it: runtime and GPU code run on it too. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <ucontext.h>
#include <sys/mman.h>
#endif

#define FIBER_NULL ((int32_t)0x80590001)
#define FIBER_ALIGNMENT ((int32_t)0x80590002)
#define FIBER_RANGE ((int32_t)0x80590003)
#define FIBER_INVALID ((int32_t)0x80590004)
#define FIBER_PERMISSION ((int32_t)0x80590005)
#define FIBER_STATE ((int32_t)0x80590006)
#define FIBER_MAGIC UINT32_C(0xdef1649c)
#define OPTION_MAGIC UINT32_C(0xbb40e64d)
#define CONTEXT_MINIMUM 512
#define CONTEXTLESS_STACK (1024*1024) /* the caller's stack on the PS4 */
#define HOST_STACK_MARGIN (256*1024)
enum { RUNNING=1, IDLE=2, TERMINATED=3 };

typedef void (ABI *FiberEntry)(uint64_t arg_on_initialize, uint64_t arg_on_run);
struct HostFiber;
/* The game's SceFiber (256 bytes) is opaque to it: this port keeps a magic and its record there. */
typedef struct { uint32_t magic, reserved; struct HostFiber *host; } GuestFiber;
typedef struct HostFiber {
    GuestFiber *guest;
    FiberEntry entry;
    uint64_t arg_initialize, arg_run, arg_return;
    void *context;
    uint64_t context_size;
    char name[32];
    int state, restart;
    uint32_t mxcsr;
    uint16_t fpucw;
#ifdef _WIN32
    void *fiber, *caller;
#else
    ucontext_t self, caller;
    void *stack;
    size_t stack_size;
    int made;
#endif
} HostFiber;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static size_t created, runs, finalized;
/* The fiber running on this thread. Read once on entry to a call: a fiber can resume on another
 * thread, and a thread-local address computed before the switch would be the first thread's. */
static _Thread_local HostFiber *running;

static HostFiber *host_of(GuestFiber *g) {
    if (!g || g->magic!=FIBER_MAGIC || !g->host || g->host->guest!=g) return NULL;
    return g->host;
}
static size_t host_stack(const HostFiber *h) {
    return (size_t)(h->context ? h->context_size : CONTEXTLESS_STACK)+HOST_STACK_MARGIN;
}
static void read_fp(uint32_t *mxcsr, uint16_t *fpucw) {
    __asm__ volatile("stmxcsr %0" : "=m"(*mxcsr));
    __asm__ volatile("fnstcw %0" : "=m"(*fpucw));
}
static void write_fp(uint32_t mxcsr, uint16_t fpucw) {
    __asm__ volatile("ldmxcsr %0" :: "m"(mxcsr));
    __asm__ volatile("fldcw %0" :: "m"(fpucw));
}
/* A new fiber starts with the floating-point modes of the thread that first runs it. */
static void fiber_body(HostFiber *h) {
    write_fp(h->mxcsr,h->fpucw);
    h->entry(h->arg_initialize,h->arg_run);
    fprintf(stderr,"STOP: fiber '%s' returned from its entry function\n",h->name);
    fflush(NULL);
    exit(21);
}
#ifdef _WIN32
static VOID CALLBACK fiber_main(LPVOID parameter) { fiber_body(parameter); }
static int made(const HostFiber *h) { return h->fiber!=NULL; }
static void make(HostFiber *h) {
    size_t size=host_stack(h);
    h->fiber=CreateFiberEx(size,size,FIBER_FLAG_FLOAT_SWITCH,fiber_main,h);
    if (!h->fiber) { fprintf(stderr,"STOP: CreateFiberEx failed (%lu)\n",GetLastError()); exit(21); }
}
static void drop(HostFiber *h) { if (h->fiber) DeleteFiber(h->fiber); h->fiber=NULL; }
static void release(HostFiber *h) { drop(h); }
static void enter(HostFiber *h) {
    void *self=IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiberEx(NULL,FIBER_FLAG_FLOAT_SWITCH);
    if (!self) { fprintf(stderr,"STOP: ConvertThreadToFiberEx failed (%lu)\n",GetLastError()); exit(21); }
    h->caller=self;
    SwitchToFiber(h->fiber);
}
static void leave(HostFiber *h) { SwitchToFiber(h->caller); }
#else
static void fiber_main(unsigned low, unsigned high) {
    fiber_body((HostFiber *)(uintptr_t)(((uint64_t)high<<32)|low));
}
static int made(const HostFiber *h) { return h->made; }
static void make(HostFiber *h) {
    if (!h->stack) {
        h->stack_size=host_stack(h);
        h->stack=runtime_low_map(h->stack_size,PROT_READ|PROT_WRITE); /* below 1 TiB, as on the PS4 */
        if (!h->stack) { fputs("STOP: cannot allocate a fiber stack\n",stderr); exit(21); }
    }
    if (getcontext(&h->self)) { perror("getcontext"); exit(21); }
    h->self.uc_stack.ss_sp=h->stack;
    h->self.uc_stack.ss_size=h->stack_size;
    h->self.uc_link=NULL;
    makecontext(&h->self,(void (*)(void))fiber_main,2,(unsigned)(uintptr_t)h,(unsigned)((uintptr_t)h>>32));
    h->made=1;
}
static void drop(HostFiber *h) { h->made=0; }
static void release(HostFiber *h) { if (h->stack) munmap(h->stack,h->stack_size); h->stack=NULL; }
static void enter(HostFiber *h) { swapcontext(&h->caller,&h->self); }
static void leave(HostFiber *h) { swapcontext(&h->self,&h->caller); }
#endif

static ABI int32_t fiber_initialize(GuestFiber *g, const char *name, FiberEntry entry, uint64_t arg_initialize,
                                    void *context, uint64_t context_size, const uint32_t *options, uint32_t build) {
    (void)build;
    if (!g || !name || !entry) return FIBER_NULL;
    if (((uintptr_t)g&7) || ((uintptr_t)context&15) || (options && ((uintptr_t)options&7))) return FIBER_ALIGNMENT;
    if (context_size && context_size<CONTEXT_MINIMUM) return FIBER_RANGE;
    if ((context_size&15) || (context==NULL)!=(context_size==0) || (options && *options!=OPTION_MAGIC)) return FIBER_INVALID;
    HostFiber *h=calloc(1,sizeof(*h));
    if (!h) { fputs("STOP: cannot allocate a fiber\n",stderr); exit(21); }
    h->guest=g; h->entry=entry; h->arg_initialize=arg_initialize;
    h->context=context; h->context_size=context_size; h->state=IDLE;
    snprintf(h->name,sizeof(h->name),"%s",name);
    g->magic=FIBER_MAGIC; g->host=h;
    pthread_mutex_lock(&lock);
    size_t n=++created;
    pthread_mutex_unlock(&lock);
    if (n<=16) printf("Runtime: fiber '%s' created (context %llu bytes)\n",h->name,(unsigned long long)context_size);
    return 0;
}
static ABI int32_t fiber_option_initialize(uint32_t *options) {
    if (!options) return FIBER_NULL;
    if ((uintptr_t)options&7) return FIBER_ALIGNMENT;
    *options=OPTION_MAGIC;
    return 0;
}
static ABI int32_t fiber_finalize(GuestFiber *g) {
    if (!g) return FIBER_NULL;
    if ((uintptr_t)g&7) return FIBER_ALIGNMENT;
    HostFiber *h=host_of(g);
    if (!h) return FIBER_INVALID;
    pthread_mutex_lock(&lock);
    int idle=h->state==IDLE;
    if (idle) { h->state=TERMINATED; ++finalized; }
    pthread_mutex_unlock(&lock);
    if (!idle) return FIBER_STATE;
    drop(h); release(h);
    g->magic=0; g->host=NULL;
    free(h);
    return 0;
}
static ABI int32_t fiber_run(GuestFiber *g, uint64_t arg_run, uint64_t *arg_return) {
    if (!g) return FIBER_NULL;
    if ((uintptr_t)g&7) return FIBER_ALIGNMENT;
    HostFiber *h=host_of(g);
    if (!h) return FIBER_INVALID;
    if (running) return FIBER_PERMISSION; /* only a thread runs fibers */
    pthread_mutex_lock(&lock);
    int idle=h->state==IDLE;
    if (idle) { h->state=RUNNING; ++runs; }
    pthread_mutex_unlock(&lock);
    if (!idle) return FIBER_STATE;
    h->arg_run=arg_run;
    if (!made(h)) { read_fp(&h->mxcsr,&h->fpucw); make(h); }
    running=h;
    enter(h);
    running=NULL; /* this thread's own frame: it always resumes on this thread */
    if (h->restart) { drop(h); h->restart=0; }
    uint64_t value=h->arg_return;
    pthread_mutex_lock(&lock);
    h->state=IDLE;
    pthread_mutex_unlock(&lock);
    if (arg_return) *arg_return=value;
    return 0;
}
static ABI int32_t fiber_return_to_thread(uint64_t arg_return, uint64_t *arg_run) {
    HostFiber *h=running;
    if (!h) return FIBER_PERMISSION;
    h->arg_return=arg_return;
    if (!h->context) h->restart=1; /* nothing to resume: the next Run starts the entry again */
    leave(h);
    /* Resumed by the next sceFiberRun, maybe on another thread: no thread-locals from here. */
    if (arg_run) *arg_run=h->arg_run;
    return 0;
}
static ABI int32_t fiber_get_self(GuestFiber **out) {
    if (!out) return FIBER_NULL;
    HostFiber *h=running;
    if (!h) return FIBER_PERMISSION;
    *out=h->guest;
    return 0;
}
/* SceFiberInfo (128 bytes): size, entry, argOnInitialize, addrContext, sizeContext, name[32],
 * sizeContextMargin (unknown: all ones), reserved. */
static ABI int32_t fiber_get_info(GuestFiber *g, unsigned char *info) {
    if (!g || !info) return FIBER_NULL;
    if (((uintptr_t)g&7) || ((uintptr_t)info&7)) return FIBER_ALIGNMENT;
    uint64_t size;
    memcpy(&size,info,8);
    HostFiber *h=host_of(g);
    if (size!=128 || !h) return FIBER_INVALID;
    uint64_t entry=(uint64_t)(uintptr_t)h->entry, context=(uint64_t)(uintptr_t)h->context, margin=UINT64_MAX;
    memcpy(info+8,&entry,8);
    memcpy(info+16,&h->arg_initialize,8);
    memcpy(info+24,&context,8);
    memcpy(info+32,&h->context_size,8);
    memset(info+40,0,32);
    memcpy(info+40,h->name,strlen(h->name));
    memcpy(info+72,&margin,8);
    return 0;
}
uintptr_t runtime_fiber_resolve(const char *name) {
    static const struct { const char *nid; void *fn; } table[]={
        {"hVYD7Ou2pCQ#libSceFiber",fiber_initialize}, {"asjUJJ+aa8s#libSceFiber",fiber_option_initialize},
        {"JeNX5F-NzQU#libSceFiber",fiber_finalize}, {"a0LLrZWac0M#libSceFiber",fiber_run},
        {"B0ZX2hx9DMw#libSceFiber",fiber_return_to_thread}, {"p+zLIOg27zU#libSceFiber",fiber_get_self},
        {"uq2Y5BFz0PE#libSceFiber",fiber_get_info},
    };
    for (size_t i=0;i<sizeof(table)/sizeof(*table);++i)
        if (!strcmp(name,table[i].nid)) return (uintptr_t)table[i].fn;
    return 0;
}
void runtime_fiber_report(void) {
    pthread_mutex_lock(&lock);
    printf("Runtime: fibers created=%zu, runs=%zu, finalized=%zu\n",created,runs,finalized);
    pthread_mutex_unlock(&lock);
}
