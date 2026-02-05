// Copyright (c) 2026 Vitor Manuel de Sousa Pereira <vmsousapereira@gmail.com>
// SPDX-License-Identifier: MIT

#include "util.h"

#include <stdio.h>
#include <stdlib.h>

char *base64_encode(const char *data, size_t len) {
  static const char table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t out_len = ((len + 2) / 3) * 4;
  char *out = calloc(out_len + 1, 1);
  if (!out) {
    die("calloc");
  }
  size_t oi = 0;
  for (size_t i = 0; i < len; i += 3) {
    unsigned char a = (unsigned char)data[i];
    unsigned char b = (i + 1 < len) ? (unsigned char)data[i + 1] : 0;
    unsigned char c = (i + 2 < len) ? (unsigned char)data[i + 2] : 0;
    unsigned triple = (unsigned)(a << 16) | (unsigned)(b << 8) | c;
    out[oi++] = table[(triple >> 18) & 0x3f];
    out[oi++] = table[(triple >> 12) & 0x3f];
    out[oi++] = (i + 1 < len) ? table[(triple >> 6) & 0x3f] : '=';
    out[oi++] = (i + 2 < len) ? table[triple & 0x3f] : '=';
  }
  out[oi] = '\0';
  return out;
}


void die(const char *msg) {
  perror(msg);
  exit(EXIT_FAILURE);
}
