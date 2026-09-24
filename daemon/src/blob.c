#include "blob.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util.h"

void blob_name(const uint8_t hash[32], char out[68])
{
  char hex[65];
  hex_encode(hash, 32, hex);
  snprintf(out, 68, "%.2s/%s", hex, hex);
}

char *blob_path(const char *dir, const char *name)
{
  return xasprintf("%s/%s", dir, name);
}

int blob_put(const char *dir, const char *name, const void *data, size_t n)
{
  char *path = blob_path(dir, name);
  struct stat st;
  if (stat(path, &st) == 0 && (size_t)st.st_size == n) { free(path); return 0; }

  char *sub = xasprintf("%s/%.2s", dir, name);
  mkdir_p(sub, 0700);
  free(sub);

  char *tmp = xasprintf("%s.tmp.%d", path, getpid());
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  int rc = -1;
  if (fd >= 0) {
    const uint8_t *p = data;
    size_t left = n;
    while (left) {
      ssize_t w = write(fd, p, left);
      if (w < 0) { if (errno == EINTR) continue; break; }
      p += w;
      left -= (size_t)w;
    }
    /* No fsync: it can stall the loop for a big image, and a clip lost to a
     * power cut is a cheaper failure than a paste that hangs. */
    if (!left && close(fd) == 0) {
      fd = -1;
      rc = rename(tmp, path);
    }
    if (fd >= 0) close(fd);
  }
  if (rc < 0) {
    log_err("blob write %s: %m", name);
    unlink(tmp);
  }
  free(tmp);
  free(path);
  return rc;
}

void *blob_map(const char *dir, const char *name, size_t *len)
{
  char *path = blob_path(dir, name);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  free(path);
  if (fd < 0) return NULL;
  struct stat st;
  void *p = NULL;
  if (fstat(fd, &st) == 0) {
    *len = (size_t)st.st_size;
    /* mmap of an empty file fails; hand back a harmless non-NULL pointer. */
    p = st.st_size ? mmap(NULL, *len, PROT_READ, MAP_PRIVATE, fd, 0) : (void *)"";
    if (p == MAP_FAILED) p = NULL;
  }
  close(fd);
  return p;
}

void blob_unmap(void *p, size_t len)
{
  if (p && len) munmap(p, len);
}

int blob_unlink(const char *dir, const char *name)
{
  char *path = blob_path(dir, name);
  int r = unlink(path);
  free(path);
  return r;
}
