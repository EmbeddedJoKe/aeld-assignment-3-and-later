# Honor CC supplied by Buildroot or the caller. GNU make otherwise defaults
# CC to cc; retain CROSS_COMPILE support for the earlier assignments.
ifeq ($(origin CC),default)
CC = $(CROSS_COMPILE)gcc
endif
CFLAGS ?= -Wall -Wextra

all: writer

writer: writer.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f writer *.o

.PHONY: all clean
