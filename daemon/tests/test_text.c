#include "harness.h"

#include <stdlib.h>

#include "mime.h"
#include "textutil.h"
#include "util.h"

#define PREVIEW(in, max, want) do { \
    char *_p = text_preview(in, strlen(in), max); \
    CHECK_STR(_p, want); \
    free(_p); \
  } while (0)

#define HTML(in, want) do { \
    char *_p = html_to_text(in, strlen(in)); \
    CHECK_STR(_p, want); \
    free(_p); \
  } while (0)

static void test_preview(void)
{
  PREVIEW("  hello\n\n  world\t!  ", 100, "hello world !");
  PREVIEW("abcdef", 3, "abc");
  PREVIEW("✓✓✓✓", 2, "✓✓");
  PREVIEW("a\x01\x02" "b", 10, "ab");
  PREVIEW("   ", 10, "");
  PREVIEW("one two", 4, "one");
  PREVIEW("one two", 5, "one t");
}

static void test_html(void)
{
  HTML("<b>bold</b> and <i>italic</i>", "bold and italic");
  HTML("<p>one</p><p>two</p>", "one\ntwo");
  HTML("a &amp; b &lt;c&gt; &#65;&#x42; &nbsp;x", "a & b <c> AB \xC2\xA0x");
  HTML("<style>p{color:red}</style>visible<script>alert(1)</script>", "visible");
  HTML("<!-- hidden -->shown", "shown");
  HTML("AT&T & co", "AT&T & co");
  HTML("<table><tr><td>a</td><td>b</td></tr></table>", "a\tb");
  HTML("<meta charset='utf-8'><span>x</span>", "x");
}

static void test_uri(void)
{
  const char *in = "# comment\r\nfile:///home/kyle/My%20File.txt\r\nfile://localhost/etc/hosts\r\n";
  char *p = uri_list_to_paths(in, strlen(in));
  CHECK_STR(p, "/home/kyle/My File.txt\n/etc/hosts");
  free(p);
  p = uri_list_to_paths("file:///a%2", 11);
  CHECK_STR(p, "/a%2");
  free(p);
}

static void test_human(void)
{
  char s[32];
  human_size(512, s);
  CHECK_STR(s, "512 B");
  human_size(1536, s);
  CHECK_STR(s, "1.5 KB");
  human_size(1258291, s);
  CHECK_STR(s, "1.2 MB");
}

static void test_mime_plan(void)
{
  struct mime_plan p;
  /* What wl-copy -t text/html offers (observed in spike S1). */
  const char *html[] = { "text/html", "text/plain", "text/plain;charset=utf-8", "TEXT", "STRING", "UTF8_STRING" };
  mime_plan_build(&p, html, ARRAY_LEN(html));
  CHECK_INT(p.n_reads, 2);
  CHECK_STR(p.reads[0].offered, "text/plain;charset=utf-8");
  CHECK_STR(p.reads[0].canonical, MIME_TEXT);
  CHECK_STR(p.reads[1].offered, "text/html");
  CHECK_INT(p.n_text_aliases, 5);
  CHECK(!p.is_ours && !p.is_sensitive);

  /* A Qt image copy: many conversions of one picture. */
  const char *img[] = { "image/png", "image/bmp", "image/jpeg", "image/tiff", "application/x-qt-image", "image/svg+xml" };
  mime_plan_build(&p, img, ARRAY_LEN(img));
  CHECK_INT(p.n_reads, 3);
  CHECK_STR(p.reads[0].offered, "image/png");
  CHECK_STR(p.reads[1].offered, "application/x-qt-image");
  CHECK_STR(p.reads[2].offered, "image/svg+xml");

  /* Only STRING: read it, store it as canonical text. */
  const char *x11[] = { "TARGETS", "STRING", "TIMESTAMP" };
  mime_plan_build(&p, x11, ARRAY_LEN(x11));
  CHECK_INT(p.n_reads, 1);
  CHECK_STR(p.reads[0].offered, "STRING");

  const char *pw[] = { "text/plain", MIME_PASSWORD_HINT };
  mime_plan_build(&p, pw, ARRAY_LEN(pw));
  CHECK(p.is_sensitive);

  const char *ours[] = { MIME_TEXT, MIME_MARKER };
  mime_plan_build(&p, ours, ARRAY_LEN(ours));
  CHECK(p.is_ours);
  CHECK_INT(p.n_reads, 1);

  /* The same type offered twice (wl-copy does this) is read once. */
  const char *dup[] = { "text/plain", "text/plain", "text/x-custom", "text/x-custom" };
  mime_plan_build(&p, dup, ARRAY_LEN(dup));
  CHECK_INT(p.n_reads, 2);
  CHECK_INT(p.n_text_aliases, 1);
}

static void test_classify(void)
{
  const char *a[] = { MIME_TEXT };
  CHECK_INT(mime_classify(a, 1), KIND_TEXT);
  const char *b[] = { MIME_TEXT, "text/html" };
  CHECK_INT(mime_classify(b, 2), KIND_RICH);
  const char *c[] = { "text/html", "image/png" };
  CHECK_INT(mime_classify(c, 2), KIND_IMAGE);
  const char *d[] = { "x-special/gnome-copied-files", "text/uri-list", MIME_TEXT };
  CHECK_INT(mime_classify(d, 3), KIND_FILES);
  const char *e[] = { "application/x-thing" };
  CHECK_INT(mime_classify(e, 1), KIND_OTHER);
}

void run_tests(void)
{
  test_preview();
  test_html();
  test_uri();
  test_human();
  test_mime_plan();
  test_classify();
}
