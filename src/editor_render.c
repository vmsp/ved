// Copyright (c) 2026 Vitor Manuel de Sousa Pereira <vmsousapereira@gmail.com>
// SPDX-License-Identifier: MIT

#include "editor.h"

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

typedef struct {
  size_t start;
  size_t end;
  const char *color;
} HighlightSpan;

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

static const char *capture_color(const char *name) {
  if (strcmp(name, "comment") == 0) {
    return "\x1b[37m"; // White
  }
  if (strcmp(name, "string") == 0) {
    return "\x1b[32m"; // Green
  }
  if (strcmp(name, "number") == 0) {
    return "\x1b[33m"; // Yellow
  }
  if (strcmp(name, "type") == 0) {
    return "\x1b[33m"; // Yellow
  }
  if (strcmp(name, "keyword") == 0) {
    return "\x1b[35m"; // Purple
  }
  if (strcmp(name, "preproc") == 0) {
    return "\x1b[95m"; // High Intensity Purple
  }
  if (strcmp(name, "function") == 0) {
    return "\x1b[34m"; // Blue
  }
  if (strcmp(name, "operator") == 0 || strcmp(name, "delimiter") == 0) {
    return "\x1b[36m"; // Cyan
  }
  return NULL;
}

static char *build_buffer_text(Editor *ed, size_t *out_len) {
  size_t total = 0;
  for (size_t i = 0; i < ed->buffer.line_count; i++) {
    total += line_length(&ed->buffer, i);
    if (i + 1 < ed->buffer.line_count) {
      total += 1;
    }
  }
  char *text = malloc(total + 1);
  if (!text) {
    die("malloc");
  }
  size_t offset = 0;
  for (size_t i = 0; i < ed->buffer.line_count; i++) {
    const char *line = ed->buffer.lines[i];
    size_t len = line_length(&ed->buffer, i);
    if (len > 0) {
      memcpy(text + offset, line, len);
      offset += len;
    }
    if (i + 1 < ed->buffer.line_count) {
      text[offset++] = '\n';
    }
  }
  text[offset] = '\0';
  *out_len = offset;
  return text;
}

static size_t *build_line_offsets(Editor *ed, size_t last_row) {
  size_t *offsets = calloc(last_row + 1, sizeof(*offsets));
  if (!offsets) {
    die("calloc");
  }
  size_t offset = 0;
  for (size_t row = 0; row <= last_row; row++) {
    offsets[row] = offset;
    offset += line_length(&ed->buffer, row);
    if (row + 1 < ed->buffer.line_count) {
      offset += 1;
    }
  }
  return offsets;
}

static HighlightSpan *collect_highlights(Editor *ed, size_t range_start,
                                         size_t range_end, size_t *out_count) {
  *out_count = 0;
  if (!ed->buffer.syntax_enabled || !ed->buffer.ts_tree ||
      !ed->buffer.ts_query) {
    return NULL;
  }
  TSQueryCursor *cursor = ts_query_cursor_new();
  if (!cursor) {
    return NULL;
  }
  ts_query_cursor_exec(cursor, ed->buffer.ts_query,
                       ts_tree_root_node(ed->buffer.ts_tree));

  HighlightSpan *spans = NULL;
  size_t span_count = 0;
  size_t span_cap = 0;
  TSQueryMatch match;
  while (ts_query_cursor_next_match(cursor, &match)) {
    for (uint32_t i = 0; i < match.capture_count; i++) {
      TSQueryCapture capture = match.captures[i];
      uint32_t name_len = 0;
      const char *name = ts_query_capture_name_for_id(ed->buffer.ts_query,
                                                      capture.index,
                                                      &name_len);
      if (!name) {
        continue;
      }
      char name_buf[32];
      if (name_len >= sizeof(name_buf)) {
        continue;
      }
      memcpy(name_buf, name, name_len);
      name_buf[name_len] = '\0';
      const char *color = capture_color(name_buf);
      if (!color) {
        continue;
      }
      size_t start = ts_node_start_byte(capture.node);
      size_t end = ts_node_end_byte(capture.node);
      if (end <= range_start || start >= range_end) {
        continue;
      }
      if (span_count == span_cap) {
        size_t next = span_cap ? span_cap * 2 : 64;
        HighlightSpan *next_spans = realloc(spans,
                                            sizeof(*spans) * next);
        if (!next_spans) {
          free(spans);
          ts_query_cursor_delete(cursor);
          die("realloc");
        }
        spans = next_spans;
        span_cap = next;
      }
      spans[span_count++] = (HighlightSpan){
        .start = start,
        .end = end,
        .color = color
      };
    }
  }
  ts_query_cursor_delete(cursor);
  *out_count = span_count;
  return spans;
}

static int span_compare(const void *a, const void *b) {
  const HighlightSpan *lhs = a;
  const HighlightSpan *rhs = b;
  if (lhs->start < rhs->start) {
    return -1;
  }
  if (lhs->start > rhs->start) {
    return 1;
  }
  if (lhs->end < rhs->end) {
    return -1;
  }
  if (lhs->end > rhs->end) {
    return 1;
  }
  return 0;
}

static void render_line_with_spans(AppendBuf *ab, const char *line,
                                   size_t line_start, size_t vis_start,
                                   size_t vis_len, HighlightSpan *spans,
                                   size_t span_count) {
  size_t vis_end = vis_start + vis_len;
  size_t cursor = vis_start;
  for (size_t i = 0; i < span_count; i++) {
    HighlightSpan span = spans[i];
    if (span.end <= cursor || span.start >= vis_end) {
      continue;
    }
    size_t seg_start = span.start > cursor ? span.start : cursor;
    size_t seg_end = span.end < vis_end ? span.end : vis_end;
    if (seg_start > cursor) {
      size_t offset = cursor - line_start;
      size_t len = seg_start - cursor;
      ab_append(ab, line + offset, len);
    }
    ab_append_str(ab, span.color);
    ab_append(ab, line + (seg_start - line_start), seg_end - seg_start);
    ab_append_str(ab, "\x1b[m");
    cursor = seg_end;
    if (cursor >= vis_end) {
      break;
    }
  }
  if (cursor < vis_end) {
    ab_append(ab, line + (cursor - line_start), vis_end - cursor);
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
  HighlightSpan *spans = NULL;
  size_t span_count = 0;
  size_t *offsets = NULL;
  size_t last_row = 0;

  editor_scroll(ed);

  if (ed->buffer.syntax_enabled && ed->buffer.ts_parser &&
      ed->buffer.ts_query) {
    if (ed->buffer.syntax_dirty || !ed->buffer.ts_tree) {
      size_t text_len = 0;
      char *text = build_buffer_text(ed, &text_len);
      TSTree *tree = ts_parser_parse_string(ed->buffer.ts_parser,
                                            ed->buffer.ts_tree,
                                            text, text_len);
      free(text);
      if (tree) {
        if (ed->buffer.ts_tree) {
          ts_tree_delete(ed->buffer.ts_tree);
        }
        ed->buffer.ts_tree = tree;
      }
      ed->buffer.syntax_dirty = false;
    }
  }

  ab_init(&ab);
  ab_append_str(&ab, "\x1b[?25l");
  ab_append_str(&ab, "\x1b[H");

  int text_rows = ed->screen_rows > 1 ? ed->screen_rows - 1 : 0;
  if (text_rows > 0 && ed->buffer.line_count > 0) {
    last_row = ed->row_offset + (size_t)text_rows - 1;
    if (last_row >= ed->buffer.line_count) {
      last_row = ed->buffer.line_count - 1;
    }
    offsets = build_line_offsets(ed, last_row);
    if (ed->buffer.syntax_enabled && ed->buffer.ts_tree &&
        ed->buffer.ts_query) {
      size_t range_start = offsets[ed->row_offset];
      size_t range_end = offsets[last_row] +
        line_length(&ed->buffer, last_row);
      spans = collect_highlights(ed, range_start, range_end, &span_count);
      if (span_count > 1) {
        qsort(spans, span_count, sizeof(*spans), span_compare);
      }
    }
  }
  for (int y = 0; y < text_rows; y++) {
    size_t file_row = ed->row_offset + (size_t)y;
    if (file_row >= ed->buffer.line_count) {
      ab_append_char(&ab, '~');
    } else {
      const char *line = ed->buffer.lines[file_row];
      size_t line_len = strlen(line);
      size_t vis_len = 0;
      if (ed->col_offset < line_len) {
        vis_len = line_len - ed->col_offset;
      }
      if (vis_len > (size_t)ed->screen_cols) {
        vis_len = ed->screen_cols;
      }
      if (vis_len > 0) {
        size_t sel_start = 0;
        size_t sel_end = 0;
        bool has_sel = selection_range_for_row(ed, file_row, &sel_start,
                                               &sel_end);
        if (!has_sel) {
          if (spans && offsets) {
            size_t line_start = offsets[file_row];
            size_t vis_start = line_start + ed->col_offset;
            render_line_with_spans(&ab, line, line_start, vis_start, vis_len,
                                   spans, span_count);
          } else {
            ab_append(&ab, line + ed->col_offset, vis_len);
          }
        } else {
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
              sel_start >= ed->col_offset + vis_len) {
            ab_append(&ab, line + ed->col_offset, vis_len);
          } else {
            size_t vis_start = ed->col_offset;
            size_t vis_end = ed->col_offset + vis_len;
            size_t draw_start = sel_start > vis_start ? sel_start : vis_start;
            size_t draw_end = sel_end < vis_end ? sel_end : vis_end;
            size_t before_len = draw_start - vis_start;
            size_t sel_len = draw_end - draw_start;
            size_t after_len = vis_end - draw_end;

            if (before_len > 0) {
              ab_append(&ab, line + ed->col_offset, before_len);
            }
            if (sel_len > 0) {
              ab_append_str(&ab, "\x1b[7m");
              ab_append(&ab, line + ed->col_offset + before_len, sel_len);
              ab_append_str(&ab, "\x1b[m");
            }
            if (after_len > 0) {
              ab_append(&ab, line + ed->col_offset + before_len + sel_len,
                        after_len);
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
  free(spans);
  free(offsets);
}
