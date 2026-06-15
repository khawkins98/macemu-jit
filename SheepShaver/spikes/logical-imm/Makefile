# Standalone spike: ARM64 logical-immediate encoder validation.
# Requires clang (used both to build the test and as an assembler oracle).
CC      ?= clang
CFLAGS  ?= -O2 -Wall -Wextra -std=c11

.PHONY: all run clean

all: test_logical_imm

test_logical_imm: test_logical_imm.c \
		../../src/kpx_cpu/src/cpu/jit/aarch64/ppc-logical-imm.hpp
	$(CC) $(CFLAGS) -o $@ test_logical_imm.c

run: test_logical_imm
	./test_logical_imm

clean:
	rm -f test_logical_imm /tmp/li_oracle.* /tmp/li_rej.*
