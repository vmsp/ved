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

static size_t tab_width_at(size_t vis_col) {
  return 8 - (vis_col % 8);
}

size_t utf8_byte_to_vis_col(const char *line, size_t byte_col) {
  size_t vis_col = 0;
  for (size_t i = 0; i < byte_col && line[i] != '\0'; i++) {
    if (line[i] == '\t') {
      vis_col += tab_width_at(vis_col);
    } else {
      vis_col++;
    }
  }
  return vis_col;
}

size_t utf8_vis_to_byte_col(const char *line, size_t len, size_t vis_col) {
  size_t current_vis = 0;
  size_t byte_col = 0;
  while (byte_col < len && current_vis < vis_col) {
    if (line[byte_col] == '\t') {
      size_t char_width = tab_width_at(current_vis);
      if (current_vis + char_width > vis_col) {
        break;
      }
      current_vis += char_width;
    } else {
      current_vis++;
    }
    byte_col++;
  }
  return byte_col;
}
