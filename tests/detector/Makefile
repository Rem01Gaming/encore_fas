# SPDX-License-Identifier: GPL-2.0-only
#
# Host tests of the detector. They need no kernel.
#   make test      run the regression test on this machine
#   make hotpath   check the arm64 code of the frame path (needs a cross compiler)

CC ?= cc
CROSS ?= aarch64-linux-gnu-
KDIR := ../../kernel
CFLAGS ?= -std=gnu11 -O2 -Wall -Wextra -Wno-unused-function
INC := -Ishim -I$(KDIR) -I$(KDIR)/include

all: test

fas_det_test: fas_det_test.c $(KDIR)/encore_fas_det.h $(wildcard shim/linux/*.h)
	$(CC) $(CFLAGS) $(INC) -o $@ fas_det_test.c -lm

test: fas_det_test
	./fas_det_test

hotpath.o: hotpath.c $(KDIR)/encore_fas_det.h
	$(CROSS)gcc $(CFLAGS) $(INC) -c -o $@ hotpath.c

# The frame path must not divide. Division is slow, and every division in the
# detector uses a reciprocal that fas_det_setup() computes once.
hotpath: hotpath.o
	@$(CROSS)objdump -d --no-show-raw-insn hotpath.o | awk ' \
		/^[0-9a-f]+ <hotpath_frame>:/ { on = 1; next } \
		/^[0-9a-f]+ <.*>:/ { on = 0 } \
		on && /\t[us]div/ { bad++ } \
		on && /^ +[0-9a-f]+:/ { n++ } \
		END { printf "hotpath_frame: %d instructions on all paths, %d divisions\n", n, bad; exit bad > 0 }'

clean:
	rm -f fas_det_test hotpath.o

.PHONY: all test hotpath clean
