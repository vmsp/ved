# Copyright (c) 2026 Vitor Manuel de Sousa Pereira <vmsousapereira@gmail.com>
# SPDX-License-Identifier: MIT

.SUFFIXES:

CFLAGS := -std=c23 -Isrc
LDFLAGS :=

ifeq ($(mode), dbg)
CFLAGS += -g3 -O0 -Wall -Wextra -Wpedantic -DDEBUG
else
CFLAGS += -g0 -O3 -ffast-math -flto -DNDEBUG
endif

ifdef $(san)
san_flags := -fsanitize=address,undefined
CFLAGS += $(san_flags)
LDFLAGS += $(san_flags)
endif

srcs := \
	src/main.c \
	src/editor_core.c \
	src/editor_render.c \
	src/editor_input.c \
	src/buffer.c \
	src/utf8.c \
	src/util.c
objs := $(srcs:.c=.o)
deps := $(objs:.o=.d)

ved: $(objs)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MF $*.d -MP -c $< -o $@

clean:
	$(RM) ved $(objs) $(deps)

.PHONY: clean
