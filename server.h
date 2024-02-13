/* See LICENSE file for copyright and license details. */
#ifndef SERVER_H
#define SERVER_H

#include <regex.h>
#include <stddef.h>

struct server {
  char *host;
  char *port;
  char *docindex;
  int listdirs;
};

void server_init_thread_pool(int, size_t, size_t, const struct server *);

#endif /* SERVER_H */
