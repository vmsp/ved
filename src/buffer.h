// Copyright (c) 2026 Vitor Manuel de Sousa Pereira <vmsousapereira@gmail.com>
// SPDX-License-Identifier: MIT

#ifndef VED_BUFFER_H
#define VED_BUFFER_H

#include <stddef.h>
#include <stdbool.h>

#include "tree_sitter/api.h"

typedef struct {
  char **lines;
  size_t line_count;
  bool syntax_enabled;
  bool syntax_dirty;
  TSParser *ts_parser;
  TSTree *ts_tree;
  TSQuery *ts_query;
} Buffer;

void buffer_init(Buffer *buf);
void buffer_free(Buffer *buf);
void buffer_clear(Buffer *buf);
void buffer_append_line(Buffer *buf, const char *line);
void buffer_load_file(Buffer *buf, const char *path);
void buffer_init_syntax(Buffer *buf, const char *path);
size_t line_length(Buffer *buf, size_t row);
bool buffer_write_file(Buffer *buf, const char *path);

void buffer_insert_char(Buffer *buf, size_t row, size_t col, char ch);
void buffer_insert_newline(Buffer *buf, size_t row, size_t col);
void buffer_delete_char(Buffer *buf, size_t row, size_t col);
void buffer_delete_forward(Buffer *buf, size_t row, size_t col);
void buffer_delete_range(Buffer *buf, size_t row, size_t start, size_t end);
void buffer_delete_region(Buffer *buf, size_t start_row, size_t start_col,
                          size_t end_row, size_t end_col);

#endif
