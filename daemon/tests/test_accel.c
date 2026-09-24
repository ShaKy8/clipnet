#include "harness.h"

#include "accel.h"

static void roundtrip(const char *in, const char *portable, const char *hypr)
{
  struct accel a;
  const char *err = NULL;
  t_checks++;
  if (!accel_parse(in, &a, &err)) {
    t_failures++;
    fprintf(stderr, "accel_parse(\"%s\") failed: %s\n", in, err);
    return;
  }
  char p[64], h[96];
  accel_format(&a, p);
  accel_to_hypr(&a, h);
  CHECK_STR(p, portable);
  CHECK_STR(h, hypr);
}

static void rejects(const char *in)
{
  struct accel a;
  const char *err = NULL;
  t_checks++;
  if (accel_parse(in, &a, &err)) {
    t_failures++;
    fprintf(stderr, "accel_parse(\"%s\") should have failed\n", in);
  } else if (!err) {
    t_failures++;
    fprintf(stderr, "accel_parse(\"%s\") failed without a message\n", in);
  }
}

void run_tests(void)
{
  roundtrip("Ctrl+Alt+1", "Ctrl+Alt+1", "CTRL + ALT + 1");
  roundtrip("alt + ctrl + v", "Ctrl+Alt+V", "CTRL + ALT + V");
  roundtrip("Super+Shift+Apostrophe", "Super+Shift+Apostrophe", "SUPER + SHIFT + apostrophe");
  roundtrip("Ctrl+'", "Ctrl+Apostrophe", "CTRL + apostrophe");
  roundtrip("Meta+PgDown", "Super+PageDown", "SUPER + Next");
  roundtrip("Control+Shift+BackSpace", "Ctrl+Shift+Backspace", "CTRL + SHIFT + BackSpace");
  roundtrip("F9", "F9", "F9");
  roundtrip("f24", "F24", "F24");
  roundtrip("Ctrl+[", "Ctrl+BracketLeft", "CTRL + bracketleft");

  rejects("");
  rejects("V");            /* no modifier */
  rejects("Shift+V");      /* would eat typed capitals */
  rejects("Ctrl+");        /* no key */
  rejects("Ctrl+Alt");     /* modifier as key */
  rejects("Hyper+V");      /* unknown modifier */
  rejects("Ctrl+NoSuchKey");
  rejects("F25");

  struct accel a;
  const char *err;
  CHECK(accel_parse("Ctrl+Apostrophe", &a, &err));
  CHECK(accel_matches_hypr(&a, 4, "apostrophe"));
  CHECK(accel_matches_hypr(&a, 4 | 16, "apostrophe")); /* Num Lock on */
  CHECK(!accel_matches_hypr(&a, 4 | 1, "apostrophe"));
  CHECK(!accel_matches_hypr(&a, 4, "grave"));
  CHECK(accel_parse("Super+Ctrl+V", &a, &err));
  CHECK(accel_matches_hypr(&a, 68, "V"));
  CHECK(accel_matches_hypr(&a, 68, "v"));
  CHECK(accel_parse("Super+Ctrl+Backspace", &a, &err));
  CHECK(accel_matches_hypr(&a, 68, "BACKSPACE"));
}
