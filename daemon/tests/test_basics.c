#include "harness.h"

#include <stdlib.h>

#include "imgmeta.h"
#include "sha256.h"
#include "util.h"

static void hex_of(const char *msg, size_t n, char out[65])
{
  uint8_t h[32];
  sha256(msg, n, h);
  hex_encode(h, 32, out);
}

static void test_sha256(void)
{
  char hex[65];
  /* FIPS 180-4 / NIST test vectors. */
  hex_of("", 0, hex);
  CHECK_STR(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  hex_of("abc", 3, hex);
  CHECK_STR(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  hex_of(two, strlen(two), hex);
  CHECK_STR(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

  /* One million 'a', fed in awkward chunk sizes to exercise buffering. */
  struct sha256 s;
  sha256_init(&s);
  char chunk[997];
  memset(chunk, 'a', sizeof chunk);
  size_t left = 1000000;
  while (left) {
    size_t n = left < sizeof chunk ? left : sizeof chunk;
    sha256_update(&s, chunk, n);
    left -= n;
  }
  uint8_t h[32];
  sha256_final(&s, h);
  hex_encode(h, 32, hex);
  CHECK_STR(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

static void test_utf8(void)
{
  CHECK(utf8_valid((const uint8_t *)"héllo ✓ 𝄞", strlen("héllo ✓ 𝄞")));
  CHECK(!utf8_valid((const uint8_t *)"\xC0\xAF", 2));          /* overlong */
  CHECK(!utf8_valid((const uint8_t *)"\xED\xA0\x80", 3));      /* surrogate */
  CHECK(!utf8_valid((const uint8_t *)"\xE2\x9C", 2));          /* truncated */
  CHECK(!utf8_valid((const uint8_t *)"\xF4\x90\x80\x80", 4));  /* > U+10FFFF */

  size_t n;
  char *s = latin1_to_utf8((const uint8_t *)"caf\xE9", 4, &n);
  CHECK_STR(s, "café");
  CHECK_INT(n, 5);
  free(s);

  const char *t = "a✓b𝄞c";
  CHECK_INT(utf8_prefix(t, strlen(t), 2), 4);
  CHECK_INT(utf8_prefix(t, strlen(t), 4), 9);
  CHECK_INT(utf8_prefix(t, strlen(t), 100), strlen(t));
}

static void test_uuid(void)
{
  char a[37], b[37];
  uuid_v4(a);
  uuid_v4(b);
  CHECK_INT(strlen(a), 36);
  CHECK(a[14] == '4');
  CHECK(a[19] == '8' || a[19] == '9' || a[19] == 'a' || a[19] == 'b');
  CHECK(strcmp(a, b) != 0);
}

static void test_imgmeta(void)
{
  static const uint8_t png[] = {
    0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R',
    0, 0, 0x07, 0x80, 0, 0, 0x04, 0x38, 8, 6, 0, 0, 0,
  };
  struct img_meta m;
  CHECK(img_meta_parse(png, sizeof png, &m));
  CHECK_STR(m.format, "PNG");
  CHECK_INT(m.width, 1920);
  CHECK_INT(m.height, 1080);

  static const uint8_t gif[] = { 'G', 'I', 'F', '8', '9', 'a', 0x40, 0x01, 0xF0, 0x00 };
  CHECK(img_meta_parse(gif, sizeof gif, &m));
  CHECK_INT(m.width, 320);
  CHECK_INT(m.height, 240);

  /* SOI, an APP0 segment, then SOF0 with 480x640. */
  static const uint8_t jpg[] = {
    0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 0x00, 0x00,
    0xFF, 0xC0, 0x00, 0x11, 0x08, 0x02, 0x80, 0x01, 0xE0, 0x03, 0, 0, 0, 0, 0,
  };
  CHECK(img_meta_parse(jpg, sizeof jpg, &m));
  CHECK_STR(m.format, "JPEG");
  CHECK_INT(m.width, 480);
  CHECK_INT(m.height, 640);

  CHECK(!img_meta_parse((const uint8_t *)"hello", 5, &m));
}

void run_tests(void)
{
  test_sha256();
  test_utf8();
  test_uuid();
  test_imgmeta();
}
