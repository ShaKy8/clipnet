#include "harness.h"

#include <stdlib.h>

#include "transform.h"

static void expect(const char *id, const char *in, const char *want)
{
  const char *err = NULL;
  char *out = transform_apply(id, in, strlen(in), NULL, &err);
  if (!out || strcmp(out, want)) {
    t_failures++;
    fprintf(stderr, "transform %s(\"%s\"):\n    got:      \"%s\"\n    expected: \"%s\"\n", id, in, out ? out : "(null)",
            want);
  }
  t_checks++;
  free(out);
}

void run_tests(void)
{
  transform_init_locale();

  expect("upper", "héllo wörld ß", "HÉLLO WÖRLD ß");
  expect("lower", "ÇA VA? ΣΟΦΊΑ", "ça va? σοφία");
  expect("invert", "Hello World", "hELLO wORLD");

  expect("title", "the quick BROWN fox", "The Quick Brown Fox");
  expect("title", "don't stop — l'été", "Don't Stop — L'été");
  expect("title", "hello-world foo_bar", "Hello-World Foo_Bar");

  expect("sentence", "HELLO THERE. how ARE you? fine!", "Hello there. How are you? Fine!");
  expect("sentence", "first line\n\nsecond PARAGRAPH", "First line\n\nSecond paragraph");
  expect("sentence", "\"quoted START.\" next", "\"Quoted start.\" Next");
  expect("sentence", "v1.2 is out", "V1.2 is out");

  expect("camel", "Hello big World", "helloBigWorld");
  expect("camel", "user_id from-table", "userIdFromTable");
  expect("camel", "fooBar baz", "fooBarBaz");
  expect("snake", "Hello big World", "hello_big_world");
  expect("snake", "someCamelCase", "some_camel_case");
  expect("slug", "  Hello, World! Ça va?  ", "hello-world-ça-va");
  expect("slug", "don't panic", "dont-panic");

  expect("trim", "  \n padded text \t\n", "padded text");
  expect("trim", "   ", "");
  expect("one_line", "line one\nline two  \r\n  line three\n", "line one line two line three");
  expect("one_line", "a\n\n\nb", "a b");
  expect("add_newline", "x", "x\n");
  expect("add_two_newlines", "x", "x\n\n");

  expect("upper", "", "");
  expect("upper", "bad \xFF byte", "BAD \xEF\xBF\xBD BYTE");

  const char *err = NULL;
  CHECK(transform_apply("nope", "x", 1, NULL, &err) == NULL);
  CHECK(err != NULL);

  char *d = transform_apply("date", "", 0, "%Y", &err);
  CHECK(d && strlen(d) == 4 && d[0] == '2');
  free(d);
  d = transform_apply("append_date", "at", 2, "[fixed]", &err);
  CHECK_STR(d, "at [fixed]");
  free(d);

  size_t n;
  const struct transform_def *defs = transform_list(&n);
  CHECK(n >= 10);
  for (size_t i = 0; i < n; i++) {
    char *out = transform_apply(defs[i].id, "abc", 3, NULL, &err);
    CHECK(out != NULL);
    free(out);
  }
}
