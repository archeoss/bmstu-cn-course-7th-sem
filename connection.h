#ifndef CONNECTION_H
#define CONNECTION_H

#include "http.h"
#include "server.h"
#include "util.h"

enum connection_state {
  C_VACANT,
  C_RECV_HEADER,
  C_SEND_HEADER,
  C_SEND_BODY,
  NUM_CONN_STATES,
};

struct connection {
  enum connection_state state;
  int fd;
  struct sockaddr_storage ia;
  struct request req;
  struct response res;
  struct buffer buf;
  size_t progress;
};

struct connection *connection_accept(int, struct connection *, size_t);
void connection_log(const struct connection *);
void connection_reset(struct connection *);
void connection_serve(struct connection *, const struct server *);
void new_connect_log(const struct connection *c);

#endif /* CONNECTION_H */
