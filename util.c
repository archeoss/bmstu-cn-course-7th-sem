#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#include "util.h"

char *argv0;

static void verr(const char *fmt, va_list ap) {
  if (argv0 && strncmp(fmt, "usage", sizeof("usage") - 1)) {
    fprintf(stderr, "%s: ", argv0);
  }

  vfprintf(stderr, fmt, ap);

  if (fmt[0] && fmt[strlen(fmt) - 1] == ':') {
    fputc(' ', stderr);
    perror(NULL);
  } else {
    fputc('\n', stderr);
  }
}

void warn(const char *fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  verr(fmt, ap);
  va_end(ap);
}

void die(const char *fmt, ...) {
  va_list ap;

  va_start(ap, fmt);
  verr(fmt, ap);
  va_end(ap);

  exit(1);
}

int timestamp(char *buf, size_t len, time_t t) {
  struct tm tm;

  if (gmtime_r(&t, &tm) == NULL ||
      strftime(buf, len, "%a, %d %b %Y %T GMT", &tm) == 0) {
    return 1;
  }

  return 0;
}

int esnprintf(char *str, size_t size, const char *fmt, ...) {
  va_list ap;
  int ret;

  va_start(ap, fmt);
  ret = vsnprintf(str, size, fmt, ap);
  va_end(ap);

  return (ret < 0 || (size_t)ret >= size);
}

#define INVALID 1
#define TOOSMALL 2
#define TOOLARGE 3

long long strtonum(const char *numstr, long long minval, long long maxval,
                   const char **errstrp) {
  long long ll = 0;
  int error = 0;
  char *ep;
  struct errval {
    const char *errstr;
    int err;
  } ev[4] = {
      {NULL, 0},
      {"invalid", EINVAL},
      {"too small", ERANGE},
      {"too large", ERANGE},
  };

  ev[0].err = errno;
  errno = 0;
  if (minval > maxval) {
    error = INVALID;
  } else {
    ll = strtoll(numstr, &ep, 10);
    if (numstr == ep || *ep != '\0')
      error = INVALID;
    else if ((ll == LLONG_MIN && errno == ERANGE) || ll < minval)
      error = TOOSMALL;
    else if ((ll == LLONG_MAX && errno == ERANGE) || ll > maxval)
      error = TOOLARGE;
  }
  if (errstrp != NULL)
    *errstrp = ev[error].errstr;
  errno = ev[error].err;
  if (error)
    ll = 0;

  return ll;
}

#define MUL_NO_OVERFLOW ((size_t)1 << (sizeof(size_t) * 4))

void *reallocarray(void *optr, size_t nmemb, size_t size) {
  if ((nmemb >= MUL_NO_OVERFLOW || size >= MUL_NO_OVERFLOW) && nmemb > 0 &&
      SIZE_MAX / nmemb < size) {
    errno = ENOMEM;
    return NULL;
  }
  return realloc(optr, size * nmemb);
}

int buffer_appendf(struct buffer *buf, const char *suffixfmt, ...) {
  va_list ap;
  int ret;

  va_start(ap, suffixfmt);
  ret = vsnprintf(buf->data + buf->len, sizeof(buf->data) - buf->len, suffixfmt,
                  ap);
  va_end(ap);

  if (ret < 0 || (size_t)ret >= (sizeof(buf->data) - buf->len)) {
    /* truncation occured*/
    memset(buf->data + buf->len, 0, sizeof(buf->data) - buf->len);
    return 1;
  }

  /* increase buffer length by number of bytes written */
  buf->len += ret;

  return 0;
}
