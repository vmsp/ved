// Copyright (c) 2026 Vitor Manuel de Sousa Pereira <vmsousapereira@gmail.com>
// SPDX-License-Identifier: MIT

#include "editor.h"

#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "buffer.h"
#include "finder.h"
#include "utf8.h"
#include "util.h"

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} AppendBuf;

typedef struct {
  size_t start_row;
  size_t start_col;
  size_t end_row;
  size_t end_col;
  const char *color;
  uint8_t priority;
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

static void ab_move_cursor(AppendBuf *ab, int row, int col) {
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", row, col);
  ab_append_str(ab, buf);
}

static void ab_append_padded(AppendBuf *ab, const char *text, size_t width) {
  size_t len = strlen(text);
  if (len > width) {
    len = width;
  }
  if (len > 0) {
    ab_append(ab, text, len);
  }
  for (size_t i = len; i < width; i++) {
    ab_append_char(ab, ' ');
  }
}

static void finder_modal_geometry(Editor *ed, int text_rows, int *out_row,
                                  int *out_col, size_t *out_width,
                                  size_t *out_list_rows) {
  size_t width = finder_modal_width(ed);
  size_t list_rows = finder_list_rows(ed);
  int start_row = 2;
  if (start_row + (int)list_rows >= text_rows) {
    start_row = 1;
  }
  if (start_row < 1) {
    start_row = 1;
  }
  int start_col = (ed->screen_cols - (int)width) / 2 + 1;
  if (start_col < 1) {
    start_col = 1;
  }
  *out_row = start_row;
  *out_col = start_col;
  *out_width = width;
  *out_list_rows = list_rows;
}

static void render_finder_modal(Editor *ed, AppendBuf *ab, int text_rows) {
  if (!ed->finder.active) {
    return;
  }
  int start_row = 0;
  int start_col = 0;
  size_t width = 0;
  size_t list_rows = 0;
  finder_modal_geometry(ed, text_rows, &start_row, &start_col, &width,
                        &list_rows);

  char header[512];
  snprintf(header, sizeof(header), " Go to file: %s", ed->finder.query);
  ab_move_cursor(ab, start_row, start_col);
  ab_append_str(ab, "\x1b[7m");
  ab_append_padded(ab, header, width);
  ab_append_str(ab, "\x1b[m");

  size_t count = ed->finder.match_count;
  size_t start = ed->finder.scroll;
  for (size_t i = 0; i < list_rows; i++) {
    size_t row = start_row + 1 + (int)i;
    ab_move_cursor(ab, (int)row, start_col);
    if (count == 0 && i == 0) {
      ab_append_padded(ab, " No matches", width);
      continue;
    }
    size_t match_index = start + i;
    if (match_index >= count) {
      ab_append_padded(ab, "", width);
      continue;
    }
    size_t file_index = ed->finder.matches[match_index];
    const char *path = ed->finder.files[file_index];
    if (match_index == ed->finder.selection) {
      ab_append_str(ab, "\x1b[7m");
      ab_append_padded(ab, path, width);
      ab_append_str(ab, "\x1b[m");
    } else {
      ab_append_padded(ab, path, width);
    }
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

static uint8_t capture_priority(const char *name) {
  if (strcmp(name, "comment") == 0) {
    return 6;
  }
  if (strcmp(name, "string") == 0) {
    return 5;
  }
  if (strcmp(name, "number") == 0) {
    return 4;
  }
  if (strcmp(name, "function") == 0) {
    return 4;
  }
  if (strcmp(name, "type") == 0) {
    return 3;
  }
  if (strcmp(name, "keyword") == 0) {
    return 2;
  }
  if (strcmp(name, "preproc") == 0) {
    return 1;
  }
  return 0;
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
  ts_query_cursor_set_byte_range(cursor, range_start, range_end);
  ts_query_cursor_exec(cursor, ed->buffer.ts_query,
                       ts_tree_root_node(ed->buffer.ts_tree));

  HighlightSpan *spans = NULL;
  size_t span_count = 0;
  size_t span_cap = 0;
  TSQueryMatch match;
  uint32_t capture_index = 0;
  while (ts_query_cursor_next_capture(cursor, &match, &capture_index)) {
    if (capture_index >= match.capture_count) {
      continue;
    }
    TSQueryCapture capture = match.captures[capture_index];
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
    uint8_t priority = capture_priority(name_buf);
    size_t start = ts_node_start_byte(capture.node);
    size_t end = ts_node_end_byte(capture.node);
    if (end <= range_start || start >= range_end) {
      continue;
    }
    TSPoint start_point = ts_node_start_point(capture.node);
    TSPoint end_point = ts_node_end_point(capture.node);
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
      .start_row = start_point.row,
      .start_col = start_point.column,
      .end_row = end_point.row,
      .end_col = end_point.column,
      .color = color,
      .priority = priority
    };
  }
  ts_query_cursor_delete(cursor);
  *out_count = span_count;
  return spans;
}

static int span_compare(const void *a, const void *b) {
  const HighlightSpan *lhs = a;
  const HighlightSpan *rhs = b;
  if (lhs->start_row < rhs->start_row) {
    return -1;
  }
  if (lhs->start_row > rhs->start_row) {
    return 1;
  }
  if (lhs->start_col < rhs->start_col) {
    return -1;
  }
  if (lhs->start_col > rhs->start_col) {
    return 1;
  }
  return 0;
}

static size_t tab_width_at_col(size_t col) {
  return 8 - (col % 8);
}

static void ab_append_expanding_tabs(AppendBuf *ab, const char *data,
                                      size_t len, size_t start_col) {
  size_t col = start_col;
  for (size_t i = 0; i < len; i++) {
    if (data[i] == '\t') {
      size_t spaces = tab_width_at_col(col);
      for (size_t s = 0; s < spaces; s++) {
        ab_append_char(ab, ' ');
      }
      col += spaces;
    } else {
      ab_append_char(ab, data[i]);
      col++;
    }
  }
}

static void render_line_with_spans(AppendBuf *ab, const char *line,
                                   size_t line_len, size_t file_row,
                                   size_t vis_col, size_t vis_len,
                                   HighlightSpan *spans,
                                   size_t span_count) {
  const char **colors = calloc(line_len, sizeof(*colors));
  uint8_t *prio = calloc(line_len, sizeof(*prio));
  if (!colors) {
    die("calloc");
  }
  if (!prio) {
    free(colors);
    die("calloc");
  }
  for (size_t i = 0; i < span_count; i++) {
    HighlightSpan span = spans[i];
    if (file_row < span.start_row || file_row > span.end_row) {
      continue;
    }
    size_t start = 0;
    size_t end = line_len;
    if (file_row == span.start_row) {
      start = span.start_col;
    }
    if (file_row == span.end_row) {
      end = span.end_col < line_len ? span.end_col : line_len;
    }
    if (start > line_len) {
      continue;
    }
    if (end > line_len) {
      end = line_len;
    }
    if (end <= start) {
      continue;
    }
    for (size_t j = start; j < end; j++) {
      if (span.priority >= prio[j]) {
        prio[j] = span.priority;
        colors[j] = span.color;
      }
    }
  }

  size_t vis_end = vis_col + vis_len;
  size_t start = vis_col < line_len ? vis_col : line_len;
  size_t end = vis_end < line_len ? vis_end : line_len;
  const char *active = NULL;
  size_t run_start = start;
  size_t current_col = utf8_byte_to_vis_col(line, start);
  for (size_t i = start; i < end; i++) {
    const char *color = colors[i];
    if (color == active) {
      continue;
    }
    if (i > run_start) {
      if (active) {
        ab_append_str(ab, active);
      } else {
        ab_append_str(ab, "\x1b[m");
      }
      ab_append_expanding_tabs(ab, line + run_start, i - run_start,
                               current_col);
      current_col += utf8_byte_to_vis_col(line + run_start, i - run_start);
    }
    run_start = i;
    active = color;
  }
  if (end > run_start) {
    if (active) {
      ab_append_str(ab, active);
    } else {
      ab_append_str(ab, "\x1b[m");
    }
    ab_append_expanding_tabs(ab, line + run_start, end - run_start,
                             current_col);
  }
  ab_append_str(ab, "\x1b[m");
  free(colors);
  free(prio);
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
  if (ed->row_offset + text_rows > ed->buffer.line_count) {
    if (ed->buffer.line_count > text_rows) {
      ed->row_offset = ed->buffer.line_count - text_rows;
    } else {
      ed->row_offset = 0;
    }
  }

  if (frame->row < ed->buffer.line_count) {
    const char *line = ed->buffer.lines[frame->row];
    size_t len = line_length(&ed->buffer, frame->row);
    size_t frame_vis_col = utf8_byte_to_vis_col(line, frame->col);
    size_t offset_vis_col = utf8_byte_to_vis_col(line, ed->col_offset);

    if (frame_vis_col < offset_vis_col) {
      ed->col_offset = utf8_vis_to_byte_col(line, len, frame_vis_col);
    }
    if (frame_vis_col >= offset_vis_col + (size_t)ed->screen_cols) {
      size_t new_vis_col = frame_vis_col - ed->screen_cols + 1;
      ed->col_offset = utf8_vis_to_byte_col(line, len, new_vis_col);
    }
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
      TSTree *tree = ts_parser_parse_string(ed->buffer.ts_parser, NULL,
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

      size_t vis_offset = utf8_byte_to_vis_col(line, ed->col_offset);
      size_t vis_end = vis_offset + (size_t)ed->screen_cols;
      size_t end_byte = utf8_vis_to_byte_col(line, line_len, vis_end);

      size_t vis_len = 0;
      if (ed->col_offset < line_len) {
        vis_len = end_byte - ed->col_offset;
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
          if (spans) {
            render_line_with_spans(&ab, line, line_len, file_row,
                                   ed->col_offset, vis_len,
                                   spans, span_count);
          } else {
            ab_append_expanding_tabs(&ab, line + ed->col_offset, vis_len,
                                     vis_offset);
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
              sel_start >= end_byte) {
            ab_append_expanding_tabs(&ab, line + ed->col_offset, vis_len,
                                     vis_offset);
          } else {
            size_t vis_start = ed->col_offset;
            size_t draw_start = sel_start > vis_start ? sel_start : vis_start;
            size_t draw_end = sel_end < end_byte ? sel_end : end_byte;
            size_t before_len = draw_start - vis_start;
            size_t sel_len = draw_end - draw_start;
            size_t after_len = end_byte - draw_end;

            if (before_len > 0) {
              ab_append_expanding_tabs(&ab, line + ed->col_offset, before_len,
                                       vis_offset);
            }
            if (sel_len > 0) {
              ab_append_str(&ab, "\x1b[7m");
              size_t col = utf8_byte_to_vis_col(line + vis_start, before_len);
              ab_append_expanding_tabs(&ab,
                                       line + ed->col_offset + before_len,
                                       sel_len,
                                       vis_offset + col);
              ab_append_str(&ab, "\x1b[m");
            }
            if (after_len > 0) {
              size_t col = utf8_byte_to_vis_col(line + vis_start,
                                                before_len + sel_len);
              ab_append_expanding_tabs(&ab,
                                       line + ed->col_offset + before_len +
                                       sel_len,
                                       after_len,
                                       vis_offset + col);
            }
          }
        }
      }
    }
    ab_append_str(&ab, "\x1b[K");
    if (y < text_rows - 1) {
      ab_append_str(&ab, "\r\n");
    }
  }

  ab_append_str(&ab, "\r\n");
  ab_append_str(&ab, "\x1b[7m");
  {
    char status[256];
    if (ed->prompt_active) {
      snprintf(status, sizeof(status), " Save as: %s", ed->prompt_buf);
    } else {
      const char *name = ed->buffer.file_path ?
        ed->buffer.file_path :
        "[No Name]";
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
  ab_append_str(&ab, "\x1b[K");

  render_finder_modal(ed, &ab, text_rows);

  int cursor_row = (int)(frame->row - ed->row_offset) + 1;
  int cursor_col = 1;
  if (ed->finder.active) {
    int start_row = 0;
    int start_col = 0;
    size_t width = 0;
    size_t list_rows = 0;
    finder_modal_geometry(ed, text_rows, &start_row, &start_col, &width,
                          &list_rows);
    size_t prefix = strlen(" Go to file: ");
    size_t col = prefix + ed->finder.query_len + 1;
    if (col > width) {
      col = width;
    }
    cursor_row = start_row;
    cursor_col = start_col + (int)col - 1;
  } else if (frame->row < ed->buffer.line_count) {
    const char *line = ed->buffer.lines[frame->row];
    size_t frame_vis = utf8_byte_to_vis_col(line, frame->col);
    size_t offset_vis = utf8_byte_to_vis_col(line, ed->col_offset);
    cursor_col = (int)(frame_vis - offset_vis) + 1;
  }
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", cursor_row, cursor_col);
  ab_append_str(&ab, buf);
  ab_append_str(&ab, "\x1b[?25h");
  write_all(STDOUT_FILENO, ab.data, ab.len);
  ab_free(&ab);
  free(spans);
  free(offsets);
}
