# LotofBench — one-command build.
#   make            (or the cc one-liner documented in the README)
CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -pedantic
LDLIBS   = -lm

SRC = src/main.c \
      src/harness.c \
      src/rng.c \
      src/workload_compression.c \
      src/workload_navigation.c \
      src/lz4.c \
      src/sha1.c
HDR = src/lotofbench.h \
      src/harness.h \
      src/rng.h \
      src/workload_compression.h \
      src/workload_navigation.h \
      src/lz4.h \
      src/sha1.h

lotofbench: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -pthread $(SRC) -o $@ $(LDLIBS)

clean:
	rm -f lotofbench

.PHONY: clean
