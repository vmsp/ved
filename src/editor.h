// Copyright (c) 2026 Vitor Manuel de Sousa Pereira <vmsousapereira@gmail.com>
// SPDX-License-Identifier: MIT

#ifndef VED_EDITOR_H
#define VED_EDITOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <termios.h>

#include "buffer.h"

#define CTRL_KEY(k) ((k) & 0x1f)
#define KILL_RING_MAX 16

typedef struct {
  Buffer *buffer;
  size_t row;
  size_t col;
} Frame;

typedef struct Editor {
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
} Editor;

static inline int pos_compare(size_t row_a, size_t col_a,
                              size_t row_b, size_t col_b) {
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

Editor *editor_create(int argc, char **argv);
void editor_run(Editor *ed);
void editor_destroy(Editor *ed);

void editor_refresh_screen(Editor *ed);
void editor_process_key(Editor *ed, uint8_t key);

#endif
