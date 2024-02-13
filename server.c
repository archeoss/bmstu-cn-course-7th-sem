#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "connection.h"
#include "queue.h"
#include "server.h"
#include "util.h"

struct worker_data {
  int insock;
  size_t nslots;
  const struct server *srv;
  // queue *q;
};

static void *server_worker(void *data) {
  struct connection *connection, *c, *newc;
  struct worker_data *d = (struct worker_data *)data;
  struct pollfd *qfd;
  size_t clients, max_clients = 64;
  queue *event = create_queue_event(d->nslots);
  // queue *event = d->q;

  /* allocate connections */
  if (!(connection = calloc(d->nslots, sizeof(*connection)))) {
    die("calloc:");
  }

  /* create event queue */
  if ((qfd = queue_create(max_clients, &clients)) == NULL) {
    exit(1);
  }

  /* add insock to the interest list (with data=NULL) */
  if (queue_add_fd(&qfd, d->insock, QUEUE_EVENT_IN, &max_clients, &clients,
                   NULL, &event) < 0) {
    exit(1);
  }

  for (;;) {
    int poll_count = queue_wait(qfd, clients);
    // Run through the existing connections looking for data to read
    for (size_t i = 0; i < clients; i++) {
      queue_event *ev = queue_event_get_data(event, i);
      c = ev->ptr;
      if (ev == NULL || ev->ptr == NULL) { // We got one!!

        if (!(newc = connection_accept(d->insock, connection, d->nslots))) {
          continue;
        }

        if (queue_add_fd(&qfd, newc->fd, QUEUE_EVENT_IN, &max_clients, &clients,
                         newc, &event) < 0) {
          continue;
        }
        new_connect_log(newc);
      } else {
        c = ev->ptr;
        connection_serve(c, d->srv);

        if (c->fd == 0) {
          memset(c, 0, sizeof(struct connection));
          queue_rem_fd(qfd, i, &clients, event);
          continue;
        }

        switch (c->state) {
        case C_RECV_HEADER:
          if (queue_mod_fd(&qfd, i, QUEUE_EVENT_IN, &max_clients, &clients, c,
                           event) < 0) {
            connection_reset(c);
            break;
          }
          break;
        case C_SEND_HEADER:
        case C_SEND_BODY:
          if (queue_mod_fd(&qfd, i, QUEUE_EVENT_OUT, &max_clients, &clients, c,
                           event) < 0) {
            connection_reset(c);
            break;
          }
          break;
        default:
          break;
        }
      }
    }
  }

  return NULL;
}

void server_init_thread_pool(int insock, size_t nthreads, size_t nslots,
                             const struct server *srv) {
  pthread_t *thread = NULL;
  struct worker_data *d = NULL;
  size_t i;

  if (!(d = reallocarray(d, nthreads, sizeof(*d)))) {
    die("reallocarray:");
  }

  // queue *event = create_queue_event(d->nslots);
  for (i = 0; i < nthreads; i++) {
    d[i].insock = insock;
    d[i].nslots = nslots;
    d[i].srv = srv;
    // d[i].q = event;
  }

  if (!(thread = reallocarray(thread, nthreads, sizeof(*thread)))) {
    die("reallocarray:");
  }
  for (i = 0; i < nthreads; i++) {
    if (pthread_create(&thread[i], NULL, server_worker, &d[i]) != 0) {
      die("pthread_create:");
    }
  }

  for (i = 0; i < nthreads; i++) {
    if ((errno = pthread_join(thread[i], NULL))) {
      warn("pthread_join:");
    }
  }
}
