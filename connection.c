#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "connection.h"
#include "data.h"
#include "http.h"
#include "server.h"
#include "sock.h"
#include "util.h"

struct worker_data {
  int insock;
  size_t nslots;
  const struct server *srv;
};

void connection_log(const struct connection *c) {
  char inaddr_str[INET6_ADDRSTRLEN /* > INET_ADDRSTRLEN */];
  char tstmp[21];

  /* timestamp */
  if (!strftime(tstmp, sizeof(tstmp), "%Y-%m-%dT%H:%M:%SZ",
                gmtime(&(time_t){time(NULL)}))) {
    warn("strftime: Exceeded buffer capacity");
    tstmp[0] = '\0';
  }

  /* address */
  if (sock_get_inaddr_str(&c->ia, inaddr_str, LEN(inaddr_str))) {
    warn("sock_get_inaddr_str: Couldn't generate adress-string");
    inaddr_str[0] = '\0';
  }

  if (c->res.status != 0) {
    printf("%s\t%s\t%s%.*d\t%s\t%s%s%s%s%s\n", tstmp, inaddr_str,
           (c->res.status == 0) ? "dropped" : "", (c->res.status == 0) ? 0 : 3,
           c->res.status,
           c->req.field[REQ_HOST][0] ? c->req.field[REQ_HOST] : "-",
           c->req.path[0] ? c->req.path : "-", c->req.query[0] ? "?" : "",
           c->req.query, c->req.fragment[0] ? "#" : "", c->req.fragment);
  }
}

void *get_in_addr(struct sockaddr *sa) {
  if (sa->sa_family == AF_INET) {
    return &(((struct sockaddr_in *)sa)->sin_addr);
  }

  return &(((struct sockaddr_in6 *)sa)->sin6_addr);
}

void new_connect_log(const struct connection *c) {
  // struct sockaddr_storage remoteaddr; // Client address
  char remoteIP[INET6_ADDRSTRLEN];
  // char buf[256]; // Buffer for client data
  printf("pollserver: new connection from %s on "
         "socket %d\n",
         inet_ntop(c->ia.ss_family,
                   // get_in_addr((struct sockaddr *)&remoteaddr),
                   get_in_addr((struct sockaddr *)&c->ia), remoteIP,
                   INET6_ADDRSTRLEN),
         c->fd);
}

void connection_reset(struct connection *c) {
  if (c != NULL) {
    shutdown(c->fd, SHUT_RDWR);
    close(c->fd);
    memset(c, 0, sizeof(*c));
  }
}

void connection_serve(struct connection *c, const struct server *srv) {
  enum status s;
  int done;
  switch (c->state) {
  case C_VACANT:
    // warn("C_VACANT");
    memset(&c->buf, 0, sizeof(c->buf));

    c->state = C_RECV_HEADER;
    /* fallthrough */
  case C_RECV_HEADER:
    // warn("C_RECV_HEADER");
    done = 0;
    if ((s = http_recv_header(c->fd, &c->buf, &done))) {
      http_prepare_error_response(&c->req, &c->res, s);
    } else {
      if (!done) {
        return;
      }

      if ((s = http_parse_header(c->buf.data, &c->req))) {
        // warn("HERE");
        http_prepare_error_response(&c->req, &c->res, s);
      } else {

        http_prepare_response(&c->req, &c->res, srv);
      }
    }
    if ((s = http_prepare_header_buf(&c->res, &c->buf))) {
      http_prepare_error_response(&c->req, &c->res, s);
      if ((s = http_prepare_header_buf(&c->res, &c->buf))) {
        c->res.status = s;
        connection_log(c);
        connection_reset(c);
        return;
      }
    }

    c->state = C_SEND_HEADER;
    /* fallthrough */
  case C_SEND_HEADER:
    // warn("C_SEND_HEADER");
    if ((s = http_send_buf(c->fd, &c->buf))) {
      c->res.status = s;
      connection_log(c);
      connection_reset(c);
      return;
    }
    if (c->buf.len > 0) {
      return;
    }

    c->state = C_SEND_BODY;
    /* fallthrough */
  case C_SEND_BODY:
    // warn("C_SEND_BODY");
    if (c->req.method == M_GET) {
      if (c->buf.len == 0) {
        // warn("before data_fct");
        if ((s = data_fct[c->res.type](&c->res, &c->buf, &c->progress))) {
          c->res.status = s;
          connection_log(c);
          connection_reset(c);
          return;
        }
        // warn("after data_fct");

        if (c->buf.len == 0) {
          // warn("buf 0");
          break;
        }
      } else {
        // warn("before send buf");
        if ((s = http_send_buf(c->fd, &c->buf))) {
          c->res.status = s;
          connection_log(c);
          connection_reset(c);
          return;
        }
        // warn("after send buf");
      }
      // warn("C_SEND_BODY end");
      return;
    }
    // warn("C_SEND_BODY end");
    break;
  default:
    warn("serve: invalid connection state");
    return;
  }
  connection_log(c);
  connection_reset(c);
}

static struct connection *
connection_get_drop_candidate(struct connection *connection, size_t nslots) {
  struct connection *c, *minc;
  size_t i, j, maxcnt, cnt;

  for (i = 0, minc = NULL, maxcnt = 0; i < nslots; i++) {
    c = &connection[i];
    for (j = 0, cnt = 0; j < nslots; j++) {
      if (!sock_same_addr(&connection[i].ia, &connection[j].ia)) {
        continue;
      }
      cnt++;
      if (connection[j].state < c->state) {
        c = &connection[j];
      } else if (connection[j].state == c->state) {
        if (c->state == C_SEND_BODY && connection[i].res.type != c->res.type) {
          if (connection[i].res.type < c->res.type) {
            c = &connection[j];
          }
        } else if (connection[j].progress < c->progress) {
          c = &connection[j];
        }
      }
    }

    if (cnt > maxcnt) {
      minc = c;
      maxcnt = cnt;
    }
  }

  return minc;
}

struct connection *connection_accept(int insock, struct connection *connection,
                                     size_t nslots) {
  struct connection *c = NULL;
  size_t i;

  for (i = 0; i < nslots; i++) {
    if (connection[i].fd == 0) {
      c = &connection[i];
      break;
    }
  }
  if (i == nslots) {
    c = connection_get_drop_candidate(connection, nslots);
    c->res.status = 0;
    // connection_log(c);
    connection_reset(c);
  }

  if ((c->fd = accept(insock, (struct sockaddr *)&c->ia,
                      &(socklen_t){sizeof(c->ia)})) < 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      warn("accept:");
    }
    return NULL;
  }

  if (sock_set_nonblocking(c->fd)) {
    return NULL;
  }
  return c;
}
