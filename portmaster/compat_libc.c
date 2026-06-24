// compat_libc.c — BSD libc functions that newlib (devkitA64) provides but glibc
// does not. Linked into the Linux/PortMaster build only.
#include <stddef.h>

// strlcpy: size-bounded copy that always NUL-terminates; returns src length.
size_t strlcpy(char *dst, const char *src, size_t size) {
  size_t srclen = 0;
  while (src[srclen]) srclen++;
  if (size) {
    size_t n = (srclen < size - 1) ? srclen : size - 1;
    for (size_t i = 0; i < n; i++) dst[i] = src[i];
    dst[n] = '\0';
  }
  return srclen;
}
