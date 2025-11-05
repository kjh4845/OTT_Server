#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>
#include <stddef.h>

typedef void (*thread_job_fn)(void *arg);

typedef struct thread_job {
    thread_job_fn fn;
    void *arg;
    struct thread_job *next;
} thread_job_t;

typedef struct thread_pool {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    thread_job_t *head;
    thread_job_t *tail;
    size_t worker_count;
    int stop;
    pthread_t *workers;
} thread_pool_t;

int thread_pool_init(thread_pool_t *pool, size_t worker_count);
void thread_pool_submit(thread_pool_t *pool, thread_job_fn fn, void *arg);
void thread_pool_destroy(thread_pool_t *pool);

#endif
