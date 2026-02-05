// Copyright (c) 2026 Vitor Manuel de Sousa Pereira <vmsousapereira@gmail.com>
// SPDX-License-Identifier: MIT

#include "editor.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "buffer.h"
#include "utf8.h"
#include "util.h"

#define CTRL_KEY(k) ((k) & 0x1f)
#define KILL_RING_MAX 16

typedef struct {
  Buffer *buffer;
  size_t row;
  size_t col;
} Frame;

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} AppendBuf;

struct Editor {
  struct termios orig_termios;
  int stdin_flags;
  int screen_rows;
  int screen_cols;
  size_t row_offset;
  size_t col_offset;
  Buffer buffer;
  Frame frame;
  char *file_path;
  bool dirty;
  bool pending_ctrl_x;
  bool pending_escape;
  bool prompt_active;
  bool prompt_save_as;
  char prompt_buf[256];
  size_t prompt_len;
  char *kill_ring[KILL_RING_MAX];
  size_t kill_ring_len;
  size_t kill_ring_index;
  bool last_was_kill;
  bool last_was_yank;
  size_t yank_start_row;
  size_t yank_start_col;
  size_t yank_end_row;
  size_t yank_end_col;
  bool mark_active;
  size_t mark_row;
  size_t mark_col;
  char status_msg[128];
};

static Editor *g_editor;

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

static void disable_raw_mode(void) {
  if (!g_editor) {
    return;
  }
  write(STDOUT_FILENO, "\x1b[?25h", 6);
  write(STDOUT_FILENO, "\x1b[m", 3);
  write(STDOUT_FILENO, "\x1b[?1049l", 8);
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_editor->orig_termios);
  if (g_editor->stdin_flags >= 0) {
    fcntl(STDIN_FILENO, F_SETFL, g_editor->stdin_flags);
  }
}

static void handle_sigint(int sig) {
  (void)sig;
  disable_raw_mode();
  _exit(130);
}

static void enable_raw_mode(Editor *ed) {
  struct termios raw;
  ed->stdin_flags = fcntl(STDIN_FILENO, F_GETFL);
  if (ed->stdin_flags == -1) {
    die("fcntl(F_GETFL)");
  }
  if (fcntl(STDIN_FILENO, F_SETFL, ed->stdin_flags | O_NONBLOCK) == -1) {
    die("fcntl(F_SETFL)");
  }
  if (tcgetattr(STDIN_FILENO, &ed->orig_termios) == -1) {
    die("tcgetattr");
  }
  raw = ed->orig_termios;
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | IEXTEN);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 0;
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
    die("tcsetattr");
  }
  write(STDOUT_FILENO, "\x1b[?1049h", 8);
}

static void get_window_size(Editor *ed) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 ||
      ws.ws_col == 0 || ws.ws_row == 0) {
    die("ioctl(TIOCGWINSZ)");
  }
  ed->screen_rows = ws.ws_row;
  ed->screen_cols = ws.ws_col;
}

static int pos_compare(size_t row_a, size_t col_a, size_t row_b, size_t col_b) {
  if (row_a < row_b) {
    return -1;
  }
  if (row_a > row_b) {
    return 1;
  }
  if (col_a < col_b) {
    return -1;
  }
  if (col_a > col_b) {
    return 1;
  }
  return 0;
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

static void editor_refresh_screen(Editor *ed) {
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
  free(text);
  buffer_delete_region(&ed->buffer, start_row, start_col, end_row, end_col);
  ed->frame.row = start_row;
  ed->frame.col = start_col;
  ed->dirty = true;
  ed->mark_active = false;
  return true;
}

static void editor_prompt_save_as(Editor *ed) {
  ed->prompt_active = true;
  ed->prompt_save_as = true;
  ed->prompt_len = 0;
  ed->prompt_buf[0] = '\0';
}

static void editor_process_key(Editor *ed, uint8_t key) {
  bool reset_kill = true;
  bool reset_yank = true;

  if (key == CTRL_KEY('g')) {
    ed->pending_ctrl_x = false;
    ed->pending_escape = false;
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
      ed->pending_escape = false;
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
          free(ed->file_path);
          ed->file_path = path;
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
    if (key == 127 || key == CTRL_KEY('h')) {
      if (ed->prompt_len > 0) {
        size_t new_len = utf8_prev_boundary(ed->prompt_buf, ed->prompt_len);
        ed->prompt_len = new_len;
        ed->prompt_buf[ed->prompt_len] = '\0';
      }
      return;
    }
    if (key >= 32 && key != 127 &&
        ed->prompt_len + 1 < sizeof(ed->prompt_buf)) {
      ed->prompt_buf[ed->prompt_len++] = (char)key;
      ed->prompt_buf[ed->prompt_len] = '\0';
    }
    return;
  }

  if (ed->pending_escape) {
    ed->pending_escape = false;
    if (key == 'f') {
      frame_move_next_word(&ed->frame);
      reset_kill = true;
      goto done;
    }
    if (key == 'b') {
      frame_move_prev_word(&ed->frame);
      reset_kill = true;
      goto done;
    }
    if (key == 'd') {
      editor_kill_word_forward(ed);
      ed->dirty = true;
      ed->last_was_kill = true;
      reset_kill = false;
      goto done;
    }
    if (key == 127 || key == CTRL_KEY('h')) {
      editor_kill_word_backward(ed);
      ed->dirty = true;
      ed->last_was_kill = true;
      reset_kill = false;
      goto done;
    }
    if (key == 'w') {
      if (editor_copy_region(ed)) {
        snprintf(ed->status_msg, sizeof(ed->status_msg), "Copied");
      } else {
        snprintf(ed->status_msg, sizeof(ed->status_msg), "No region");
      }
      ed->last_was_kill = false;
      reset_kill = true;
      goto done;
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
        reset_yank = false;
      }
      goto done;
    }
  }

  if (ed->pending_ctrl_x) {
    ed->pending_ctrl_x = false;
    if (key == CTRL_KEY('s')) {
      if (!ed->file_path) {
        editor_prompt_save_as(ed);
      } else if (buffer_write_file(&ed->buffer, ed->file_path)) {
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
    case 27:
      ed->pending_escape = true;
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
      } else if (frame->row + 1 < buf->line_count) {
        editor_kill_newline_and_next(ed);
        ed->dirty = true;
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
    case 127:
    case CTRL_KEY('h'):
      if (ed->frame.col > 0 || ed->frame.row > 0) {
        buffer_delete_char(&ed->buffer, ed->frame.row, ed->frame.col);
        frame_move_prev_char(&ed->frame);
        ed->dirty = true;
      }
      break;
    case '\r':
      buffer_insert_newline(&ed->buffer, ed->frame.row, ed->frame.col);
      ed->frame.row++;
      ed->frame.col = 0;
      ed->dirty = true;
      break;
    case CTRL_KEY('d'):
      buffer_delete_forward(&ed->buffer, ed->frame.row, ed->frame.col);
      ed->dirty = true;
      break;
    default:
      if (key >= 32 && key != 127) {
        buffer_insert_char(&ed->buffer, ed->frame.row, ed->frame.col, key);
        ed->frame.col++;
        ed->dirty = true;
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

static void editor_loop(Editor *ed) {
  struct pollfd pfd = {
    .fd = STDIN_FILENO,
    .events = POLLIN
  };

  while (true) {
    int ready = poll(&pfd, 1, -1);
    if (ready == -1) {
      if (errno == EINTR) {
        continue;
      }
      die("poll");
    }
    if (pfd.revents & POLLIN) {
      uint8_t buf[256];
      ssize_t nread = read(STDIN_FILENO, buf, sizeof(buf));
      if (nread > 0) {
        for (ssize_t i = 0; i < nread; i++) {
          editor_process_key(ed, buf[i]);
        }
        get_window_size(ed);
        editor_refresh_screen(ed);
      } else if (nread == -1 && errno != EAGAIN) {
        die("read");
      }
    }
  }
}

Editor *editor_create(int argc, char **argv) {
  Editor *ed = calloc(1, sizeof(*ed));
  if (!ed) {
    die("calloc");
  }

  g_editor = ed;
  if (signal(SIGINT, handle_sigint) == SIG_ERR) {
    die("signal");
  }
  atexit(disable_raw_mode);

  buffer_init(&ed->buffer);
  if (argc > 1) {
    ed->file_path = strdup(argv[1]);
    if (!ed->file_path) {
      die("strdup");
    }
    buffer_load_file(&ed->buffer, ed->file_path);
  }
  ed->dirty = false;
  ed->pending_ctrl_x = false;
  ed->pending_escape = false;
  ed->prompt_active = false;
  ed->prompt_save_as = false;
  ed->prompt_len = 0;
  for (size_t i = 0; i < KILL_RING_MAX; i++) {
    ed->kill_ring[i] = NULL;
  }
  ed->kill_ring_len = 0;
  ed->kill_ring_index = 0;
  ed->last_was_kill = false;
  ed->last_was_yank = false;
  ed->yank_start_row = 0;
  ed->yank_start_col = 0;
  ed->yank_end_row = 0;
  ed->yank_end_col = 0;
  ed->mark_active = false;
  ed->mark_row = 0;
  ed->mark_col = 0;
  snprintf(ed->status_msg, sizeof(ed->status_msg),
           "C-x C-s to save");
  ed->frame.buffer = &ed->buffer;
  ed->frame.row = 0;
  ed->frame.col = 0;
  get_window_size(ed);

  enable_raw_mode(ed);
  return ed;
}

void editor_run(Editor *ed) {
  editor_refresh_screen(ed);
  editor_loop(ed);
}

void editor_destroy(Editor *ed) {
  if (!ed) {
    return;
  }
  disable_raw_mode();
  buffer_free(&ed->buffer);
  for (size_t i = 0; i < ed->kill_ring_len; i++) {
    free(ed->kill_ring[i]);
  }
  free(ed->file_path);
  free(ed);
}
