# Copyright (C) 2026 Vitor Manuel de Sousa Pereira <vmsousapereira@gmail.com>
# SPDX-License-Identifier: MIT

.SUFFIXES:

CFLAGS := -std=c23 -Isrc
LDFLAGS :=

ifeq ($(MODE), dbg)
CFLAGS += -g3 -O0 -Wall -Wextra -Wpedantic -DDEBUG
else
CFLAGS += -g0 -Ofast -flto -DNDEBUG
endif

ifdef $(SAN)
san_flags := -fsanitize=address,undefined
CFLAGS += $(san_flags)
LDFLAGS += $(san_flags)
endif

srcs := src/main.c
objs := $(srcs:.c=.o)
deps := $(objs:.o=.d)

ved: $(objs)
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MF $*.d -MP -c $< -o $@

clean:
	$(RM) ved $(objs) $(deps)

.PHONY: clean
