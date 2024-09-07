CC = gcc
CFLAGS = -g -Wall

http-server: http-server.o

http-server.o: http-server.c

.PHONY: clean
clean:
	rm -f *.o a.out core http-server

.PHONY: all
all: clean http-server
