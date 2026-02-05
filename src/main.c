// Copyright (c) 2026 Vitor Manuel de Sousa Pereira <vmsousapereira@gmail.com>
// SPDX-License-Identifier: MIT

#include "editor.h"

int main(int argc, char **argv) {
  Editor *ed = editor_create(argc, argv);
  editor_run(ed);
  editor_destroy(ed);
  return 0;
}
