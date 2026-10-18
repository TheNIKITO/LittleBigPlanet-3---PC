/* Modules the game loads itself (sceKernelLoadStartModule): link_modules.py linked them into the
 * image with the rest, the loader registers them here, and loading one runs its initializer
 * then, with the game's arguments. A module is found by its file name, wherever the game says
 * it lies; loading it again returns the same handle without starting it twice. */
#include "runtime.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#define ERR(n) ((int32_t)(UINT32_C(0x80020000)|(n)))
#define MAX_MODULES 16
#define FIRST_HANDLE 0x20 /* handles of the system modules come before */

typedef int32_t (ABI *ModuleStart)(uint64_t args, const void *argp, void *param);
static struct { char name[64]; ModuleStart start; int started; } modules[MAX_MODULES];
static int count;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;

void runtime_module_add(const char *name, void *start) {
    if (count==MAX_MODULES) { fputs("STOP: too many game modules\n",stderr); exit(21); }
    snprintf(modules[count].name,sizeof(modules[count].name),"%s",name);
    modules[count].start=(ModuleStart)start;
    ++count;
}
static int same_name(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b) if (tolower((unsigned char)*a)!=tolower((unsigned char)*b)) return 0;
    return !*a && !*b;
}
static ABI int32_t load_start(const char *path, uint64_t args, const void *argp, uint32_t flags,
                              const void *options, int32_t *result) {
    (void)flags; (void)options;
    if (!path) return ERR(22);
    const char *file=path;
    for (const char *p=path; *p; ++p) if (*p=='/' || *p=='\\') file=p+1;
    pthread_mutex_lock(&lock);
    int i=0;
    while (i<count && !same_name(modules[i].name,file)) ++i;
    if (i==count) {
        pthread_mutex_unlock(&lock);
        printf("Runtime: module %s is not linked into the image; load refused\n",path);
        return ERR(2); /* ENOENT */
    }
    int start=!modules[i].started;
    modules[i].started=1;
    pthread_mutex_unlock(&lock);
    if (!start) {
        if (result) *result=0;
        return FIRST_HANDLE+i;
    }
    printf("Runtime: starting game module %s (%s)\n",modules[i].name,path);
    int32_t status=modules[i].start(args,argp,NULL);
    printf("Runtime: module %s start returned %d\n",modules[i].name,status);
    if (result) *result=status;
    return FIRST_HANDLE+i;
}
uintptr_t runtime_module_resolve(const char *name) {
    if (!strcmp(name,"wzvqT4UqKX8#libkernel")) return (uintptr_t)load_start;
    return 0;
}
