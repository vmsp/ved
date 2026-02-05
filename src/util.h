// Copyright (c) 2026 Vitor Manuel de Sousa Pereira <vmsousapereira@gmail.com>
// SPDX-License-Identifier: MIT

#ifndef VED_UTIL_H
#define VED_UTIL_H

#include <stdlib.h>

char *base64_encode(const char *data, size_t len);
void die(const char *msg);

#endif
