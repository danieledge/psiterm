/* EPOC R5 estlib has vsprintf but no vsnprintf/snprintf. libvterm only formats
   short escape sequences, so format into a scratch buffer and truncate. */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stddef.h>

int vsnprintf(char *str, size_t size, const char *fmt, va_list ap)
{
  char tmp[1024];
  int n = vsprintf(tmp, fmt, ap);
  if (size > 0) {
    size_t c = (n < 0) ? 0 : (size_t)n;
    if (c >= size) c = size - 1;
    memcpy(str, tmp, c);
    str[c] = 0;
  }
  return n;
}

int snprintf(char *str, size_t size, const char *fmt, ...)
{
  va_list ap; int n;
  va_start(ap, fmt);
  n = vsnprintf(str, size, fmt, ap);
  va_end(ap);
  return n;
}
