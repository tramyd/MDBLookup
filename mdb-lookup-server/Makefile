CC      = gcc
CFLAGS  = -g -Wall -I../linked-list/part1
LDFLAGS = -L../linked-list/part1
LDLIBS  = -lmylist

mdb-lookup-server: mdb-lookup-server.o

mdb-lookup-server.o: mdb.h mdb.o

mdb.o: mdb.c

.PHONY: clean
clean:
	rm -f *.o a.out mdb-lookup-server

.PHONY: all
all: clean default