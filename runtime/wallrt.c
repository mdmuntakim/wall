/* wallrt.c - freestanding Wall runtime.
 *
 * This object is linked into every Wall executable by the Wall linker
 * (src/linker/linker.c).  It contains no libc dependency: every operation is
 * implemented with Linux AArch64 system calls directly.
 *
 * Provided symbols:
 *   _start        (see start.S) program entry
 *   printf        formatted output subset: %d %i %u %x %X %c %s %p %%
 *   puts, putchar
 *   write, exit
 *   strlen, strcmp, memcpy, memset, memmove, memcmp, bzero
 *
 * Build:
 *   clang --target=aarch64-unknown-linux-android24 -ffreestanding -nostdlib \
 *         -fno-builtin -fno-stack-protector -fno-asynchronous-unwind-tables \
 *         -fno-unwind-tables -O2 -c wallrt.c -o wallrt.o
 */
typedef unsigned long usize;
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type) __builtin_va_arg(ap, type)
#define va_end(ap) __builtin_va_end(ap)
#define va_copy(d, s) __builtin_va_copy(d, s)

/* ------------------------------------------------------------------------- */
/* System calls (Linux AArch64)                                              */
/* ------------------------------------------------------------------------- */
static long sys3(long n, long a, long b, long c) {
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x8 __asm__("x8") = n;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}

static long sys1(long n, long a) {
  register long x0 __asm__("x0") = a;
  register long x8 __asm__("x8") = n;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
  return x0;
}

long write(int fd, const void *buf, usize len) {
  return sys3(64, (long)fd, (long)buf, (long)len);
}

void _exit(int code) {
  (void)sys1(93, (long)code);
  for (;;) { }
}

void exit(int code) {
  _exit(code);
}

/* ------------------------------------------------------------------------- */
/* String primitives                                                          */
/* ------------------------------------------------------------------------- */
usize strlen(const char *s) {
  usize n = 0;
  while (s && s[n]) n++;
  return n;
}

int strcmp(const char *a, const char *b) {
  while (*a && *a == *b) { a++; b++; }
  return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, usize n) {
  while (n && *a && *a == *b) { a++; b++; n--; }
  if (!n) return 0;
  return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

void *memcpy(void *dst, const void *src, usize n) {
  unsigned char *d = (unsigned char *)dst;
  const unsigned char *s = (const unsigned char *)src;
  while (n--) *d++ = *s++;
  return dst;
}

void *memmove(void *dst, const void *src, usize n) {
  unsigned char *d = (unsigned char *)dst;
  const unsigned char *s = (const unsigned char *)src;
  if (d < s) { while (n--) *d++ = *s++; }
  else { d += n; s += n; while (n--) *--d = *--s; }
  return dst;
}

void *memset(void *dst, int c, usize n) {
  unsigned char *d = (unsigned char *)dst;
  while (n--) *d++ = (unsigned char)c;
  return dst;
}

void bzero(void *dst, usize n) { memset(dst, 0, n); }

int memcmp(const void *a, const void *b, usize n) {
  const unsigned char *x = (const unsigned char *)a;
  const unsigned char *y = (const unsigned char *)b;
  while (n--) {
    if (*x != *y) return (int)*x - (int)*y;
    x++; y++;
  }
  return 0;
}

/* ------------------------------------------------------------------------- */
/* Formatted output                                                           */
/* ------------------------------------------------------------------------- */
static int out(const char *s, usize n) {
  return (int)write(1, s, n);
}

static int out_num(unsigned long v, int base, int is_signed, int neg) {
  char tmp[64];
  char buf[64];
  int i = 0, j = 0;
  if (v == 0) tmp[i++] = '0';
  while (v) {
    int d = (int)(v % (unsigned long)base);
    tmp[i++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    v /= (unsigned long)base;
  }
  if (is_signed && neg) buf[j++] = '-';
  while (i > 0) buf[j++] = tmp[--i];
  return out(buf, (usize)j);
}

int printf(const char *fmt, ...) {
  va_list ap;
  int total = 0;
  va_start(ap, fmt);
  if (!fmt) { va_end(ap); return 0; }
  for (const char *p = fmt; *p; p++) {
    if (*p != '%') { total += out(p, 1); continue; }
    p++;
    if (*p == 0) { break; }
    if (*p == '%') { total += out("%", 1); continue; }
    int lng = 0;
    while (*p == 'l' || *p == 'z' || *p == 'h') { lng = 1; p++; }
    if (*p == 'd' || *p == 'i') {
      if (lng) { long v = va_arg(ap, long); total += out_num(v < 0 ? (unsigned long)(-v) : (unsigned long)v, 10, 1, v < 0); }
      else { int v = va_arg(ap, int); total += out_num(v < 0 ? (unsigned long)(-v) : (unsigned long)v, 10, 1, v < 0); }
    } else if (*p == 'u') {
      unsigned long v = lng ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
      total += out_num(v, 10, 0, 0);
    } else if (*p == 'x' || *p == 'X') {
      unsigned long v = lng ? va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
      total += out_num(v, 16, 0, 0);
    } else if (*p == 'c') {
      int c = va_arg(ap, int);
      char ch = (char)c;
      total += out(&ch, 1);
    } else if (*p == 's') {
      const char *s = va_arg(ap, const char *);
      if (!s) s = "(null)";
      total += out(s, strlen(s));
    } else if (*p == 'p') {
      void *v = va_arg(ap, void *);
      total += out("0x", 2);
      total += out_num((unsigned long)v, 16, 0, 0);
    } else {
      total += out(p, 1);
    }
  }
  va_end(ap);
  return total;
}

int puts(const char *s) {
  int n = (int)strlen(s);
  n += out(s, (usize)n);
  n += out("\n", 1);
  return n;
}

int putchar(int c) {
  char ch = (char)c;
  return out(&ch, 1);
}
