// Copyright (c) 2026 Vitor Manuel de Sousa Pereira <vmsousapereira@gmail.com>
// SPDX-License-Identifier: MIT

#include "buffer.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "utf8.h"
#include "util.h"

extern const TSLanguage *tree_sitter_c(void);

static const unsigned char C_HIGHLIGHT_QUERY[] = {
#embed "../vendor/tree-sitter-c/queries/highlights.scm"
  , 0
};

static bool has_c_extension(const char *path) {
  if (!path) {
    return false;
  }
  const char *dot = strrchr(path, '.');
  if (!dot) {
    return false;
  }
  return strcmp(dot, ".c") == 0 || strcmp(dot, ".h") == 0;
}

static bool is_makefile(const char *path) {
  if (!path) {
    return false;
  }
  const char *slash = strrchr(path, '/');
  const char *filename = slash ? slash + 1 : path;
  return strcmp(filename, "makefile") == 0 ||
         strcmp(filename, "Makefile") == 0;
}

BufferMode detect_mode(const char *path) {
  if (is_makefile(path)) {
    return MAKEFILE_MODE;
  }
  return C_MODE;
}

void buffer_init(Buffer *buf) {
  buf->line_count = 1;
  buf->lines = calloc(buf->line_count, sizeof(*buf->lines));
  if (!buf->lines) {
    die("calloc");
  }
  buf->lines[0] = strdup("");
  if (!buf->lines[0]) {
    die("strdup");
  }
  buf->file_path = NULL;
  buf->mode = C_MODE;
  buf->syntax_enabled = false;
  buf->syntax_dirty = false;
  buf->ts_parser = NULL;
  buf->ts_tree = NULL;
  buf->ts_query = NULL;
}

void buffer_free(Buffer *buf) {
  for (size_t i = 0; i < buf->line_count; i++) {
    free(buf->lines[i]);
  }
  free(buf->lines);
  free(buf->file_path);
  if (buf->ts_tree) {
    ts_tree_delete(buf->ts_tree);
  }
  if (buf->ts_query) {
    ts_query_delete(buf->ts_query);
  }
  if (buf->ts_parser) {
    ts_parser_delete(buf->ts_parser);
  }
}

void buffer_clear(Buffer *buf) {
  buffer_free(buf);
  buffer_init(buf);
}

void buffer_append_line(Buffer *buf, const char *line) {
  char **lines = realloc(buf->lines,
                         sizeof(*buf->lines) * (buf->line_count + 1));
  if (!lines) {
    die("realloc");
  }
  buf->lines = lines;
  buf->lines[buf->line_count] = strdup(line);
  if (!buf->lines[buf->line_count]) {
    die("strdup");
  }
  buf->line_count++;
}

void buffer_load_file(Buffer *buf, const char *path) {
  FILE *file = fopen(path, "rb");
  if (!file) {
    return;
  }

  buffer_clear(buf);
  free(buf->lines[0]);
  buf->line_count = 0;

  char *line = NULL;
  size_t cap = 0;
  ssize_t len = 0;
  while ((len = getline(&line, &cap, file)) != -1) {
    while (len > 0 &&
           (line[len - 1] == '\n' || line[len - 1] == '\r')) {
      line[--len] = '\0';
    }
    buffer_append_line(buf, line);
  }
  free(line);
  fclose(file);

  if (buf->line_count == 0) {
    buffer_append_line(buf, "");
  }

  free(buf->file_path);
  buf->file_path = strdup(path);
  if (!buf->file_path) {
    die("strdup");
  }
  buf->mode = detect_mode(path);
}

void buffer_init_syntax(Buffer *buf, const char *path) {
  buf->syntax_enabled = has_c_extension(path);
  buf->syntax_dirty = buf->syntax_enabled;
  if (!buf->syntax_enabled) {
    return;
  }
  const char *query_source = (const char *)C_HIGHLIGHT_QUERY;
  size_t query_len = sizeof(C_HIGHLIGHT_QUERY) - 1;
  buf->ts_parser = ts_parser_new();
  if (!buf->ts_parser || !ts_parser_set_language(buf->ts_parser,
                                                 tree_sitter_c())) {
    buf->syntax_enabled = false;
    return;
  }
  uint32_t error_offset = 0;
  TSQueryError error_type = TSQueryErrorNone;
  buf->ts_query = ts_query_new(tree_sitter_c(), query_source, query_len,
                               &error_offset, &error_type);
  if (!buf->ts_query || error_type != TSQueryErrorNone) {
    buf->syntax_enabled = false;
  }
}

size_t line_length(Buffer *buf, size_t row) {
  if (row >= buf->line_count) {
    return 0;
  }
  return strlen(buf->lines[row]);
}

bool buffer_write_file(Buffer *buf, const char *path) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd == -1) {
    return false;
  }
  for (size_t i = 0; i < buf->line_count; i++) {
    size_t len = strlen(buf->lines[i]);
    if (len > 0) {
      if (write(fd, buf->lines[i], len) != (ssize_t)len) {
        close(fd);
        return false;
      }
    }
    if (i + 1 < buf->line_count) {
      if (write(fd, "\n", 1) != 1) {
        close(fd);
        return false;
      }
    }
  }
  if (close(fd) == -1) {
    return false;
  }
  return true;
}

void buffer_insert_char(Buffer *buf, size_t row, size_t col, char ch) {
  if (row >= buf->line_count) {
    return;
  }
  size_t len = line_length(buf, row);
  if (col > len) {
    col = len;
  }
  char *line = buf->lines[row];
  char *updated = realloc(line, len + 2);
  if (!updated) {
    die("realloc");
  }
  memmove(updated + col + 1, updated + col, len - col + 1);
  updated[col] = ch;
  buf->lines[row] = updated;
}

void buffer_insert_newline(Buffer *buf, size_t row, size_t col) {
  if (row >= buf->line_count) {
    return;
  }
  size_t len = line_length(buf, row);
  if (col > len) {
    col = len;
  }
  char *line = buf->lines[row];
  char *left = strndup(line, col);
  char *right = strdup(line + col);
  if (!left || !right) {
    die("strdup");
  }
  free(line);
  buf->lines[row] = left;

  char **lines = realloc(buf->lines,
                         sizeof(*buf->lines) * (buf->line_count + 1));
  if (!lines) {
    die("realloc");
  }
  buf->lines = lines;
  memmove(&buf->lines[row + 2], &buf->lines[row + 1],
          sizeof(*buf->lines) * (buf->line_count - row - 1));
  buf->lines[row + 1] = right;
  buf->line_count++;
}

void buffer_insert_tab(Buffer *buf, size_t row, size_t col) {
  if (buf->mode == MAKEFILE_MODE) {
    buffer_insert_char(buf, row, col, '\t');
  } else {
    buffer_insert_char(buf, row, col, ' ');
    buffer_insert_char(buf, row, col + 1, ' ');
  }
}

void buffer_delete_char(Buffer *buf, size_t row, size_t col) {
  if (row >= buf->line_count) {
    return;
  }
  if (col == 0) {
    if (row == 0) {
      return;
    }
    size_t prev_len = line_length(buf, row - 1);
    size_t cur_len = line_length(buf, row);
    char *merged = realloc(buf->lines[row - 1], prev_len + cur_len + 1);
    if (!merged) {
      die("realloc");
    }
    memcpy(merged + prev_len, buf->lines[row], cur_len + 1);
    free(buf->lines[row]);
    buf->lines[row - 1] = merged;
    memmove(&buf->lines[row], &buf->lines[row + 1],
            sizeof(*buf->lines) * (buf->line_count - row - 1));
    buf->line_count--;
    return;
  }
  size_t len = line_length(buf, row);
  if (col > len) {
    col = len;
  }
  char *line = buf->lines[row];
  size_t start = utf8_prev_boundary(line, col);
  memmove(line + start, line + col, len - col + 1);
}

void buffer_delete_forward(Buffer *buf, size_t row, size_t col) {
  if (row >= buf->line_count) {
    return;
  }
  size_t len = line_length(buf, row);
  if (col > len) {
    col = len;
  }
  if (col == len) {
    if (row + 1 >= buf->line_count) {
      return;
    }
    size_t next_len = line_length(buf, row + 1);
    char *merged = realloc(buf->lines[row], len + next_len + 1);
    if (!merged) {
      die("realloc");
    }
    memcpy(merged + len, buf->lines[row + 1], next_len + 1);
    free(buf->lines[row + 1]);
    buf->lines[row] = merged;
    memmove(&buf->lines[row + 1], &buf->lines[row + 2],
            sizeof(*buf->lines) * (buf->line_count - row - 2));
    buf->line_count--;
    return;
  }
  char *line = buf->lines[row];
  size_t next = utf8_next_boundary(line, len, col);
  memmove(line + col, line + next, len - next + 1);
}

void buffer_delete_range(Buffer *buf, size_t row, size_t start, size_t end) {
  if (row >= buf->line_count) {
    return;
  }
  size_t len = line_length(buf, row);
  if (start > len) {
    start = len;
  }
  if (end > len) {
    end = len;
  }
  if (end <= start) {
    return;
  }
  char *line = buf->lines[row];
  memmove(line + start, line + end, len - end + 1);
}

void buffer_delete_region(Buffer *buf, size_t start_row, size_t start_col,
                          size_t end_row, size_t end_col) {
  if (start_row >= buf->line_count || end_row >= buf->line_count) {
    return;
  }
  if (start_row > end_row ||
      (start_row == end_row && start_col > end_col)) {
    size_t tmp_row = start_row;
    size_t tmp_col = start_col;
    start_row = end_row;
    start_col = end_col;
    end_row = tmp_row;
    end_col = tmp_col;
  }
  if (start_row == end_row) {
    buffer_delete_range(buf, start_row, start_col, end_col);
    return;
  }

  size_t start_len = line_length(buf, start_row);
  if (start_col > start_len) {
    start_col = start_len;
  }
  size_t end_len = line_length(buf, end_row);
  if (end_col > end_len) {
    end_col = end_len;
  }

  char *end_tail = strdup(buf->lines[end_row] + end_col);
  if (!end_tail) {
    die("strdup");
  }
  char *start_line = buf->lines[start_row];
  start_line[start_col] = '\0';
  char *merged = realloc(start_line, start_col + strlen(end_tail) + 1);
  if (!merged) {
    die("realloc");
  }
  memcpy(merged + start_col, end_tail, strlen(end_tail) + 1);
  free(end_tail);
  buf->lines[start_row] = merged;

  for (size_t i = start_row + 1; i <= end_row; i++) {
    free(buf->lines[i]);
  }
  size_t remove_count = end_row - start_row;
  memmove(&buf->lines[start_row + 1], &buf->lines[end_row + 1],
          sizeof(*buf->lines) * (buf->line_count - end_row - 1));
  buf->line_count -= remove_count;
}
