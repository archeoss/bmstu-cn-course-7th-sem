#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <regex.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "http.h"
#include "util.h"

const char *req_field_str[] = {
    [REQ_HOST] = "Host",
    [REQ_RANGE] = "Range",
    [REQ_IF_MODIFIED_SINCE] = "If-Modified-Since",
};

const char *req_method_str[] = {
    [M_GET] = "GET",
    [M_HEAD] = "HEAD",
};

const char *status_str[] = {
    [S_OK] = "OK",
    [S_PARTIAL_CONTENT] = "Partial Content",
    [S_NOT_MODIFIED] = "Not Modified",
    [S_BAD_REQUEST] = "Bad Request",
    [S_FORBIDDEN] = "Forbidden",
    [S_NOT_FOUND] = "Not Found",
    [S_METHOD_NOT_ALLOWED] = "Method Not Allowed",
};

const char *res_field_str[] = {
    [RES_ALLOW] = "Allow",
    [RES_LOCATION] = "Location",
    [RES_LAST_MODIFIED] = "Last-Modified",
    [RES_CONTENT_LENGTH] = "Content-Length",
    [RES_CONTENT_RANGE] = "Content-Range",
    [RES_CONTENT_TYPE] = "Content-Type",
};

enum status http_prepare_header_buf(const struct response *res,
                                    struct buffer *buf) {
  char tstmp[FIELD_MAX];
  size_t i;

  memset(buf, 0, sizeof(*buf));

  if (timestamp(tstmp, sizeof(tstmp), time(NULL))) {
    memset(buf, 0, sizeof(*buf));
    return S_INTERNAL_SERVER_ERROR;
  }
  if (buffer_appendf(buf,
                     "HTTP/1.1 %d %s\r\n"
                     "Date: %s\r\n"
                     "Connection: close\r\n",
                     res->status, status_str[res->status], tstmp)) {
    memset(buf, 0, sizeof(*buf));
    return S_INTERNAL_SERVER_ERROR;
  }
  for (i = 0; i < NUM_RES_FIELDS; i++) {
    if (res->field[i][0] != '\0' &&
        buffer_appendf(buf, "%s: %s\r\n", res_field_str[i], res->field[i])) {

      memset(buf, 0, sizeof(*buf));
      return S_INTERNAL_SERVER_ERROR;
    }
  }

  if (buffer_appendf(buf, "\r\n")) {
    memset(buf, 0, sizeof(*buf));
    return S_INTERNAL_SERVER_ERROR;
  }

  return 0;
}

enum status http_send_buf(int fd, struct buffer *buf) {
  ssize_t r;

  if (buf == NULL) {
    return S_INTERNAL_SERVER_ERROR;
  }

  while (buf->len > 0) {
    if ((r = write(fd, buf->data, buf->len)) <= 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0;
      } else {
        return S_REQUEST_TIMEOUT;
      }
    }
    memmove(buf->data, buf->data + r, buf->len - r);
    buf->len -= r;
  }

  return 0;
}

static void decode(const char src[PATH_MAX], char dest[PATH_MAX]) {
  size_t i;
  uint8_t n;
  const char *s;

  for (s = src, i = 0; *s; i++) {
    if (*s == '%' && isxdigit((unsigned char)s[1]) &&
        isxdigit((unsigned char)s[2])) {
      sscanf(s + 1, "%2hhx", &n);
      dest[i] = n;
      s += 3;
    } else {
      dest[i] = *s++;
    }
  }
  dest[i] = '\0';
}

enum status http_recv_header(int fd, struct buffer *buf, int *done) {
  enum status s;
  ssize_t r;

  while (1) {
    if ((r = read(fd, buf->data + buf->len, sizeof(buf->data) - buf->len)) <
        0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        *done = 0;
        return 0;
      } else {
        s = S_REQUEST_TIMEOUT;

        memset(buf, 0, sizeof(*buf));
        return s;
      }
    } else if (r == 0) {
      s = S_BAD_REQUEST;
      memset(buf, 0, sizeof(*buf));
      return s;
    }
    buf->len += r;

    if (buf->len >= 4 && !memcmp(buf->data + buf->len - 4, "\r\n\r\n", 4)) {
      break;
    }

    if (r == 0 || buf->len == sizeof(buf->data)) {
      s = S_REQUEST_TOO_LARGE;
      memset(buf, 0, sizeof(*buf));
      return s;
    }
  }

  buf->len -= 2;
  *done = 1;

  return 0;
}

enum status http_parse_header(const char *h, struct request *req) {
  size_t i, mlen;
  const char *p, *q, *r, *s, *t;

  memset(req, 0, sizeof(*req));

  for (i = 0; i < NUM_REQ_METHODS; i++) {
    mlen = strlen(req_method_str[i]);
    if (!strncmp(req_method_str[i], h, mlen)) {
      req->method = i;
      break;
    }
  }
  if (i == NUM_REQ_METHODS) {
    return S_METHOD_NOT_ALLOWED;
  }

  if (h[mlen] != ' ') {
    return S_BAD_REQUEST;
  }

  p = h + mlen + 1;
  /*
   * path?query#fragment
   * ^   ^     ^        ^
   * |   |     |        |
   * p   r     s        q
   *
   */
  if (!(q = strchr(p, ' '))) {
    return S_BAD_REQUEST;
  }

  /* search for first '?' */
  for (r = p; r < q; r++) {
    if (!isprint(*r)) {
      return S_BAD_REQUEST;
    }
    if (*r == '?') {
      break;
    }
  }
  if (r == q) {
    /* not found */
    r = NULL;
  }

  /* search for first '#' */
  for (s = p; s < q; s++) {
    if (!isprint(*s)) {
      return S_BAD_REQUEST;
    }
    if (*s == '#') {
      break;
    }
  }
  if (s == q) {
    /* not found */
    s = NULL;
  }

  if (r != NULL && s != NULL && s < r) {
    /*
     * '#' comes before '?' and thus the '?' is literal,
     * because the query must come before the fragment
     */
    r = NULL;
  }

  /* write path using temporary endpointer t */
  if (r != NULL) {
    /* resource contains a query, path ends at r */
    t = r;
  } else if (s != NULL) {
    /* resource contains only a fragment, path ends at s */
    t = s;
  } else {
    /* resource contains no queries, path ends at q */
    t = q;
  }

  if ((size_t)(t - p + 1) > LEN(req->path)) {
    return S_REQUEST_TOO_LARGE;
  }
  memcpy(req->path, p, t - p);
  req->path[t - p] = '\0';
  decode(req->path, req->path);

  if (r != NULL) {
    t = (s != NULL) ? s : q;

    if ((size_t)(t - (r + 1) + 1) > LEN(req->query)) {
      return S_REQUEST_TOO_LARGE;
    }
    memcpy(req->query, r + 1, t - (r + 1));
    req->query[t - (r + 1)] = '\0';
  }

  if (s != NULL) {
    if ((size_t)(q - (s + 1) + 1) > LEN(req->fragment)) {
      return S_REQUEST_TOO_LARGE;
    }
    memcpy(req->fragment, s + 1, q - (s + 1));
    req->fragment[q - (s + 1)] = '\0';
  }

  p = q + 1;

  if (strncmp(p, "HTTP/", sizeof("HTTP/") - 1)) {
    return S_BAD_REQUEST;
  }
  p += sizeof("HTTP/") - 1;
  if (strncmp(p, "1.0", sizeof("1.0") - 1) &&
      strncmp(p, "1.1", sizeof("1.1") - 1)) {
    return S_VERSION_NOT_SUPPORTED;
  }
  p += sizeof("1.*") - 1;

  if (strncmp(p, "\r\n", sizeof("\r\n") - 1)) {
    return S_BAD_REQUEST;
  }

  p += sizeof("\r\n") - 1;

  for (; *p != '\0';) {
    for (i = 0; i < NUM_REQ_FIELDS; i++) {
      if (!strncasecmp(p, req_field_str[i], strlen(req_field_str[i]))) {
        break;
      }
    }
    if (i == NUM_REQ_FIELDS) {
      if (!(q = strstr(p, "\r\n"))) {
        return S_BAD_REQUEST;
      }
      p = q + (sizeof("\r\n") - 1);
      continue;
    }

    p += strlen(req_field_str[i]);

    if (*p != ':') {
      return S_BAD_REQUEST;
    }

    for (++p; *p == ' ' || *p == '\t'; p++)
      ;

    if (!(q = strstr(p, "\r\n"))) {
      return S_BAD_REQUEST;
    }
    if ((size_t)(q - p + 1) > LEN(req->field[i])) {
      return S_REQUEST_TOO_LARGE;
    }
    memcpy(req->field[i], p, q - p);
    req->field[i][q - p] = '\0';

    p = q + (sizeof("\r\n") - 1);
  }

  return 0;
}

static void encode(const char src[PATH_MAX], char dest[PATH_MAX]) {
  size_t i;
  const char *s;

  for (s = src, i = 0; *s && i < (PATH_MAX - 4); s++) {
    if (iscntrl(*s) || (unsigned char)*s > 127) {
      i += snprintf(dest + i, PATH_MAX - i, "%%%02X", (unsigned char)*s);
    } else {
      dest[i] = *s;
      i++;
    }
  }
  dest[i] = '\0';
}

static enum status path_ensure_dirslash(char uri[PATH_MAX], int *redirect) {
  size_t len;

  /* append '/' to URI if not present */
  len = strlen(uri);
  if (len + 1 + 1 > PATH_MAX) {
    return S_REQUEST_TOO_LARGE;
  }
  if (len > 0 && uri[len - 1] != '/') {
    uri[len] = '/';
    uri[len + 1] = '\0';
    if (redirect != NULL) {
      *redirect = 1;
    }
  }

  return 0;
}

static enum status parse_range(const char *str, size_t size, size_t *lower,
                               size_t *upper) {
  char first[FIELD_MAX], last[FIELD_MAX];
  const char *p, *q, *r, *err;

  /* default to the complete range */
  *lower = 0;
  *upper = size - 1;

  /* done if no range-string is given */
  if (str == NULL || *str == '\0') {
    return 0;
  }

  /* skip opening statement */
  if (strncmp(str, "bytes=", sizeof("bytes=") - 1)) {
    return S_BAD_REQUEST;
  }
  p = str + (sizeof("bytes=") - 1);

  /* check string (should only contain numbers and a hyphen) */
  for (r = p, q = NULL; *r != '\0'; r++) {
    if (*r < '0' || *r > '9') {
      if (*r == '-') {
        if (q != NULL) {
          /* we have already seen a hyphen */
          return S_BAD_REQUEST;
        } else {
          /* place q after the hyphen */
          q = r + 1;
        }
      } else if (*r == ',' && r > p) {
        /*
         * we refuse to accept range-lists out
         * of spite towards this horrible part
         * of the spec
         */
        return S_RANGE_NOT_SATISFIABLE;
      } else {
        return S_BAD_REQUEST;
      }
    }
  }
  if (q == NULL) {
    /* the input string must contain a hyphen */
    return S_BAD_REQUEST;
  }
  r = q + strlen(q);

  /*
   *  byte-range=first-last\0
   *             ^     ^   ^
   *             |     |   |
   *             p     q   r
   */

  /* copy 'first' and 'last' to their respective arrays */
  if ((size_t)((q - 1) - p + 1) > sizeof(first) ||
      (size_t)(r - q + 1) > sizeof(last)) {
    return S_REQUEST_TOO_LARGE;
  }
  memcpy(first, p, (q - 1) - p);
  first[(q - 1) - p] = '\0';
  memcpy(last, q, r - q);
  last[r - q] = '\0';

  if (first[0] != '\0') {
    /*
     * range has format "first-last" or "first-",
     * i.e. return bytes 'first' to 'last' (or the
     * last byte if 'last' is not given),
     * inclusively, and byte-numbering beginning at 0
     */
    *lower = strtonum(first, 0, MIN(SIZE_MAX, LLONG_MAX), &err);
    if (!err) {
      if (last[0] != '\0') {
        *upper = strtonum(last, 0, MIN(SIZE_MAX, LLONG_MAX), &err);
      } else {
        *upper = size - 1;
      }
    }
    if (err) {
      /* one of the strtonum()'s failed */
      return S_BAD_REQUEST;
    }

    /* check ranges */
    if (*lower > *upper || *lower >= size) {
      return S_RANGE_NOT_SATISFIABLE;
    }

    /* adjust upper limit to be at most the last byte */
    *upper = MIN(*upper, size - 1);
  } else {
    /* last must not also be empty */
    if (last[0] == '\0') {
      return S_BAD_REQUEST;
    }

    /*
     * use upper as a temporary storage for 'num',
     * as we know 'upper' is size - 1
     */
    *upper = strtonum(last, 0, MIN(SIZE_MAX, LLONG_MAX), &err);
    if (err) {
      return S_BAD_REQUEST;
    }

    /* determine lower */
    if (*upper > size) {
      /* more bytes requested than we have */
      *lower = 0;
    } else {
      *lower = size - *upper;
    }

    /* set upper to the correct value */
    *upper = size - 1;
  }

  return 0;
}

void http_prepare_response(const struct request *req, struct response *res,
                           const struct server *srv) {
  enum status s, tmps;
  struct stat st;
  size_t i;
  int redirect;
  static char tmppath[PATH_MAX];
  char *p, *mime;

  /* empty all response fields */
  memset(res, 0, sizeof(*res));

  /* copy request-path to response-path and clean it up */
  redirect = 0;
  memcpy(res->path, req->path, MIN(sizeof(res->path), sizeof(req->path)));

  /*
   * generate and stat internal path based on the cleaned up request
   * path while ignoring query and fragment
   */
  if (esnprintf(res->internal_path, sizeof(res->internal_path), "/%s",
                res->path)) {
    s = S_REQUEST_TOO_LARGE;
    http_prepare_error_response(req, res, s);
    return;
  }
  if (stat(res->internal_path, &st) < 0) {
    s = (errno == EACCES) ? S_FORBIDDEN : S_NOT_FOUND;
    http_prepare_error_response(req, res, s);
    return;
  }

  /*
   * if the path points at a directory, make sure both the path
   * and internal path have a trailing slash
   */
  if (S_ISDIR(st.st_mode)) {
    if ((tmps = path_ensure_dirslash(res->path, &redirect)) ||
        (tmps = path_ensure_dirslash(res->internal_path, NULL))) {
      s = tmps;
      http_prepare_error_response(req, res, s);
      return;
    }
  }

  /* redirect if the path-cleanup necessitated it earlier */
  if (redirect) {
    res->status = S_MOVED_PERMANENTLY;

    /* encode path */
    encode(res->path, tmppath);

    /*
     * write relative redirection URI to response struct
     * (re-including the query and fragment, if present)
     */
    if (esnprintf(res->field[RES_LOCATION], sizeof(res->field[RES_LOCATION]),
                  "%s%s%s%s%s", tmppath, req->query[0] ? "?" : "", req->query,
                  req->fragment[0] ? "#" : "", req->fragment)) {
      s = S_REQUEST_TOO_LARGE;
      http_prepare_error_response(req, res, s);
      return;
    }

    return;
  }

  if (S_ISDIR(st.st_mode)) {
    if (esnprintf(tmppath, sizeof(tmppath), "%s%s", res->internal_path,
                  srv->docindex)) {
      s = S_REQUEST_TOO_LARGE;
      http_prepare_error_response(req, res, s);
      return;
    }

    /* stat the temporary path, which must be a regular file */
    if (stat(tmppath, &st) < 0 || !S_ISREG(st.st_mode)) {
      if (access(res->internal_path, R_OK) != 0) {
        s = S_FORBIDDEN;
        http_prepare_error_response(req, res, s);
        return;
      } else {
        res->status = S_OK;
      }
      res->type = RESTYPE_DIRLISTING;

      if (esnprintf(res->field[RES_CONTENT_TYPE],
                    sizeof(res->field[RES_CONTENT_TYPE]), "%s",
                    "text/html; charset=utf-8")) {
        s = S_INTERNAL_SERVER_ERROR;
        http_prepare_error_response(req, res, s);
        return;
      }

      return;

    } else {
      if (esnprintf(res->internal_path, sizeof(res->internal_path), "%s",
                    tmppath)) {
        s = S_REQUEST_TOO_LARGE;
        http_prepare_error_response(req, res, s);
        return;
      }
    }
  }

  /* range */
  if ((s = parse_range(req->field[REQ_RANGE], st.st_size, &(res->file.lower),
                       &(res->file.upper)))) {
    if (s == S_RANGE_NOT_SATISFIABLE) {
      res->status = S_RANGE_NOT_SATISFIABLE;

      if (esnprintf(res->field[RES_CONTENT_RANGE],
                    sizeof(res->field[RES_CONTENT_RANGE]), "bytes */%zu",
                    st.st_size)) {
        s = S_INTERNAL_SERVER_ERROR;
        http_prepare_error_response(req, res, s);
        return;
      }

      return;
    } else {
      http_prepare_error_response(req, res, s);
      return;
    }
  }

  mime = "application/octet-stream";
  if ((p = strrchr(res->internal_path, '.'))) {
    for (i = 0; i < LEN(mimes); i++) {
      if (!strcmp(mimes[i].ext, p + 1)) {
        mime = mimes[i].type;
        break;
      }
    }
  }

  res->type = RESTYPE_FILE;

  res->status = (access(res->internal_path, R_OK))   ? S_FORBIDDEN
                : (req->field[REQ_RANGE][0] != '\0') ? S_PARTIAL_CONTENT
                                                     : S_OK;

  if (esnprintf(res->field[RES_ACCEPT_RANGES],
                sizeof(res->field[RES_ACCEPT_RANGES]), "%s", "bytes")) {
    s = S_INTERNAL_SERVER_ERROR;
    http_prepare_error_response(req, res, s);
    return;
  }

  if (esnprintf(res->field[RES_CONTENT_LENGTH],
                sizeof(res->field[RES_CONTENT_LENGTH]), "%zu",
                res->file.upper - res->file.lower + 1)) {
    s = S_INTERNAL_SERVER_ERROR;
    http_prepare_error_response(req, res, s);
    return;
  }
  if (req->field[REQ_RANGE][0] != '\0') {
    if (esnprintf(res->field[RES_CONTENT_RANGE],
                  sizeof(res->field[RES_CONTENT_RANGE]), "bytes %zd-%zd/%zu",
                  res->file.lower, res->file.upper, st.st_size)) {
      s = S_INTERNAL_SERVER_ERROR;
      http_prepare_error_response(req, res, s);
      return;
    }
  }
  if (esnprintf(res->field[RES_CONTENT_TYPE],
                sizeof(res->field[RES_CONTENT_TYPE]), "%s", mime)) {
    s = S_INTERNAL_SERVER_ERROR;
    http_prepare_error_response(req, res, s);
    return;
  }
  if (timestamp(res->field[RES_LAST_MODIFIED],
                sizeof(res->field[RES_LAST_MODIFIED]), st.st_mtim.tv_sec)) {
    s = S_INTERNAL_SERVER_ERROR;
    http_prepare_error_response(req, res, s);
    return;
  }
}

void http_prepare_error_response(const struct request *req,
                                 struct response *res, enum status s) {
  /* used later */
  (void)req;

  /* empty all response fields */
  memset(res, 0, sizeof(*res));

  res->type = RESTYPE_ERROR;
  res->status = s;

  if (esnprintf(res->field[RES_CONTENT_TYPE],
                sizeof(res->field[RES_CONTENT_TYPE]),
                "text/html; charset=utf-8")) {
    res->status = S_INTERNAL_SERVER_ERROR;
  }

  if (res->status == S_METHOD_NOT_ALLOWED) {
    if (esnprintf(res->field[RES_ALLOW], sizeof(res->field[RES_ALLOW]),
                  "Allow: GET, HEAD")) {
      res->status = S_INTERNAL_SERVER_ERROR;
    }
  }
}
