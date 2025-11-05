#include "threadpool.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

static void *worker_main(void *arg) {
    thread_pool_t *pool = (thread_pool_t *)arg;
    for (;;) {
        pthread_mutex_lock(&pool->mutex);
        while (!pool->stop && pool->head == NULL) {
            pthread_cond_wait(&pool->cond, &pool->mutex);
        }
        if (pool->stop) {
            pthread_mutex_unlock(&pool->mutex);
            break;
        }
        thread_job_t *job = pool->head;
        if (job) {
            pool->head = job->next;
            if (pool->head == NULL) {
                pool->tail = NULL;
            }
        }
        pthread_mutex_unlock(&pool->mutex);
        if (job) {
            job->fn(job->arg);
            free(job);
        }
    }
    return NULL;
}

int thread_pool_init(thread_pool_t *pool, size_t worker_count) {
    memset(pool, 0, sizeof(*pool));
    pool->worker_count = worker_count;
    if (pthread_mutex_init(&pool->mutex, NULL) != 0) {
        return -1;
    }
    if (pthread_cond_init(&pool->cond, NULL) != 0) {
        pthread_mutex_destroy(&pool->mutex);
        return -1;
    }
    pool->workers = calloc(worker_count, sizeof(pthread_t));
    if (!pool->workers) {
        pthread_cond_destroy(&pool->cond);
        pthread_mutex_destroy(&pool->mutex);
        return -1;
    }
    for (size_t i = 0; i < worker_count; ++i) {
        if (pthread_create(&pool->workers[i], NULL, worker_main, pool) != 0) {
            pool->stop = 1;
            for (size_t j = 0; j < i; ++j) {
                pthread_cond_broadcast(&pool->cond);
                pthread_join(pool->workers[j], NULL);
            }
            free(pool->workers);
            pthread_cond_destroy(&pool->cond);
            pthread_mutex_destroy(&pool->mutex);
            return -1;
        }
    }
    return 0;
}

void thread_pool_submit(thread_pool_t *pool, thread_job_fn fn, void *arg) {
    thread_job_t *job = calloc(1, sizeof(thread_job_t));
    if (!job) {
        return;
    }
    job->fn = fn;
    job->arg = arg;
    pthread_mutex_lock(&pool->mutex);
    if (pool->tail) {
        pool->tail->next = job;
    } else {
        pool->head = job;
    }
    pool->tail = job;
    pthread_cond_signal(&pool->cond);
    pthread_mutex_unlock(&pool->mutex);
}

void thread_pool_destroy(thread_pool_t *pool) {
    pthread_mutex_lock(&pool->mutex);
    pool->stop = 1;
    pthread_cond_broadcast(&pool->cond);
    pthread_mutex_unlock(&pool->mutex);
    for (size_t i = 0; i < pool->worker_count; ++i) {
        pthread_join(pool->workers[i], NULL);
    }
    thread_job_t *job = pool->head;
    while (job) {
        thread_job_t *next = job->next;
        free(job);
        job = next;
    }
    free(pool->workers);
    pthread_cond_destroy(&pool->cond);
    pthread_mutex_destroy(&pool->mutex);
}
