#pragma once
#include <pthread.h>
#include <assert.h>
typedef pthread_mutex_t StaticSemaphore_t;
typedef pthread_mutex_t *SemaphoreHandle_t;
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffu
#define configASSERT(x) assert(x)
static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t *m)
{
    pthread_mutexattr_t a;
    assert(!pthread_mutexattr_init(&a));
    assert(!pthread_mutexattr_settype(&a,PTHREAD_MUTEX_RECURSIVE));
    assert(!pthread_mutex_init(m,&a));
    assert(!pthread_mutexattr_destroy(&a));
    return m;
}
static inline int xSemaphoreTakeRecursive(SemaphoreHandle_t m, unsigned timeout)
{ return (timeout ? pthread_mutex_lock(m) : pthread_mutex_trylock(m))==0; }
static inline int xSemaphoreGiveRecursive(SemaphoreHandle_t m)
{ return pthread_mutex_unlock(m)==0; }
