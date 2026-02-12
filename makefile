# Copyright (c) 2026 Vitor Manuel de Sousa Pereira <vmsousapereira@gmail.com>
# SPDX-License-Identifier: MIT

.SUFFIXES:

TARGET ?= ved
PREFIX ?= /usr/local/bin

CFLAGS := -std=c23 -Ivendor/tree-sitter/lib/include
LDFLAGS :=

ifeq ($(MODE), dev)
CFLAGS += -g3 -O0 -Wall -Wextra -Wpedantic -DDEBUG
else
CFLAGS += -g0 -O3 -ffast-math -flto -DNDEBUG
endif

ifdef SAN
san_flags := -fsanitize=address,undefined
CFLAGS += $(san_flags)
LDFLAGS += $(san_flags)
endif

srcs := \
	src/buffer.c \
	src/core.c \
	src/finder.c \
	src/input.c \
	src/main.c \
	src/render.c \
	src/utf8.c \
	src/util.c \
	vendor/tree-sitter/lib/src/lib.c \
	vendor/tree-sitter-c/src/parser.c
objs := $(srcs:.c=.o)
deps := $(objs:.o=.d)

$(TARGET): $(objs)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MF $*.d -MP -c $< -o $@

install: $(TARGET)
	install -m 755 $(TARGET) $(PREFIX)

uninstall:
	$(RM) $(PREFIX)/$(TARGET)

clean:
	$(RM) $(TARGET) $(objs) $(deps)

.PHONY: install uninstall clean
