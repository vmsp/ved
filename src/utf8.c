// Copyright (c) 2026 Vitor Manuel de Sousa Pereira <vmsousapereira@gmail.com>
// SPDX-License-Identifier: MIT

#include "utf8.h"

bool utf8_is_continuation(unsigned char ch) {
  return (ch & 0xc0) == 0x80;
}

size_t utf8_next_boundary(const char *line, size_t len, size_t col) {
  if (col >= len) {
    return len;
  }
  col++;
  while (col < len && utf8_is_continuation((unsigned char)line[col])) {
    col++;
  }
  return col;
}

size_t utf8_prev_boundary(const char *line, size_t col) {
  if (col == 0) {
    return 0;
  }
  col--;
  while (col > 0 && utf8_is_continuation((unsigned char)line[col])) {
    col--;
  }
  return col;
}

size_t utf8_clamp_col(const char *line, size_t len, size_t col) {
  if (col > len) {
    col = len;
  }
  if (col > 0 && col < len &&
      utf8_is_continuation((unsigned char)line[col])) {
    col = utf8_prev_boundary(line, col);
  }
  return col;
}
