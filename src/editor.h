// Copyright (c) 2026 Vitor Manuel de Sousa Pereira <vmsousapereira@gmail.com>
// SPDX-License-Identifier: MIT

#ifndef VED_EDITOR_H
#define VED_EDITOR_H

#include <stdbool.h>

typedef struct Editor Editor;

Editor *editor_create(int argc, char **argv);
void editor_run(Editor *ed);
void editor_destroy(Editor *ed);

#endif
