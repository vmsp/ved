// Copyright (c) 2026 Vitor Manuel de Sousa Pereira <vmsousapereira@gmail.com>
// SPDX-License-Identifier: MIT

#include "editor.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "finder.h"
#include "util.h"

static Editor *g_editor;

static void disable_raw_mode(void) {
  if (!g_editor) {
    return;
  }
  write(STDOUT_FILENO, "\x1b[?25h", 6);
  write(STDOUT_FILENO, "\x1b[m", 3);
  write(STDOUT_FILENO, "\x1b[?1000l", 8);
  write(STDOUT_FILENO, "\x1b[?1002l", 8);
  write(STDOUT_FILENO, "\x1b[?1006l", 8);
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
  write(STDOUT_FILENO, "\x1b[?1000h", 8);
  write(STDOUT_FILENO, "\x1b[?1002h", 8);
  write(STDOUT_FILENO, "\x1b[?1006h", 8);
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
    buffer_load_file(&ed->buffer, argv[1]);
  }
  ed->dirty = false;
  ed->pending_ctrl_x = false;
  ed->esc_state = ESC_NONE;
  ed->esc_len = 0;
  ed->prompt_active = false;
  ed->prompt_save_as = false;
  ed->prompt_len = 0;
  ed->finder.active = false;
  ed->finder.query_len = 0;
  ed->finder.query[0] = '\0';
  ed->finder.project_root = NULL;
  ed->finder.files = NULL;
  ed->finder.file_count = 0;
  ed->finder.file_cap = 0;
  ed->finder.matches = NULL;
  ed->finder.match_count = 0;
  ed->finder.match_cap = 0;
  ed->finder.scored = NULL;
  ed->finder.scored_cap = 0;
  ed->finder.selection = 0;
  ed->finder.scroll = 0;
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
  buffer_init_syntax(&ed->buffer, ed->buffer.file_path);
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
  finder_reset(ed);
  for (size_t i = 0; i < ed->kill_ring_len; i++) {
    free(ed->kill_ring[i]);
  }
  free(ed);
}
