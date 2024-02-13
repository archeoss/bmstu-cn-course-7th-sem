#include <grp.h>
#include <pwd.h>
#include <regex.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "arg.h"
#include "server.h"
#include "sock.h"
#include "util.h"

static void usage(void) {
  const char *opts = "[-d dir] "
                     "[-i file] ...";

  die("usage: %s -p port [-h host] %s\n", argv0, opts);
}

int main(int argc, char *argv[]) {
  struct rlimit rlim;
  struct server srv = {
      .docindex = "index.html",
  };
  int insock, status = 0;

  /* defaults */
  size_t nthreads = 8;
  size_t nslots = 20;
  char *servedir = ".";

  ARGBEGIN {
  case 'd':
    servedir = EARGF(usage());
    break;
  case 'h':
    srv.host = EARGF(usage());
    break;
  case 'i':
    srv.docindex = EARGF(usage());
    if (strchr(srv.docindex, '/')) {
      die("The document index must not contain '/'");
    }
    break;
  case 'p':
    srv.port = EARGF(usage());
    break;
  default:
    usage();
  }
  ARGEND

  if (argc) {
    usage();
  }

  rlim.rlim_cur = rlim.rlim_max =
      3 + nthreads + nthreads * nslots + 5 * nthreads;

  insock = sock_get_ips(srv.host, srv.port);
  if (sock_set_nonblocking(insock)) {
    return 1;
  }
  /* chroot */
  if (chdir(servedir) < 0) {
    die("chdir '%s':", servedir);
  }
  if (chroot(".") < 0) {
    die("chroot:");
  }

  /* accept incoming connections */
  server_init_thread_pool(insock, nthreads, nslots, &srv);

  // exit(0);
  while (wait(&status) > 0)
    ;

  return status;
}
