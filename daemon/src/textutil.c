#include "textutil.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"

char *text_preview(const char *s, size_t n, size_t max_chars)
{
  struct buf b = { 0 };
  size_t chars = 0;
  bool pending_space = false;
  size_t i = 0;
  while (i < n && chars < max_chars) {
    unsigned char c = (unsigned char)s[i];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
      pending_space = b.len > 0;
      i++;
      continue;
    }
    if (c < 0x20 || c == 0x7F) { i++; continue; } /* other control chars */
    if (pending_space) {
      /* A space only earns its place if a character follows it. */
      if (chars + 1 >= max_chars) break;
      buf_append(&b, " ", 1);
      chars++;
      pending_space = false;
    }
    size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : 4;
    if (i + len > n) break;
    buf_append(&b, s + i, len);
    i += len;
    chars++;
  }
  if (!b.data) buf_append(&b, "", 0);
  return buf_steal(&b, NULL);
}

static void put_utf8(struct buf *b, unsigned long cp)
{
  char o[4];
  if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
  if (cp < 0x80) { o[0] = (char)cp; buf_append(b, o, 1); }
  else if (cp < 0x800) { o[0] = (char)(0xC0 | cp >> 6); o[1] = (char)(0x80 | (cp & 0x3F)); buf_append(b, o, 2); }
  else if (cp < 0x10000) {
    o[0] = (char)(0xE0 | cp >> 12); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[2] = (char)(0x80 | (cp & 0x3F));
    buf_append(b, o, 3);
  } else {
    o[0] = (char)(0xF0 | cp >> 18); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F));
    buf_append(b, o, 4);
  }
}

/* Decode the entity starting at s[0] == '&'. Returns bytes consumed, or 0 if
 * it is not a recognised entity (the '&' is then kept literally). */
static size_t entity(const char *s, size_t n, struct buf *b)
{
  static const struct { const char *name; unsigned long cp; } named[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' },
    { "nbsp", 0xA0 }, { "copy", 0xA9 }, { "reg", 0xAE }, { "hellip", 0x2026 },
    { "mdash", 0x2014 }, { "ndash", 0x2013 }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 },
    { "ldquo", 0x201C }, { "rdquo", 0x201D }, { "bull", 0x2022 }, { "middot", 0xB7 },
  };
  size_t end = 1;
  while (end < n && end < 12 && s[end] != ';') end++;
  if (end >= n || s[end] != ';') return 0;
  if (s[1] == '#') {
    char *e;
    char tmp[16];
    size_t len = end - 2;
    if (len == 0 || len >= sizeof tmp) return 0;
    memcpy(tmp, s + 2, len);
    tmp[len] = 0;
    unsigned long cp = (tmp[0] == 'x' || tmp[0] == 'X') ? strtoul(tmp + 1, &e, 16) : strtoul(tmp, &e, 10);
    if (*e) return 0;
    put_utf8(b, cp);
    return end + 1;
  }
  for (size_t i = 0; i < ARRAY_LEN(named); i++) {
    size_t len = strlen(named[i].name);
    if (len == end - 1 && !memcmp(s + 1, named[i].name, len)) {
      put_utf8(b, named[i].cp);
      return end + 1;
    }
  }
  return 0;
}

static bool tag_is(const char *tag, size_t len, const char *name)
{
  size_t nl = strlen(name);
  return len == nl && !strncasecmp(tag, name, nl);
}

char *html_to_text(const char *s, size_t n)
{
  static const char *const block[] = {
    "p", "div", "br", "li", "tr", "h1", "h2", "h3", "h4", "h5", "h6",
    "pre", "blockquote", "table", "ul", "ol", "hr", "section", "article",
  };
  struct buf b = { 0 };
  size_t i = 0;
  const char *skip_until = NULL; /* inside <script> or <style> */
  while (i < n) {
    if (s[i] == '<') {
      size_t j = i + 1;
      bool closing = j < n && s[j] == '/';
      if (closing) j++;
      size_t name_start = j;
      while (j < n && (isalnum((unsigned char)s[j]))) j++;
      size_t name_len = j - name_start;
      /* Comments and doctype: skip to the matching end. */
      if (i + 3 < n && !memcmp(s + i, "<!--", 4)) {
        const char *e = memmem(s + i + 4, n - i - 4, "-->", 3);
        i = e ? (size_t)(e - s) + 3 : n;
        continue;
      }
      const char *gt = memchr(s + j, '>', n - j);
      size_t after = gt ? (size_t)(gt - s) + 1 : n;
      const char *name = s + name_start;
      if (skip_until) {
        if (closing && tag_is(name, name_len, skip_until)) skip_until = NULL;
        i = after;
        continue;
      }
      if (!closing && (tag_is(name, name_len, "script") || tag_is(name, name_len, "style")))
        skip_until = tag_is(name, name_len, "script") ? "script" : "style";
      for (size_t k = 0; k < ARRAY_LEN(block); k++) {
        if (tag_is(name, name_len, block[k])) {
          if (b.len && b.data[b.len - 1] != '\n') buf_append(&b, "\n", 1);
          break;
        }
      }
      if (tag_is(name, name_len, "td") || tag_is(name, name_len, "th")) {
        if (!closing && b.len && b.data[b.len - 1] != '\n') buf_append(&b, "\t", 1);
      }
      i = after;
      continue;
    }
    if (skip_until) { i++; continue; }
    if (s[i] == '&') {
      size_t used = entity(s + i, n - i, &b);
      if (used) { i += used; continue; }
    }
    buf_append(&b, s + i, 1);
    i++;
  }
  /* Trim trailing whitespace. */
  while (b.len && (b.data[b.len - 1] == '\n' || b.data[b.len - 1] == ' ' || b.data[b.len - 1] == '\t'))
    b.data[--b.len] = 0;
  if (!b.data) buf_append(&b, "", 0);
  return buf_steal(&b, NULL);
}

static int hexval(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

char *uri_list_to_paths(const char *s, size_t n)
{
  struct buf b = { 0 };
  size_t i = 0;
  while (i < n) {
    size_t e = i;
    while (e < n && s[e] != '\n' && s[e] != '\r') e++;
    if (e > i && s[i] != '#') {
      const char *line = s + i;
      size_t len = e - i;
      if (len >= 7 && !strncmp(line, "file://", 7)) {
        line += 7;
        len -= 7;
        /* file://host/path: drop the (usually empty) host. */
        const char *slash = memchr(line, '/', len);
        if (slash) { len -= (size_t)(slash - line); line = slash; }
      }
      if (b.len) buf_append(&b, "\n", 1);
      for (size_t k = 0; k < len; k++) {
        int h1, h2;
        if (line[k] == '%' && k + 2 < len &&
            (h1 = hexval(line[k + 1])) >= 0 && (h2 = hexval(line[k + 2])) >= 0) {
          char c = (char)(h1 * 16 + h2);
          buf_append(&b, &c, 1);
          k += 2;
        } else {
          buf_append(&b, line + k, 1);
        }
      }
    }
    i = e;
    while (i < n && (s[i] == '\n' || s[i] == '\r')) i++;
  }
  if (!b.data) buf_append(&b, "", 0);
  return buf_steal(&b, NULL);
}

void human_size(uint64_t bytes, char out[32])
{
  if (bytes < 1024) snprintf(out, 32, "%llu B", (unsigned long long)bytes);
  else if (bytes < 1024 * 1024) snprintf(out, 32, "%.1f KB", bytes / 1024.0);
  else if (bytes < 1024ull * 1024 * 1024) snprintf(out, 32, "%.1f MB", bytes / (1024.0 * 1024));
  else snprintf(out, 32, "%.1f GB", bytes / (1024.0 * 1024 * 1024));
}
