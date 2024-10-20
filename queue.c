#include "queue.h"
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>

struct queue {
    int size;
    int in;
    int out;
    int count;
    void **buf;
    pthread_mutex_t mutex;
    pthread_cond_t empty;
    pthread_cond_t full;
};
queue_t *queue_new(int size) {
    if (size <= 0)
        return NULL;
    queue_t *q = (queue_t *) malloc(sizeof(queue_t));
    q->buf = (void **) malloc(size * sizeof(void *));
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->empty, NULL);
    pthread_cond_init(&q->full, NULL);
    q->size = size;
    q->in = 0;
    q->out = 0;
    q->count = 0;
    return q;
}

void queue_delete(queue_t **q) {
    pthread_mutex_destroy(&(*q)->mutex);
    pthread_cond_destroy(&(*q)->empty);
    pthread_cond_destroy(&(*q)->full);
    free((*q)->buf);
    (*q)->buf = NULL;
    free(*q);
    *q = NULL;
}

bool queue_push(queue_t *q, void *elem) {
    if (q == NULL) {
        return (false);
    }
    pthread_mutex_lock(&q->mutex);
    while (q->count == q->size) {
        pthread_cond_wait(&q->empty, &q->mutex);
    }
    q->buf[q->in] = elem;
    q->in = (q->in + 1) % (q->size);
    q->count++;
    pthread_cond_signal(&q->full);
    pthread_mutex_unlock(&q->mutex);
    return true;
}
bool queue_pop(queue_t *q, void **elem) {
    if (q == NULL) {
        return (false);
    }
    pthread_mutex_lock(&q->mutex);
    while (q->count == 0) {
        pthread_cond_wait(&q->full, &q->mutex);
    }
    *elem = q->buf[q->out];
    q->out = (q->out + 1) % q->size;
    q->count--;
    pthread_cond_signal(&q->empty);
    pthread_mutex_unlock(&q->mutex);
    return true;
}
