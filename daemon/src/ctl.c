#include "ctl.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "cJSON.h"
#include "util.h"

int ctl_main(const char *socket_path, const char *request_json)
{
  cJSON *req = cJSON_Parse(request_json);
  if (!cJSON_IsObject(req)) {
    fprintf(stderr, "clipnetd --ctl: not a JSON object\n");
    cJSON_Delete(req);
    return 2;
  }
  if (!cJSON_GetObjectItem(req, "id")) cJSON_AddNumberToObject(req, "id", 1);
  char *line = cJSON_PrintUnformatted(req);
  cJSON_Delete(req);

  struct sockaddr_un sa = { .sun_family = AF_UNIX };
  snprintf(sa.sun_path, sizeof sa.sun_path, "%s", socket_path);
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
    fprintf(stderr, "clipnetd is not running (%s: %s)\n", socket_path, strerror(errno));
    free(line);
    if (fd >= 0) close(fd);
    return 2;
  }
  struct buf out = { 0 };
  buf_append_str(&out, line);
  buf_append(&out, "\n", 1);
  free(line);
  size_t off = 0;
  while (off < out.len) {
    ssize_t w = send(fd, out.data + off, out.len - off, MSG_NOSIGNAL);
    if (w < 0 && errno == EINTR) continue;
    if (w < 0) { perror("clipnetd --ctl: send"); close(fd); buf_free(&out); return 2; }
    off += (size_t)w;
  }
  buf_free(&out);

  /* The reply is the first complete line (we did not subscribe to events). */
  struct buf in = { 0 };
  char chunk[65536];
  char *nl = NULL;
  while (!nl) {
    ssize_t r = recv(fd, chunk, sizeof chunk, 0);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) break;
    buf_append(&in, chunk, (size_t)r);
    nl = memchr(in.data, '\n', in.len);
  }
  close(fd);
  if (!nl) {
    fprintf(stderr, "clipnetd --ctl: no reply\n");
    buf_free(&in);
    return 2;
  }
  cJSON *rep = cJSON_ParseWithLength((char *)in.data, (size_t)(nl - (char *)in.data));
  buf_free(&in);
  int rc;
  if (cJSON_IsTrue(cJSON_GetObjectItem(rep, "ok"))) {
    char *s = cJSON_Print(cJSON_GetObjectItem(rep, "result"));
    puts(s ? s : "null");
    free(s);
    rc = 0;
  } else {
    cJSON *e = cJSON_GetObjectItem(rep, "error");
    const char *msg = cJSON_GetStringValue(cJSON_GetObjectItem(e, "message"));
    const char *code = cJSON_GetStringValue(cJSON_GetObjectItem(e, "code"));
    fprintf(stderr, "clipnet: %s%s%s%s\n", msg ? msg : "error", code ? " (" : "", code ? code : "", code ? ")" : "");
    rc = 1;
  }
  cJSON_Delete(rep);
  return rc;
}
