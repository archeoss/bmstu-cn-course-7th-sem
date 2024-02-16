#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <sys/types.h>

#include "queue.h"
#include "util.h"

struct pollfd *queue_create(size_t max_clients, size_t *clients) {
  struct pollfd *pollfds = NULL;
  if (!(pollfds = calloc(max_clients, sizeof(struct pollfd)))) {
    die("calloc:");
  }
  *clients = 0;

  return pollfds;
}

int queue_add_fd(struct pollfd **pollfds, int fd, enum queue_event_type t,
                 size_t *max_clients, size_t *clients, const void *data,
                 queue **queue) {
  if (*clients == *max_clients) {
    *max_clients *= 2;
    if (!(*pollfds =
              reallocarray(*pollfds, *max_clients, sizeof(struct pollfd)))) {
      die("reallocarray:");
    }
  }

  switch (t) {
  case QUEUE_EVENT_IN:
    (*pollfds)[*clients].fd = fd;
    (*pollfds)[*clients].events = POLLIN;
    break;
  case QUEUE_EVENT_OUT:
    (*pollfds)[*clients].fd = fd;
    (*pollfds)[*clients].events = POLLOUT;
    break;
  }
  (*clients)++;

  enqueue(queue, data, fd);

  return 0;
}

// int queue_mod_fd(struct pollfd **pollfds, int fd, enum queue_event_type t,
int queue_mod_fd(struct pollfd **pollfds, int i, enum queue_event_type t,
                 size_t *_max_clients, size_t *_clients, const void *data,
                 queue *queue) {
  // for (size_t i = 0; i < *clients; i++) {
  //   if ((*pollfds)[i].fd == fd) {
  //     dequeue_by_fd(queue, fd);
  //     enqueue(queue, data, fd);
  insert(queue, data, i);
  switch (t) {
  case QUEUE_EVENT_IN:
    (*pollfds)[i].events = POLLIN;
    (*pollfds)[i].revents = 0;
  case QUEUE_EVENT_OUT:
    (*pollfds)[i].events = POLLOUT;
    (*pollfds)[i].revents = 0;
  }
  return 0;
  //   }
  // }
  // queue_rem_fd(*pollfds, fd, clients, queue);
  // return queue_add_fd(pollfds, fd, t, max_clients, clients, data, queue);
  // return 1;
}

// int queue_rem_fd(struct pollfd *pollfds, int fd, size_t *clients,
int queue_rem_fd(struct pollfd *pollfds, int i, size_t *clients, queue *queue) {
  if (!*clients)
    return 1;

  // for (long int i = 0; i < *clients; i++) {
  // if (pollfds[i].fd == fd) {
  pollfds[i] = pollfds[--(*clients)];
  pollfds[*clients] = (const struct pollfd){0};
  // dequeue_by_fd(queue, fd);
  dequeue(queue, i);
  // break;
  // }
  // }
  return 0;
}

ssize_t queue_wait(struct pollfd *fds, size_t clients) {
  ssize_t nready;

  if ((nready = poll(fds, clients, -1)) < 0) {
    warn("poll:");
    return -1;
  }

  return nready;
}

// queue_event *queue_event_get_data(queue *e, int fd) {
queue_event *queue_event_get_data(queue *e, int i) {
  // pthread_mutex_lock(&e->lock);
  return &e->arr[i];
  // queue_event *ev = e->head;
  // while (ev) {
  //   if (ev->fd == fd) {
  //     // pthread_mutex_unlock(&e->lock);
  //     return ev;
  //   }
  //   ev = ev->next;
  // }
  // warn("null ret");
  // pthread_mutex_unlock(&e->lock);
  // return NULL;
}

int queue_event_is_error(struct pollfd *pollfd) {
  short revents = (*pollfd).revents;
  // (*pollfd).revents = 0;
  return (revents & ~(POLLIN | POLLOUT)) ? 1 : 0;
}

queue *create_queue_event(int capacity) {
  queue *q = malloc(sizeof(queue));
  q->capacity = capacity;
  q->size = 0;
  q->arr = calloc(capacity, sizeof(queue_event));
  // if (pthread_mutex_init(&(q->lock), NULL) != 0) {
  //   die("Error Initialising mutex lock\n");
  //   free(q);
  //   q = NULL;
  //   return NULL;
  // }

  return q;
}
//
// int enqueue(queue *q, const void *data, int fd) {
//   if (q->size == q->capacity) {
//     return -1;
//   }
//   queue_event *node = malloc(sizeof(queue_event));
//   node->fd = fd;
//   node->ptr = (void *)data;
//   node->next = NULL;
//   if (q->size == 0) {
//     q->head = node;
//     q->tail = node;
//   } else {
//     q->tail->next = node;
//     q->tail = node;
//   }
//   q->size++;
//   return 1;
// }
//
// int dequeue(queue *q) {
//   if (q->size == 0)
//     return 0;
//
//   int retval = q->head->fd;
//   if (q->size == 1) {
//     free(q->head);
//     q->head = NULL;
//     q->tail = NULL;
//   } else {
//     queue_event *oldhead = q->head;
//     q->head = oldhead->next;
//     free(oldhead);
//     oldhead = NULL;
//   }
//   q->size--;
//   return retval;
// }
//
// queue_event *inspect(queue_event *q, size_t i) {
//   if (i == 0)
//     return q;
//   if (!q) {
//     return q;
//   }
//   return (inspect(q->next, i - 1));
// }
//
// int dequeue_node_by_fd(queue_event *head, queue_event **tail, int fd) {
//   if (!head->next) {
//     return 0;
//   }
//   int found_fd = head->next->fd;
//   if (found_fd == fd) {
//     queue_event *ret = head->next;
//     head->next = head->next->next;
//     free(ret);
//     if (head->next == NULL) {
//       *tail = head;
//     }
//     return fd;
//   }
//   return dequeue_node_by_fd(head->next, tail, fd);
// }
//
// int dequeue_by_fd(queue *q, int fd) {
//   if (q->size == 0)
//     return 0;
//   if (q->head->fd == fd) {
//     return dequeue(q);
//   }
//   int res;
//   if ((res = dequeue_node_by_fd(q->head, &q->tail, fd)) != 0) {
//     q->size--;
//     if (!q->size) {
//       free(q->head);
//       q->head = NULL;
//       q->tail = NULL;
//     }
//   }
//   return res;
// }
//
// void freequeue(queue *q) {
//   queue_event *cursor = q->head, *temp;
//   while (cursor != NULL) {
//     temp = cursor;
//     cursor = cursor->next;
//     free(temp);
//   }
//   pthread_mutex_destroy(&(q->lock));
//   free(q);
//
//   return;
// }

/* Add a new file descriptor to the set */
void enqueue(queue **q, const void *data, int fd) {
  /* If we don't have room, add more space in the qfd array */
  // pthread_mutex_lock(&(*q)->lock);
  if ((*q)->size == (*q)->capacity) {
    (*q)->capacity *= 2; // Double it

    *q = realloc(*q, sizeof(**q) * ((*q)->capacity));
  }

  (*q)->arr[(*q)->size].fd = fd;
  (*q)->arr[(*q)->size].ptr = (void *)data; // Check ready-to-read
  (*q)->size++;
  // pthread_mutex_unlock(&(*q)->lock);
}

// Remove an index from the set
void dequeue(queue *q, int i) {
  // pthread_mutex_lock(&q->lock);
  // Copy the one from the end over this one
  q->arr[i] = q->arr[--(q->size)];
  q->arr[(q->size)] = (const queue_event){0};
  // pthread_mutex_unlock(&q->lock);
}

// Add a new file descriptor to the set
void insert(queue *q, const void *data, int i) {
  // pthread_mutex_lock(&q->lock);
  q->arr[i].ptr = (void *)data; // Check ready-to-read
  // pthread_mutex_unlock(&q->lock);
}
