/* Deriving searchable and displayable text from clip payloads. */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* One line for the list: whitespace runs (including newlines) collapse to a
 * single space, leading/trailing space is trimmed, at most max_chars code
 * points. Input must be valid UTF-8. */
char *text_preview(const char *s, size_t n, size_t max_chars);

/* Visible text of an HTML fragment: tags dropped, <script>/<style> bodies
 * dropped, block-level tags become newlines, common entities decoded.
 * Output is valid UTF-8 when the input is. */
char *html_to_text(const char *s, size_t n);

/* "file:///a/b%20c.txt\r\nfile:///d" → "/a/b c.txt\n/d". Comment lines
 * (starting with #) are skipped. */
char *uri_list_to_paths(const char *s, size_t n);

/* "1.2 MB" style size. out must hold 32 bytes. */
void human_size(uint64_t bytes, char out[32]);
