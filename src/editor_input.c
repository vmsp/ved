// Copyright (c) 2026 Vitor Manuel de Sousa Pereira <vmsousapereira@gmail.com>
// SPDX-License-Identifier: MIT

#include "editor.h"

#include <ctype.h>
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

#define KEY_BACKSPACE 127

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

static void osc52_set_clipboard(const char *data, size_t len) {
  char *b64 = base64_encode(data, len);
  const char *prefix = "\x1b]52;c;";
  const char *suffix = "\x07";
  write_all(STDOUT_FILENO, prefix, strlen(prefix));
  write_all(STDOUT_FILENO, b64, strlen(b64));
  write_all(STDOUT_FILENO, suffix, strlen(suffix));
  free(b64);
}

static bool editor_get_region(const Editor *ed, size_t *start_row,
                              size_t *start_col, size_t *end_row,
                              size_t *end_col) {
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
  if (row_a == row_b && col_a == col_b) {
    return false;
  }
  *start_row = row_a;
  *start_col = col_a;
  *end_row = row_b;
  *end_col = col_b;
  return true;
}

static char *editor_copy_region_text(Editor *ed, size_t *out_len) {
  size_t start_row = 0;
  size_t start_col = 0;
  size_t end_row = 0;
  size_t end_col = 0;
  if (!editor_get_region(ed, &start_row, &start_col, &end_row, &end_col)) {
    return NULL;
  }
  *out_len = 0;
  char *buf = NULL;

  for (size_t row = start_row; row <= end_row; row++) {
    const char *line = ed->buffer.lines[row];
    size_t len = line_length(&ed->buffer, row);
    size_t from = 0;
    size_t to = len;
    if (row == start_row) {
      from = start_col < len ? start_col : len;
    }
    if (row == end_row) {
      to = end_col < len ? end_col : len;
    }
    if (to > from) {
      size_t add = to - from;
      char *next = realloc(buf, *out_len + add + 1);
      if (!next) {
        free(buf);
        die("realloc");
      }
      buf = next;
      memcpy(buf + *out_len, line + from, add);
      *out_len += add;
      buf[*out_len] = '\0';
    }
    if (row < end_row) {
      char *next = realloc(buf, *out_len + 2);
      if (!next) {
        free(buf);
        die("realloc");
      }
      buf = next;
      buf[*out_len] = '\n';
      (*out_len)++;
      buf[*out_len] = '\0';
    }
  }
  return buf;
}

static bool is_word_char(unsigned char ch) {
  if (ch >= 128) {
    return true;
  }
  return (ch >= 'a' && ch <= 'z') ||
         (ch >= 'A' && ch <= 'Z') ||
         (ch >= '0' && ch <= '9') ||
         ch == '_';
}

static void frame_clamp_col(Frame *frame) {
  size_t len = line_length(frame->buffer, frame->row);
  const char *line = frame->buffer->lines[frame->row];
  frame->col = utf8_clamp_col(line, len, frame->col);
}

static void frame_move_prev_line(Frame *frame) {
  if (frame->row == 0) {
    return;
  }
  frame->row--;
  frame_clamp_col(frame);
}

static void frame_move_next_line(Frame *frame) {
  if (frame->row + 1 >= frame->buffer->line_count) {
    return;
  }
  frame->row++;
  frame_clamp_col(frame);
}

static void frame_move_prev_char(Frame *frame) {
  if (frame->col > 0) {
    const char *line = frame->buffer->lines[frame->row];
    frame->col = utf8_prev_boundary(line, frame->col);
    return;
  }
  if (frame->row == 0) {
    return;
  }
  frame->row--;
  frame->col = line_length(frame->buffer, frame->row);
}

static void frame_move_next_char(Frame *frame) {
  size_t len = line_length(frame->buffer, frame->row);
  const char *line = frame->buffer->lines[frame->row];
  if (frame->col < len) {
    frame->col = utf8_next_boundary(line, len, frame->col);
    return;
  }
  if (frame->row + 1 >= frame->buffer->line_count) {
    return;
  }
  frame->row++;
  frame->col = 0;
}

static void frame_move_line_start(Frame *frame) {
  frame->col = 0;
}

static void frame_move_line_end(Frame *frame) {
  frame->col = line_length(frame->buffer, frame->row);
}

static void frame_move_next_word(Frame *frame) {
  size_t row = frame->row;
  size_t col = frame->col;

  while (row < frame->buffer->line_count) {
    const char *line = frame->buffer->lines[row];
    size_t len = line_length(frame->buffer, row);
    col = utf8_clamp_col(line, len, col);

    while (col < len) {
      unsigned char ch = (unsigned char)line[col];
      if (ch >= 128 || !isspace(ch)) {
        break;
      }
      col = utf8_next_boundary(line, len, col);
    }
    while (col < len) {
      unsigned char ch = (unsigned char)line[col];
      if (!is_word_char(ch)) {
        break;
      }
      col = utf8_next_boundary(line, len, col);
    }
    if (col < len || row + 1 >= frame->buffer->line_count) {
      break;
    }
    row++;
    col = 0;
  }
  frame->row = row;
  frame->col = col;
}

static void frame_move_prev_word(Frame *frame) {
  size_t row = frame->row;
  size_t col = frame->col;

  while (row < frame->buffer->line_count) {
    const char *line = frame->buffer->lines[row];
    size_t len = line_length(frame->buffer, row);
    col = utf8_clamp_col(line, len, col);

    if (col == 0) {
      if (row == 0) {
        break;
      }
      row--;
      col = line_length(frame->buffer, row);
      continue;
    }

    while (col > 0) {
      size_t prev = utf8_prev_boundary(line, col);
      unsigned char ch = (unsigned char)line[prev];
      if (ch >= 128 || !isspace(ch)) {
        break;
      }
      col = prev;
    }
    while (col > 0) {
      size_t prev = utf8_prev_boundary(line, col);
      unsigned char ch = (unsigned char)line[prev];
      if (!is_word_char(ch)) {
        break;
      }
      col = prev;
    }
    break;
  }
  frame->row = row;
  frame->col = col;
}

static void editor_kill_set(Editor *ed, const char *data, size_t len,
                            bool append) {
  if (len == 0) {
    return;
  }
  if (!append || ed->kill_ring_len == 0) {
    size_t next_index = 0;
    if (ed->kill_ring_len < KILL_RING_MAX) {
      next_index = ed->kill_ring_len;
      ed->kill_ring_len++;
    } else {
      next_index = (ed->kill_ring_index + 1) % KILL_RING_MAX;
      free(ed->kill_ring[next_index]);
    }
    ed->kill_ring_index = next_index;
    ed->kill_ring[next_index] = NULL;
  }
  char **entry = &ed->kill_ring[ed->kill_ring_index];
  size_t cur_len = *entry ? strlen(*entry) : 0;
  char *buf = realloc(*entry, cur_len + len + 1);
  if (!buf) {
    die("realloc");
  }
  memcpy(buf + cur_len, data, len);
  buf[cur_len + len] = '\0';
  *entry = buf;
}

static void editor_kill_newline_and_next(Editor *ed) {
  Buffer *buf = &ed->buffer;
  Frame *frame = &ed->frame;
  if (frame->row + 1 >= buf->line_count) {
    return;
  }
  const char *next = buf->lines[frame->row + 1];
  size_t next_len = strlen(next);
  editor_kill_set(ed, "\n", 1, ed->last_was_kill);
  editor_kill_set(ed, next, next_len, true);

  size_t cur_len = line_length(buf, frame->row);
  char *merged = realloc(buf->lines[frame->row], cur_len + next_len + 1);
  if (!merged) {
    die("realloc");
  }
  memcpy(merged + cur_len, next, next_len + 1);
  free(buf->lines[frame->row + 1]);
  buf->lines[frame->row] = merged;
  memmove(&buf->lines[frame->row + 1], &buf->lines[frame->row + 2],
          sizeof(*buf->lines) * (buf->line_count - frame->row - 2));
  buf->line_count--;
}

static void editor_kill_word_forward(Editor *ed) {
  Buffer *buf = &ed->buffer;
  Frame *frame = &ed->frame;
  if (frame->row >= buf->line_count) {
    return;
  }
  const char *line = buf->lines[frame->row];
  size_t len = line_length(buf, frame->row);
  size_t col = frame->col;

  if (col >= len) {
    editor_kill_newline_and_next(ed);
    return;
  }

  size_t start = col;
  while (col < len) {
    unsigned char ch = (unsigned char)line[col];
    if (ch >= 128 || !isspace(ch)) {
      break;
    }
    col = utf8_next_boundary(line, len, col);
  }
  while (col < len) {
    unsigned char ch = (unsigned char)line[col];
    if (!is_word_char(ch)) {
      break;
    }
    col = utf8_next_boundary(line, len, col);
  }
  if (col > start) {
    editor_kill_set(ed, line + start, col - start, ed->last_was_kill);
    buffer_delete_range(buf, frame->row, start, col);
  }
}

static void editor_kill_word_backward(Editor *ed) {
  Buffer *buf = &ed->buffer;
  Frame *frame = &ed->frame;
  if (frame->row >= buf->line_count) {
    return;
  }
  const char *line = buf->lines[frame->row];
  size_t len = line_length(buf, frame->row);
  size_t col = frame->col;

  if (col == 0) {
    if (frame->row == 0) {
      return;
    }
    frame->row--;
    frame->col = line_length(buf, frame->row);
    editor_kill_newline_and_next(ed);
    return;
  }

  col = utf8_clamp_col(line, len, col);
  size_t end = col;
  while (col > 0) {
    size_t prev = utf8_prev_boundary(line, col);
    unsigned char ch = (unsigned char)line[prev];
    if (ch >= 128 || !isspace(ch)) {
      break;
    }
    col = prev;
  }
  while (col > 0) {
    size_t prev = utf8_prev_boundary(line, col);
    unsigned char ch = (unsigned char)line[prev];
    if (!is_word_char(ch)) {
      break;
    }
    col = prev;
  }
  if (end > col) {
    editor_kill_set(ed, line + col, end - col, ed->last_was_kill);
    buffer_delete_range(buf, frame->row, col, end);
    frame->col = col;
  }
}

static bool editor_yank_from(Editor *ed, const char *data) {
  if (!data) {
    return false;
  }
  size_t len = strlen(data);
  ed->yank_start_row = ed->frame.row;
  ed->yank_start_col = ed->frame.col;
  for (size_t i = 0; i < len; i++) {
    if (data[i] == '\n') {
      buffer_insert_newline(&ed->buffer, ed->frame.row, ed->frame.col);
      ed->frame.row++;
      ed->frame.col = 0;
    } else {
      buffer_insert_char(&ed->buffer, ed->frame.row, ed->frame.col, data[i]);
      ed->frame.col++;
    }
  }
  ed->yank_end_row = ed->frame.row;
  ed->yank_end_col = ed->frame.col;
  ed->dirty = true;
  ed->buffer.syntax_dirty = true;
  return true;
}

static bool editor_yank(Editor *ed) {
  if (ed->kill_ring_len == 0) {
    return false;
  }
  return editor_yank_from(ed, ed->kill_ring[ed->kill_ring_index]);
}

static bool editor_copy_region(Editor *ed) {
  size_t text_len = 0;
  char *text = editor_copy_region_text(ed, &text_len);
  if (!text) {
    return false;
  }
  editor_kill_set(ed, text, text_len, false);
  osc52_set_clipboard(text, text_len);
  free(text);
  ed->mark_active = false;
  return true;
}

static bool editor_kill_region(Editor *ed) {
  size_t start_row = 0;
  size_t start_col = 0;
  size_t end_row = 0;
  size_t end_col = 0;
  if (!editor_get_region(ed, &start_row, &start_col, &end_row, &end_col)) {
    return false;
  }
  size_t text_len = 0;
  char *text = editor_copy_region_text(ed, &text_len);
  if (!text) {
    return false;
  }
  editor_kill_set(ed, text, text_len, ed->last_was_kill);
  osc52_set_clipboard(text, text_len);
  free(text);
  buffer_delete_region(&ed->buffer, start_row, start_col, end_row, end_col);
  ed->frame.row = start_row;
  ed->frame.col = start_col;
  ed->dirty = true;
  ed->buffer.syntax_dirty = true;
  ed->mark_active = false;
  return true;
}

static void editor_prompt_save_as(Editor *ed) {
  ed->prompt_active = true;
  ed->prompt_save_as = true;
  ed->prompt_len = 0;
  ed->prompt_buf[0] = '\0';
}

static bool handle_meta_key(Editor *ed, uint8_t key, bool *reset_kill,
                            bool *reset_yank) {
  if (key == 'f') {
    frame_move_next_word(&ed->frame);
    *reset_kill = true;
    return true;
  }
  if (key == 'b') {
    frame_move_prev_word(&ed->frame);
    *reset_kill = true;
    return true;
  }
  if (key == 'd') {
    editor_kill_word_forward(ed);
    ed->dirty = true;
    ed->buffer.syntax_dirty = true;
    ed->last_was_kill = true;
    *reset_kill = false;
    return true;
  }
  if (key == KEY_BACKSPACE) {
    editor_kill_word_backward(ed);
    ed->dirty = true;
    ed->buffer.syntax_dirty = true;
    ed->last_was_kill = true;
    *reset_kill = false;
    return true;
  }
  if (key == 'w') {
    if (editor_copy_region(ed)) {
      snprintf(ed->status_msg, sizeof(ed->status_msg), "Copied");
    } else {
      snprintf(ed->status_msg, sizeof(ed->status_msg), "No region");
    }
    ed->last_was_kill = false;
    *reset_kill = true;
    return true;
  }
  if (key == 'y') {
    if (ed->last_was_yank && ed->kill_ring_len > 0) {
      size_t max = ed->kill_ring_len < KILL_RING_MAX
        ? ed->kill_ring_len : KILL_RING_MAX;
      if (max > 1) {
        if (ed->kill_ring_index == 0) {
          ed->kill_ring_index = max - 1;
        } else {
          ed->kill_ring_index--;
        }
        buffer_delete_region(&ed->buffer, ed->yank_start_row,
                             ed->yank_start_col, ed->yank_end_row,
                             ed->yank_end_col);
        ed->frame.row = ed->yank_start_row;
        ed->frame.col = ed->yank_start_col;
        editor_yank_from(ed, ed->kill_ring[ed->kill_ring_index]);
        ed->last_was_yank = true;
      }
      *reset_yank = false;
    }
    return true;
  }
  return false;
}

static bool handle_mouse_event(Editor *ed, const char *seq, char type) {
  char *end = NULL;
  long b = strtol(seq, &end, 10);
  if (!end || *end != ';') {
    return false;
  }
  long x = strtol(end + 1, &end, 10);
  if (!end || *end != ';') {
    return false;
  }
  long y = strtol(end + 1, &end, 10);
  if (!end) {
    return false;
  }
  bool pressed = type == 'M';
  int text_rows = ed->screen_rows > 1 ? ed->screen_rows - 1 : 0;
  if (x <= 0 || y <= 0 || y > text_rows) {
    return true;
  }
  if (b == 64) {
    if (ed->row_offset > 0) {
      ed->row_offset--;
      if (ed->frame.row > 0) {
        ed->frame.row--;
      }
    }
    return true;
  }
  if (b == 65) {
    if (ed->row_offset + 1 < ed->buffer.line_count) {
      ed->row_offset++;
      if (ed->frame.row + 1 < ed->buffer.line_count) {
        ed->frame.row++;
      }
    }
    return true;
  }
  if (b == 0) {
    size_t row = ed->row_offset + (size_t)(y - 1);
    if (row >= ed->buffer.line_count) {
      row = ed->buffer.line_count - 1;
    }
    size_t col = ed->col_offset + (size_t)(x - 1);
    if (pressed) {
      ed->mark_active = true;
      ed->mark_row = row;
      ed->mark_col = col;
    }
    ed->frame.row = row;
    ed->frame.col = col;
    frame_clamp_col(&ed->frame);
    if (!pressed) {
      return true;
    }
  } else if (b != 32) {
    return true;
  }
  if (b == 32) {
    size_t row = ed->row_offset + (size_t)(y - 1);
    if (row >= ed->buffer.line_count) {
      row = ed->buffer.line_count - 1;
    }
    size_t col = ed->col_offset + (size_t)(x - 1);
    if (!ed->mark_active) {
      ed->mark_active = true;
      ed->mark_row = ed->frame.row;
      ed->mark_col = ed->frame.col;
    }
    ed->frame.row = row;
    ed->frame.col = col;
    frame_clamp_col(&ed->frame);
  }
  return true;
}

static bool handle_escape(Editor *ed, uint8_t key, bool *reset_kill,
                          bool *reset_yank) {
  if (ed->esc_state == ESC_SEEN) {
    if (key == '[') {
      ed->esc_state = ESC_CSI;
      return true;
    }
    ed->esc_state = ESC_NONE;
    return handle_meta_key(ed, key, reset_kill, reset_yank);
  }
  if (ed->esc_state == ESC_CSI) {
    if (key == '<') {
      ed->esc_state = ESC_MOUSE;
      ed->esc_len = 0;
      return true;
    }
    ed->esc_state = ESC_NONE;
    return false;
  }
  if (ed->esc_state == ESC_MOUSE) {
    if (key == 'm' || key == 'M') {
      ed->esc_buf[ed->esc_len] = '\0';
      ed->esc_state = ESC_NONE;
      return handle_mouse_event(ed, ed->esc_buf, (char)key);
    }
    if (ed->esc_len + 1 < sizeof(ed->esc_buf)) {
      ed->esc_buf[ed->esc_len++] = (char)key;
    } else {
      ed->esc_state = ESC_NONE;
    }
    return true;
  }
  return false;
}

void editor_process_key(Editor *ed, uint8_t key) {
  bool reset_kill = true;
  bool reset_yank = true;

  if (ed->esc_state != ESC_NONE) {
    if (handle_escape(ed, key, &reset_kill, &reset_yank)) {
      goto done;
    }
  }

  if (key == 27) {
    ed->esc_state = ESC_SEEN;
    goto done;
  }

  if (key == CTRL_KEY('g')) {
    ed->pending_ctrl_x = false;
    ed->esc_state = ESC_NONE;
    ed->esc_len = 0;
    ed->prompt_active = false;
    ed->prompt_save_as = false;
    ed->prompt_len = 0;
    ed->mark_active = false;
    ed->last_was_kill = false;
    ed->last_was_yank = false;
    snprintf(ed->status_msg, sizeof(ed->status_msg), "Quit");
    return;
  }

  if (ed->prompt_active) {
    if (key == 27) {
      ed->prompt_active = false;
      ed->prompt_save_as = false;
      snprintf(ed->status_msg, sizeof(ed->status_msg), "Save canceled");
      return;
    }
    if (key == '\r') {
      if (ed->prompt_len == 0) {
        snprintf(ed->status_msg, sizeof(ed->status_msg), "No file name");
      } else if (ed->prompt_save_as) {
        char *path = strndup(ed->prompt_buf, ed->prompt_len);
        if (!path) {
          die("strdup");
        }
        if (buffer_write_file(&ed->buffer, path)) {
          free(ed->buffer.file_path);
          ed->buffer.file_path = path;
          ed->buffer.mode = detect_mode(path);
          ed->dirty = false;
          snprintf(ed->status_msg, sizeof(ed->status_msg), "Saved");
        } else {
          snprintf(ed->status_msg, sizeof(ed->status_msg),
                   "Save failed: %s", strerror(errno));
          free(path);
        }
      }
      ed->prompt_active = false;
      ed->prompt_save_as = false;
      return;
    }
    if (key == KEY_BACKSPACE) {
      if (ed->prompt_len > 0) {
        size_t new_len = utf8_prev_boundary(ed->prompt_buf, ed->prompt_len);
        ed->prompt_len = new_len;
        ed->prompt_buf[ed->prompt_len] = '\0';
      }
      return;
    }
    if (key >= 32 && key != KEY_BACKSPACE &&
        ed->prompt_len + 1 < sizeof(ed->prompt_buf)) {
      ed->prompt_buf[ed->prompt_len++] = (char)key;
      ed->prompt_buf[ed->prompt_len] = '\0';
    }
    return;
  }

  if (ed->pending_ctrl_x) {
    ed->pending_ctrl_x = false;
    if (key == CTRL_KEY('s')) {
      if (!ed->buffer.file_path) {
        editor_prompt_save_as(ed);
      } else if (buffer_write_file(&ed->buffer, ed->buffer.file_path)) {
        ed->dirty = false;
        snprintf(ed->status_msg, sizeof(ed->status_msg), "Saved");
      } else {
        snprintf(ed->status_msg, sizeof(ed->status_msg),
                 "Save failed: %s", strerror(errno));
      }
      reset_kill = true;
      goto done;
    }
  }

  switch (key) {
    case 0:
      if (ed->mark_active &&
          ed->mark_row == ed->frame.row &&
          ed->mark_col == ed->frame.col) {
        ed->mark_active = false;
        snprintf(ed->status_msg, sizeof(ed->status_msg), "Mark cleared");
      } else {
        ed->mark_active = true;
        ed->mark_row = ed->frame.row;
        ed->mark_col = ed->frame.col;
        snprintf(ed->status_msg, sizeof(ed->status_msg), "Mark set");
      }
      reset_kill = true;
      break;
    case CTRL_KEY('x'):
      ed->pending_ctrl_x = true;
      snprintf(ed->status_msg, sizeof(ed->status_msg), "C-x");
      reset_kill = true;
      break;
    case CTRL_KEY('p'):
      frame_move_prev_line(&ed->frame);
      break;
    case CTRL_KEY('n'):
      frame_move_next_line(&ed->frame);
      break;
    case CTRL_KEY('b'):
      frame_move_prev_char(&ed->frame);
      break;
    case CTRL_KEY('f'):
      frame_move_next_char(&ed->frame);
      break;
    case CTRL_KEY('a'):
      frame_move_line_start(&ed->frame);
      break;
    case CTRL_KEY('e'):
      frame_move_line_end(&ed->frame);
      break;
    case CTRL_KEY('k'): {
      Buffer *buf = &ed->buffer;
      Frame *frame = &ed->frame;
      size_t len = line_length(buf, frame->row);
      if (frame->col < len) {
        char *line = buf->lines[frame->row];
        size_t kill_len = len - frame->col;
        editor_kill_set(ed, line + frame->col, kill_len, ed->last_was_kill);
        line[frame->col] = '\0';
        ed->dirty = true;
        ed->buffer.syntax_dirty = true;
      } else if (frame->row + 1 < buf->line_count) {
        editor_kill_newline_and_next(ed);
        ed->dirty = true;
        ed->buffer.syntax_dirty = true;
      }
      ed->last_was_kill = true;
      reset_kill = false;
      break;
    }
    case CTRL_KEY('y'):
      if (editor_yank(ed)) {
        ed->last_was_yank = true;
        reset_yank = false;
      }
      break;
    case CTRL_KEY('w'):
      if (editor_kill_region(ed)) {
        ed->last_was_kill = true;
        reset_kill = false;
        snprintf(ed->status_msg, sizeof(ed->status_msg), "Killed");
      } else {
        snprintf(ed->status_msg, sizeof(ed->status_msg), "No region");
      }
      break;
    case KEY_BACKSPACE:
      if (ed->frame.col > 0 || ed->frame.row > 0) {
        buffer_delete_char(&ed->buffer, ed->frame.row, ed->frame.col);
        frame_move_prev_char(&ed->frame);
        ed->dirty = true;
        ed->buffer.syntax_dirty = true;
      }
      break;
    case '\r':
      buffer_insert_newline(&ed->buffer, ed->frame.row, ed->frame.col);
      ed->frame.row++;
      ed->frame.col = 0;
      ed->dirty = true;
      ed->buffer.syntax_dirty = true;
      break;
    case '\t':
      buffer_insert_tab(&ed->buffer, ed->frame.row, ed->frame.col);
      if (ed->buffer.mode == MAKEFILE_MODE) {
        ed->frame.col++;
      } else {
        ed->frame.col += 2;
      }
      ed->dirty = true;
      ed->buffer.syntax_dirty = true;
      break;
    case CTRL_KEY('d'):
      buffer_delete_forward(&ed->buffer, ed->frame.row, ed->frame.col);
      ed->dirty = true;
      ed->buffer.syntax_dirty = true;
      break;
    default:
      if (key >= 32 && key != KEY_BACKSPACE) {
        buffer_insert_char(&ed->buffer, ed->frame.row, ed->frame.col, key);
        ed->frame.col++;
        ed->dirty = true;
        ed->buffer.syntax_dirty = true;
      }
      break;
  }

done:
  if (reset_kill) {
    ed->last_was_kill = false;
  }
  if (reset_yank) {
    ed->last_was_yank = false;
  }
}
