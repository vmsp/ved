// Copyright (c) 2026 Vitor Manuel de Sousa Pereira <vmsousapereira@gmail.com>
// SPDX-License-Identifier: MIT

#ifndef VED_UTF8_H
#define VED_UTF8_H

#include <stddef.h>
#include <stdbool.h>

bool utf8_is_continuation(unsigned char ch);
size_t utf8_next_boundary(const char *line, size_t len, size_t col);
size_t utf8_prev_boundary(const char *line, size_t col);
size_t utf8_clamp_col(const char *line, size_t len, size_t col);

size_t utf8_byte_to_vis_col(const char *line, size_t byte_col);
size_t utf8_vis_to_byte_col(const char *line, size_t len, size_t vis_col);

#endif
