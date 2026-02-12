// Copyright (c) 2026 Vitor Manuel de Sousa Pereira <vmsousapereira@gmail.com>
// SPDX-License-Identifier: MIT

#ifndef VED_FINDER_H
#define VED_FINDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct Editor Editor;

#define FINDER_MAX_ROWS 8

bool finder_open(Editor *ed);
void finder_close(Editor *ed);
void finder_reset(Editor *ed);
void finder_handle_key(Editor *ed, uint8_t key);
void finder_move_selection(Editor *ed, int delta);
size_t finder_list_rows(const Editor *ed);
size_t finder_modal_width(const Editor *ed);

#endif
