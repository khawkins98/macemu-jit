CC ?= clang
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11

dual_map_test: dual_map_test.c
	$(CC) $(CFLAGS) -o $@ $<

run: dual_map_test
	./dual_map_test

clean:
	rm -f dual_map_test

.PHONY: run clean
