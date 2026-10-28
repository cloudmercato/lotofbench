# LotofBench — one-command build.
#   make            (or the cc one-liner documented in the README)
CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -pedantic
LDLIBS   = -lm

SRC = src/main.c src/lz4.c src/sha1.c
HDR = src/lotofbench.h src/lz4.h src/sha1.h

lotofbench: $(SRC) $(HDR)
	$(CC) $(CFLAGS) -pthread $(SRC) -o $@ $(LDLIBS)

clean:
	rm -f lotofbench

.PHONY: clean
