# Preserve an explicit CC from Buildroot or the caller.
ifeq ($(origin CC),default)
CC = $(CROSS_COMPILE)gcc
endif
CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -Werror

.DEFAULT_GOAL := all
.PHONY: all default check clean

all default: aesdsocket

aesdsocket: aesdsocket.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

check: tests/test_aesdsocket
	./tests/test_aesdsocket

tests/test_aesdsocket: tests/test_aesdsocket.c aesdsocket.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f aesdsocket tests/test_aesdsocket *.o
