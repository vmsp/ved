// Copyright (c) 2026 Vitor Manuel de Sousa Pereira <vmsousapereira@gmail.com>
// SPDX-License-Identifier: MIT

#include "editor_internal.h"

#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "buffer.h"
#include "utf8.h"
#include "util.h"

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} AppendBuf;

static void ab_init(AppendBuf *ab) {
  ab->data = NULL;
  ab->len = 0;
  ab->cap = 0;
}

static void ab_free(AppendBuf *ab) {
  free(ab->data);
  ab->data = NULL;
  ab->len = 0;
  ab->cap = 0;
}

static void ab_append(AppendBuf *ab, const char *data, size_t len) {
  if (len == 0) {
    return;
  }
  size_t needed = ab->len + len + 1;
  if (needed > ab->cap) {
    size_t next = ab->cap ? ab->cap * 2 : 256;
    while (next < needed) {
      next *= 2;
    }
    char *buf = realloc(ab->data, next);
    if (!buf) {
      die("realloc");
    }
    ab->data = buf;
    ab->cap = next;
  }
  memcpy(ab->data + ab->len, data, len);
  ab->len += len;
  ab->data[ab->len] = '\0';
}

static void ab_append_str(AppendBuf *ab, const char *str) {
  ab_append(ab, str, strlen(str));
}

static void ab_append_char(AppendBuf *ab, char ch) {
  ab_append(ab, &ch, 1);
}

static void write_all(int fd, const char *data, size_t len) {
  size_t offset = 0;
  while (offset < len) {
    ssize_t written = write(fd, data + offset, len - offset);
    if (written > 0) {
      offset += (size_t)written;
      continue;
    }
    if (written == -1 && errno == EINTR) {
      continue;
    }
    if (written == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      struct pollfd pfd = {
        .fd = fd,
        .events = POLLOUT
      };
      poll(&pfd, 1, -1);
      continue;
    }
    break;
  }
}

static bool selection_range_for_row(const Editor *ed, size_t row,
                                    size_t *start, size_t *end) {
  if (!ed->mark_active) {
    return false;
  }
  size_t row_a = ed->mark_row;
  size_t col_a = ed->mark_col;
  size_t row_b = ed->frame.row;
  size_t col_b = ed->frame.col;

  if (pos_compare(row_a, col_a, row_b, col_b) > 0) {
    size_t tmp_row = row_a;
    size_t tmp_col = col_a;
    row_a = row_b;
    col_a = col_b;
    row_b = tmp_row;
    col_b = tmp_col;
  }

  if (row < row_a || row > row_b) {
    return false;
  }
  if (row == row_a && row == row_b) {
    *start = col_a;
    *end = col_b;
    return col_a != col_b;
  }
  if (row == row_a) {
    *start = col_a;
    *end = SIZE_MAX;
    return true;
  }
  if (row == row_b) {
    *start = 0;
    *end = col_b;
    return col_b != 0;
  }
  *start = 0;
  *end = SIZE_MAX;
  return true;
}

static void editor_scroll(Editor *ed) {
  Frame *frame = &ed->frame;
  size_t text_rows = 0;
  if (ed->screen_rows > 1) {
    text_rows = (size_t)ed->screen_rows - 1;
  }
  if (frame->row < ed->row_offset) {
    ed->row_offset = frame->row;
  }
  if (text_rows > 0 &&
      frame->row >= ed->row_offset + text_rows) {
    ed->row_offset = frame->row - text_rows + 1;
  }
  if (frame->col < ed->col_offset) {
    ed->col_offset = frame->col;
  }
  if (frame->col >= ed->col_offset + (size_t)ed->screen_cols) {
    ed->col_offset = frame->col - ed->screen_cols + 1;
  }
  if (frame->row < ed->buffer.line_count) {
    const char *line = ed->buffer.lines[frame->row];
    size_t len = line_length(&ed->buffer, frame->row);
    ed->col_offset = utf8_clamp_col(line, len, ed->col_offset);
  }
}

void editor_refresh_screen(Editor *ed) {
  Frame *frame = &ed->frame;
  char buf[64];
  AppendBuf ab;

  editor_scroll(ed);

  ab_init(&ab);
  ab_append_str(&ab, "\x1b[?25l");
  ab_append_str(&ab, "\x1b[H");

  int text_rows = ed->screen_rows > 1 ? ed->screen_rows - 1 : 0;
  for (int y = 0; y < text_rows; y++) {
    size_t file_row = ed->row_offset + (size_t)y;
    if (file_row >= ed->buffer.line_count) {
      ab_append_char(&ab, '~');
    } else {
      const char *line = ed->buffer.lines[file_row];
      size_t len = strlen(line);
      if (ed->col_offset < len) {
        line += ed->col_offset;
        len -= ed->col_offset;
      } else {
        len = 0;
      }
      if (len > (size_t)ed->screen_cols) {
        len = ed->screen_cols;
      }
      if (len > 0) {
        size_t sel_start = 0;
        size_t sel_end = 0;
        bool has_sel = selection_range_for_row(ed, file_row, &sel_start,
                                               &sel_end);
        if (!has_sel) {
          ab_append(&ab, line, len);
        } else {
          size_t line_len = strlen(ed->buffer.lines[file_row]);
          if (sel_end > line_len) {
            sel_end = line_len;
          }
          if (sel_start > line_len) {
            sel_start = line_len;
          }
          if (sel_end < sel_start) {
            sel_end = sel_start;
          }
          if (sel_end <= ed->col_offset ||
              sel_start >= ed->col_offset + len) {
            ab_append(&ab, line, len);
          } else {
            size_t vis_start = ed->col_offset;
            size_t vis_end = ed->col_offset + len;
            size_t draw_start = sel_start > vis_start ? sel_start : vis_start;
            size_t draw_end = sel_end < vis_end ? sel_end : vis_end;
            size_t before_len = draw_start - vis_start;
            size_t sel_len = draw_end - draw_start;
            size_t after_len = vis_end - draw_end;

            if (before_len > 0) {
              ab_append(&ab, line, before_len);
            }
            if (sel_len > 0) {
              ab_append_str(&ab, "\x1b[7m");
              ab_append(&ab, line + before_len, sel_len);
              ab_append_str(&ab, "\x1b[m");
            }
            if (after_len > 0) {
              ab_append(&ab, line + before_len + sel_len, after_len);
            }
          }
        }
      }
    }
    ab_append_str(&ab, "\x1b[K");
    if (y < ed->screen_rows - 1) {
      ab_append_str(&ab, "\r\n");
    }
  }

  ab_append_str(&ab, "\x1b[7m");
  {
    char status[256];
    if (ed->prompt_active) {
      snprintf(status, sizeof(status), " Save as: %s", ed->prompt_buf);
    } else {
      const char *name = ed->file_path ? ed->file_path : "[No Name]";
      const char *dirty = ed->dirty ? "*" : "";
      snprintf(status, sizeof(status), " %s%s | %zu lines | %s",
               name, dirty, ed->buffer.line_count, ed->status_msg);
    }
    size_t len = strlen(status);
    if (len > (size_t)ed->screen_cols) {
      len = ed->screen_cols;
    }
    if (len > 0) {
      ab_append(&ab, status, len);
    }
    if (len < (size_t)ed->screen_cols) {
      for (int i = 0; i < ed->screen_cols - (int)len; i++) {
        ab_append_char(&ab, ' ');
      }
    }
  }
  ab_append_str(&ab, "\x1b[m");

  int cursor_row = (int)(frame->row - ed->row_offset) + 1;
  int cursor_col = (int)(frame->col - ed->col_offset) + 1;
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", cursor_row, cursor_col);
  ab_append_str(&ab, buf);
  ab_append_str(&ab, "\x1b[?25h");
  write_all(STDOUT_FILENO, ab.data, ab.len);
  ab_free(&ab);
}
