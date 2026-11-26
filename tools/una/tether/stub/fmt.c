/* SPDX-FileCopyrightText: 2026 The unapebble authors */
/* SPDX-License-Identifier: Apache-2.0 */

/* A small vsnprintf: %d %i %u %x %X %o %p %s %c %%, flags - 0 + space, width, l, ll, z */

#include "stub.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

struct out {
  char *buf;
  size_t size;
  size_t n;
};

static void prv_put(struct out *o, char c) {
  if (o->n + 1U < o->size) {
    o->buf[o->n] = c;
  }
  o->n++;
}

static void prv_pad(struct out *o, char c, int n) {
  while (n-- > 0) {
    prv_put(o, c);
  }
}

int fmt_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
  struct out o = {buf, size, 0U};

  for (; *fmt != '\0'; fmt++) {
    if (*fmt != '%') {
      prv_put(&o, *fmt);
      continue;
    }
    fmt++;
    bool left = false, zero = false, plus = false, space = false;
    for (;; fmt++) {
      if (*fmt == '-') {
        left = true;
      } else if (*fmt == '0') {
        zero = true;
      } else if (*fmt == '+') {
        plus = true;
      } else if (*fmt == ' ') {
        space = true;
      } else {
        break;
      }
    }
    int width = 0;
    if (*fmt == '*') {
      width = va_arg(ap, int);
      fmt++;
    } else {
      while (*fmt >= '0' && *fmt <= '9') {
        width = width * 10 + (*fmt++ - '0');
      }
    }
    int prec = -1;
    if (*fmt == '.') {
      fmt++;
      prec = 0;
      while (*fmt >= '0' && *fmt <= '9') {
        prec = prec * 10 + (*fmt++ - '0');
      }
    }
    int lng = 0;
    while (*fmt == 'l') {
      lng++;
      fmt++;
    }
    if (*fmt == 'z' || *fmt == 'h') {
      fmt++;
    }

    char tmp[24];
    const char *s = tmp;
    int len = 0;
    char sign = 0;
    switch (*fmt) {
      case 'c':
        tmp[0] = (char)va_arg(ap, int);
        len = 1;
        break;
      case 's':
        s = va_arg(ap, const char *);
        if (s == NULL) {
          s = "(null)";
        }
        while (s[len] != '\0' && (prec < 0 || len < prec)) {
          len++;
        }
        break;
      case 'd':
      case 'i':
      case 'u':
      case 'x':
      case 'X':
      case 'o':
      case 'p': {
        unsigned long long v;
        unsigned base = 10U;
        const char *digits = "0123456789abcdef";
        if (*fmt == 'p') {
          v = (uintptr_t)va_arg(ap, void *);
          base = 16U;
          zero = true;
          width = 10;
          tmp[0] = '0';
        } else if (*fmt == 'd' || *fmt == 'i') {
          long long sv = lng >= 2 ? va_arg(ap, long long)
                         : lng    ? va_arg(ap, long)
                                  : va_arg(ap, int);
          if (sv < 0) {
            sign = '-';
            v = (unsigned long long)(-(sv + 1)) + 1U;
          } else {
            sign = plus ? '+' : space ? ' ' : 0;
            v = (unsigned long long)sv;
          }
        } else {
          v = lng >= 2 ? va_arg(ap, unsigned long long)
              : lng    ? va_arg(ap, unsigned long)
                       : va_arg(ap, unsigned int);
          base = (*fmt == 'o') ? 8U : (*fmt == 'u') ? 10U : 16U;
          if (*fmt == 'X') {
            digits = "0123456789ABCDEF";
          }
        }
        char rev[24];
        int n = 0;
        do {
          rev[n++] = digits[v % base];
          v /= base;
        } while (v != 0U);
        if (*fmt == 'p') {
          tmp[1] = 'x';
          len = 2;
          for (int i = n; i < 8; i++) {
            tmp[len++] = '0';
          }
          width = 0;
        }
        while (n > 0) {
          tmp[len++] = rev[--n];
        }
        break;
      }
      case '%':
        tmp[0] = '%';
        len = 1;
        break;
      case '\0':
        fmt--;
        continue;
      default:
        tmp[0] = '%';
        tmp[1] = *fmt;
        len = 2;
        break;
    }
    int pad = width - len - (sign ? 1 : 0);
    if (!left && !zero) {
      prv_pad(&o, ' ', pad);
    }
    if (sign) {
      prv_put(&o, sign);
    }
    if (!left && zero) {
      prv_pad(&o, '0', pad);
    }
    for (int i = 0; i < len; i++) {
      prv_put(&o, s[i]);
    }
    if (left) {
      prv_pad(&o, ' ', pad);
    }
  }
  if (o.size != 0U) {
    o.buf[o.n < o.size ? o.n : o.size - 1U] = '\0';
  }
  return (int)o.n;
}

int fmt_snprintf(char *buf, size_t size, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = fmt_vsnprintf(buf, size, fmt, ap);
  va_end(ap);
  return n;
}
