PG_CONFIG ?= pg_config

PG_LIBDIR  := $(shell $(PG_CONFIG) --libdir)
PG_INCDIR  := $(shell $(PG_CONFIG) --includedir)

CC      = gcc
CFLAGS  = -O2 -Wall -I$(PG_INCDIR)
LDFLAGS = -L$(PG_LIBDIR) -lpq -lpthread -lm
RPATH   = -Wl,-rpath,$(PG_LIBDIR)

all: bench_tiktoken

bench_tiktoken: bench_tiktoken.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(RPATH)

clean:
	rm -f bench_tiktoken

.PHONY: all clean
