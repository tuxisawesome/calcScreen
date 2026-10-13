# tools/Makefile - PC-side helpers (not needed on the Pi or the calculator).
#
#   make            build bavconv
#   make test       build and run the bavconv byte-exact test
#   make clean

CC      ?= cc
CFLAGS  ?= -O2
WARN     = -std=c99 -Wall -Wextra -Werror -pedantic
CPPFLAGS += -I../core

all: bavconv

bavconv: bavconv.c ../core/bav.h ../core/dock_common.h
	$(CC) $(WARN) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ bavconv.c

test: bavconv
	./test_bavconv.sh ./bavconv

clean:
	rm -f bavconv

.PHONY: all test clean
