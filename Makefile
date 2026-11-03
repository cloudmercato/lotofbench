# LotofBench — one-command build.
#   make            (or the cc one-liner documented in the README)
#   make test       (unit tests)
# -ffp-contract=off is mandatory on every compile: the PDF renderer's
# CTM algebra and the corpus generator's coordinate quantization must
# not be contracted into FMAs, or results stop being bit-identical
# across compilers and machines (docs/benchmarks.md, "Workload 4").
CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -pedantic
FPC      = -ffp-contract=off
LDLIBS   = -lm

SRC = src/main.c \
      src/harness.c \
      src/rng.c \
      src/workload_compression.c \
      src/workload_navigation.c \
      src/workload_html5browser.c \
      src/workload_pdfrender.c \
      src/pdfparse.c \
      src/pdfrender.c \
      src/workload_clang.c \
      src/workload_textprocessing.c \
      src/workload_photofilter.c \
      src/imgops.c \
      src/lz4.c \
      src/sha1.c
HDR = src/lotofbench.h \
      src/harness.h \
      src/rng.h \
      src/workload_compression.h \
      src/workload_navigation.h \
      src/workload_html5browser.h \
      src/workload_pdfrender.h \
      src/pdf.h \
      src/pdfrender.h \
      src/workload_clang.h \
      src/workload_textprocessing.h \
      src/workload_photofilter.h \
      src/imgops.h \
      src/lz4.h \
      src/sha1.h

TESTS_SRC = tests/runner.c \
            tests/test_util.c \
            tests/test_rng.c \
            tests/test_sha1.c \
            tests/test_lz4.c \
            tests/test_harness.c \
            tests/test_workload_compression.c \
            tests/test_workload_navigation.c \
            tests/test_workload_html5browser.c \
            tests/test_workload_pdfrender.c \
            tests/test_workload_clang.c \
            tests/test_workload_textprocessing.c \
            tests/test_imgops.c \
            tests/test_workload_photofilter.c
TESTS_HDR = tests/tests.h
# The test binary links everything but the suite driver (it has its own main).
TESTS_LIB = $(filter-out src/main.c,$(SRC))

lotofbench: $(SRC) $(HDR)
	$(CC) $(CFLAGS) $(FPC) -pthread $(SRC) -o $@ $(LDLIBS)

tests/lotofbench_test: $(TESTS_SRC) $(TESTS_HDR) $(TESTS_LIB) $(HDR)
	$(CC) $(CFLAGS) $(FPC) -Isrc -pthread $(TESTS_SRC) $(TESTS_LIB) -o $@ $(LDLIBS)

tools/gen_goldens: tools/gen_goldens.c $(TESTS_LIB) $(HDR)
	$(CC) $(CFLAGS) $(FPC) -Isrc -pthread $< $(TESTS_LIB) -o $@ $(LDLIBS)

test: tests/lotofbench_test
	./tests/lotofbench_test

clean:
	rm -f lotofbench tests/lotofbench_test tools/gen_goldens

.PHONY: test clean
