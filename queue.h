#ifndef QUEUE_H
#define QUEUE_H

#include <stddef.h>
#include <sys/time.h>
#include <sys/types.h>

typedef struct queue_ev {
  void *ptr;
  int fd;
} queue_event;

struct queue {
  size_t size;
  size_t capacity;
  queue_event *arr;
  // pthread_mutex_t lock;
};

enum queue_event_type {
  QUEUE_EVENT_IN,
  QUEUE_EVENT_OUT,
};

typedef struct queue queue;
struct pollfd *queue_create(size_t max_clients, size_t *clients);
int queue_add_fd(struct pollfd **pollfds, int fd, enum queue_event_type t,
                 size_t *max_clients, size_t *clients, const void *data,
                 queue **queue);
int queue_mod_fd(struct pollfd **pollfds, int fd, enum queue_event_type t,
                 size_t *max_clients, size_t *clients, const void *data,
                 queue *queue);
int queue_rem_fd(struct pollfd *pollfds, int fd, size_t *clients, queue *queue);
ssize_t queue_wait(struct pollfd *fds, size_t clients);

queue_event *queue_event_get_data(queue *, int fd);

int queue_event_is_error(struct pollfd *pollfd);

void enqueue(queue **q, const void *data, int fd);
void dequeue(queue *q, int i);
void insert(queue *q, const void *data, int i);
queue *create_queue_event(int capacity);

#endif /* QUEUE_H */
