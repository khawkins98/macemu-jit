# M1 Task 9 thunk pre-work spike. macOS arm64 only.
# `make run` : build + run the end-to-end thunk test (PASS/FAIL).
# `make xcheck` : cross-verify every helper encoding against clang's assembler.
# `make all` (default) : both.

CC      := clang
CFLAGS  := -O2 -std=c11 -Wall -Wextra
TARGET  := thunk_ref

all: run xcheck

$(TARGET): thunk_ref.c
	$(CC) $(CFLAGS) -o $@ thunk_ref.c

run: $(TARGET)
	./$(TARGET)

# Assemble the reference instructions with clang, then disassemble the words
# and diff against the helper-produced encodings.
clang_ref.o: clang_ref.s
	$(CC) -x assembler -target arm64-apple-macos -c -o $@ clang_ref.s

xcheck: $(TARGET) clang_ref.o
	@echo "--- clang cross-check (helper encoding vs clang -c) ---"
	@./extract_xcheck.sh

clean:
	rm -f $(TARGET) clang_ref.o clang_words.txt

.PHONY: all run xcheck clean
