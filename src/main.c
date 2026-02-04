// Copyright (C) 2026 Vitor Manuel de Sousa Pereira <vmsousapereira@gmail.com>
// SPDX-License-Identifier: MIT

#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define CTRL_KEY(k) ((k) & 0x1f)

typedef struct {
  char **lines;
  size_t line_count;
} Buffer;

typedef struct {
  Buffer *buffer;
  size_t row;
  size_t col;
} Frame;

typedef struct {
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
  char *kill_buf;
  size_t kill_len;
  bool last_was_kill;
  char status_msg[128];
} Editor;

static Editor *g_editor;

static void die(const char *msg) {
  perror(msg);
  exit(EXIT_FAILURE);
}

static void disable_raw_mode(void) {
  if (!g_editor) {
    return;
  }
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

static void buffer_init(Buffer *buf) {
  buf->line_count = 1;
  buf->lines = calloc(buf->line_count, sizeof(*buf->lines));
  if (!buf->lines) {
    die("calloc");
  }
  buf->lines[0] = strdup("");
  if (!buf->lines[0]) {
    die("strdup");
  }
}

static void buffer_free(Buffer *buf) {
  for (size_t i = 0; i < buf->line_count; i++) {
    free(buf->lines[i]);
  }
  free(buf->lines);
}

static void buffer_clear(Buffer *buf) {
  buffer_free(buf);
  buffer_init(buf);
}

static void buffer_append_line(Buffer *buf, const char *line) {
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

static void buffer_load_file(Buffer *buf, const char *path) {
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
}

static size_t line_length(Buffer *buf, size_t row) {
  if (row >= buf->line_count) {
    return 0;
  }
  return strlen(buf->lines[row]);
}

static bool utf8_is_continuation(unsigned char ch) {
  return (ch & 0xc0) == 0x80;
}

static size_t utf8_next_boundary(const char *line, size_t len, size_t col) {
  if (col >= len) {
    return len;
  }
  col++;
  while (col < len && utf8_is_continuation((unsigned char)line[col])) {
    col++;
  }
  return col;
}

static size_t utf8_prev_boundary(const char *line, size_t col) {
  if (col == 0) {
    return 0;
  }
  col--;
  while (col > 0 && utf8_is_continuation((unsigned char)line[col])) {
    col--;
  }
  return col;
}

static size_t utf8_clamp_col(const char *line, size_t len, size_t col) {
  if (col > len) {
    col = len;
  }
  if (col > 0 && col < len &&
      utf8_is_continuation((unsigned char)line[col])) {
    col = utf8_prev_boundary(line, col);
  }
  return col;
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

static void buffer_insert_char(Buffer *buf, size_t row, size_t col, char ch) {
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

static void buffer_insert_newline(Buffer *buf, size_t row, size_t col) {
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

static void buffer_delete_char(Buffer *buf, size_t row, size_t col) {
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

static void buffer_delete_forward(Buffer *buf, size_t row, size_t col) {
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

static void buffer_delete_range(Buffer *buf, size_t row, size_t start,
                                size_t end) {
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

  editor_scroll(ed);

  write(STDOUT_FILENO, "\x1b[?25l", 6);
  write(STDOUT_FILENO, "\x1b[H", 3);

  int text_rows = ed->screen_rows > 1 ? ed->screen_rows - 1 : 0;
  for (int y = 0; y < text_rows; y++) {
    size_t file_row = ed->row_offset + (size_t)y;
    if (file_row >= ed->buffer.line_count) {
      write(STDOUT_FILENO, "~", 1);
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
        write(STDOUT_FILENO, line, len);
      }
    }
    write(STDOUT_FILENO, "\x1b[K", 3);
    if (y < ed->screen_rows - 1) {
      write(STDOUT_FILENO, "\r\n", 2);
    }
  }

  write(STDOUT_FILENO, "\x1b[7m", 4);
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
      write(STDOUT_FILENO, status, len);
    }
    if (len < (size_t)ed->screen_cols) {
      for (int i = 0; i < ed->screen_cols - (int)len; i++) {
        write(STDOUT_FILENO, " ", 1);
      }
    }
  }
  write(STDOUT_FILENO, "\x1b[m", 3);

  int cursor_row = (int)(frame->row - ed->row_offset) + 1;
  int cursor_col = (int)(frame->col - ed->col_offset) + 1;
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", cursor_row, cursor_col);
  write(STDOUT_FILENO, buf, strlen(buf));
  write(STDOUT_FILENO, "\x1b[?25h", 6);
}

static bool buffer_write_file(Buffer *buf, const char *path) {
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

static void editor_kill_set(Editor *ed, const char *data, size_t len,
                            bool append) {
  if (!append) {
    free(ed->kill_buf);
    ed->kill_buf = NULL;
    ed->kill_len = 0;
  }
  if (len == 0) {
    return;
  }
  char *buf = realloc(ed->kill_buf, ed->kill_len + len + 1);
  if (!buf) {
    die("realloc");
  }
  memcpy(buf + ed->kill_len, data, len);
  ed->kill_len += len;
  buf[ed->kill_len] = '\0';
  ed->kill_buf = buf;
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

static void editor_yank(Editor *ed) {
  if (ed->kill_len == 0 || !ed->kill_buf) {
    return;
  }
  const char *data = ed->kill_buf;
  size_t len = ed->kill_len;
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
  ed->dirty = true;
}

static void editor_prompt_save_as(Editor *ed) {
  ed->prompt_active = true;
  ed->prompt_save_as = true;
  ed->prompt_len = 0;
  ed->prompt_buf[0] = '\0';
}

static void editor_process_key(Editor *ed, uint8_t key) {
  bool reset_kill = true;

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
      editor_yank(ed);
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

int main(int argc, char **argv) {
  Editor ed = {0};

  g_editor = &ed;
  if (signal(SIGINT, handle_sigint) == SIG_ERR) {
    die("signal");
  }
  atexit(disable_raw_mode);

  buffer_init(&ed.buffer);
  if (argc > 1) {
    ed.file_path = strdup(argv[1]);
    if (!ed.file_path) {
      die("strdup");
    }
    buffer_load_file(&ed.buffer, ed.file_path);
  }
  ed.dirty = false;
  ed.pending_ctrl_x = false;
  ed.pending_escape = false;
  ed.prompt_active = false;
  ed.prompt_save_as = false;
  ed.prompt_len = 0;
  ed.kill_buf = NULL;
  ed.kill_len = 0;
  ed.last_was_kill = false;
  snprintf(ed.status_msg, sizeof(ed.status_msg),
           "C-x C-s to save");
  ed.frame.buffer = &ed.buffer;
  ed.frame.row = 0;
  ed.frame.col = 0;
  get_window_size(&ed);

  enable_raw_mode(&ed);
  editor_refresh_screen(&ed);
  editor_loop(&ed);

  buffer_free(&ed.buffer);
  free(ed.kill_buf);
  free(ed.file_path);
  return 0;
}
