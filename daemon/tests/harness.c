#include "harness.h"

#include <stdlib.h>
#include <unistd.h>

#include "util.h"

int t_failures, t_checks;
static char *tmpdir;

static void cleanup(void)
{
  if (!tmpdir) return;
  char *cmd = xasprintf("rm -rf '%s'", tmpdir);
  if (system(cmd) != 0) fprintf(stderr, "could not remove %s\n", tmpdir);
  free(cmd);
}

const char *test_tmpdir(void)
{
  if (tmpdir) return tmpdir;
  const char *base = getenv("TMPDIR");
  char *tmpl = xasprintf("%s/clipnet-test-XXXXXX", base && *base ? base : "/tmp");
  tmpdir = mkdtemp(tmpl);
  if (!tmpdir) { perror("mkdtemp"); exit(2); }
  atexit(cleanup);
  return tmpdir;
}

int main(int argc, char **argv)
{
  (void)argc;
  log_threshold = getenv("CLIPNET_TEST_LOG") ? LOG_DEBUG : LOG_ERR;
  run_tests();
  const char *name = strrchr(argv[0], '/');
  name = name ? name + 1 : argv[0];
  if (t_failures) {
    fprintf(stderr, "FAIL %s: %d of %d checks failed\n", name, t_failures, t_checks);
    return 1;
  }
  printf("ok   %s (%d checks)\n", name, t_checks);
  return 0;
}
