/* Minimal test harness: CHECK records a failure and carries on, so one run
 * reports every broken expectation in a file, not just the first. */
#pragma once

#include <stdio.h>
#include <string.h>

extern int t_failures, t_checks;

#define CHECK(cond) do { \
    t_checks++; \
    if (!(cond)) { t_failures++; fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)

#define CHECK_STR(a, b) do { \
    const char *_a = (a), *_b = (b); \
    t_checks++; \
    if (!_a || !_b || strcmp(_a, _b)) { \
      t_failures++; \
      fprintf(stderr, "%s:%d: CHECK_STR failed: %s\n    got:      \"%s\"\n    expected: \"%s\"\n", \
              __FILE__, __LINE__, #a, _a ? _a : "(null)", _b ? _b : "(null)"); \
    } \
  } while (0)

#define CHECK_INT(a, b) do { \
    long long _a = (long long)(a), _b = (long long)(b); \
    t_checks++; \
    if (_a != _b) { t_failures++; fprintf(stderr, "%s:%d: CHECK_INT failed: %s = %lld, expected %lld\n", \
                                          __FILE__, __LINE__, #a, _a, _b); } \
  } while (0)

/* Each test file defines run_tests(); the harness supplies main(). */
void run_tests(void);
/* A fresh private directory under $TMPDIR, removed at exit. */
const char *test_tmpdir(void);
