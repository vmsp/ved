// Copyright (c) 2026 Vitor Manuel de Sousa Pereira <vmsousapereira@gmail.com>
// SPDX-License-Identifier: MIT

#include "util.h"

#include <stdio.h>
#include <stdlib.h>

void die(const char *msg) {
  perror(msg);
  exit(EXIT_FAILURE);
}
