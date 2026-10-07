CC = $(CROSS_COMPILE)gcc # selects compiler
CFLAGS = -Wall -Wextra # keeps compiler warnings enabled

all: writer # build the writer app by default

writer: writer.c # writer depends on writer.c and runs executable
	$(CC) $(CFLAGS) -o writer writer.c

clean: # removes executable and files ending in .o from current folder
	rm -f writer *.o

.PHONY: all clean # prevents a file named clean from blocking cleanup
