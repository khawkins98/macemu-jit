# SheepShaver AArch64 JIT — Master Makefile
SHELL       := /bin/bash
SS_DIR      := src/Unix
SS_BIN      := $(SS_DIR)/SheepShaver
JIT_DIR     := src/kpx_cpu/src/cpu/jit/aarch64
ROM_HARNESS := rom-harness/rom-harness

ROM         := /workspace/tmp/rpi-basilisk-assets/ppc-roms/PowerMac-9500-OldWorld.rom
DISK        := /workspace/tmp/ss-test-disk.img
BENCH_DISK  := /workspace/tmp/Benchmark.hda
PREFS_DIR   := /tmp/ss-desktop
PREFS_FILE  := $(PREFS_DIR)/.sheepshaver_prefs
VNC_PORT    := 5999
TMUX_SESSION ?= ss-repro
TMUX_LOG     ?= /tmp/$(TMUX_SESSION).log

JIT_CXX     := g++
JIT_CXXFLAGS := -g -O2 -std=c++11 -DHAVE_CONFIG_H
JIT_INCLUDES := -I$(JIT_DIR)

.PHONY: all build build-ss build-rom-harness build-rom-inspect inspect-rom \
        test test-opcodes test-jit test-rom \
        run run-jit run-tmux run-jit-tmux \
        tmux-attach tmux-log tmux-stop \
        screenshot kill clean \
        ensure-xvfb ensure-prefs

all: build

build: build-ss build-rom-harness

# --- build-ss: platform-split build ------------------------------------------
#
# macOS (this fork's macos-arm64 branch): configure.ac already includes the
# AArch64 JIT (ppc-jit.cpp) in CPUSRCS, so the autoconf build in src/Unix
# compiles and links everything with the correct flags. We just delegate.
#
# Linux (upstream rcarmo workflow): upstream's configure does NOT know about
# the JIT, so the recipe hand-compiles ppc-jit.o and hand-relinks with the
# GTK/X11 library set. That recipe is kept byte-for-byte as upstream has it.
#
# NOTE: this split is a MINIMAL REMEDIATION (Option A). It exists because the
# Linux recipe is destructive on macOS: the GTK link fails and the failed
# linker deletes the existing SheepShaver binary. The proper fix (Option B)
# is to teach upstream's configure.ac to put ppc-jit.cpp in CPUSRCS on Linux
# ARM64 the same way this branch does — then BOTH platforms reduce to
# "cd src/Unix && make -j<n>" and the hand-compile/hand-link below disappears.
# Option B should be done in coordination with upstream (needs Linux ARM64
# testing) and is the shape this fix should take in an upstream PR.
# See docs/superpowers/specs/2026-06-03-build-ss-macos-fix-design.md.
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
build-ss:
	@echo "=== Building SheepShaver (macOS: autoconf build owns the JIT) ==="
	@cd $(SS_DIR) && $(MAKE) -j$$(sysctl -n hw.ncpu)
	@echo "Built: $(SS_BIN)"
else
build-ss:
	@echo "=== Building SheepShaver ==="
	@cd $(SS_DIR) && $(MAKE) -j$$(nproc) 2>&1 | tail -3 || true
	@echo "=== Compiling JIT ==="
	@$(JIT_CXX) $(JIT_CXXFLAGS) $(JIT_INCLUDES) -c \
		-o $(SS_DIR)/obj/ppc-jit.o $(JIT_DIR)/ppc-jit.cpp
	@cd $(SS_DIR) && $(JIT_CXX) -o SheepShaver obj/*.o \
		-lpthread -lm -lSDL2 \
		-lgtk-x11-2.0 -lgdk-x11-2.0 -lpangocairo-1.0 -latk-1.0 -lcairo \
		-lgdk_pixbuf-2.0 -lgio-2.0 -lpangoft2-1.0 -lpango-1.0 \
		-lgobject-2.0 -lglib-2.0 -lharfbuzz -lvncserver -lfontconfig -lfreetype
	@echo "Built: $(SS_BIN)"
endif

build-rom-harness:
	@echo "=== Building ROM harness ==="
	@cd rom-harness && $(MAKE)

build-rom-inspect:
	@echo "=== Building ROM inspector ==="
	@cd rom-inspect && $(MAKE)

# Inspect a ROM file: does DecodeROM()/PatchROM() type-detection accept it?
#   make inspect-rom ROM="/path/to/Mac OS ROM"
inspect-rom: build-rom-inspect
	@./rom-inspect/rom-inspect "$(ROM)"

test: test-opcodes test-jit test-rom
	@echo ""
	@echo "=== All tests complete ==="

# test-opcodes: DEFAULT mode = interpreter determinism (each vector run twice
# through the interpreter, REGDUMPs diffed).  This validates the interpreter, NOT
# the JIT codegen.  For codegen changes, use `make test-jit` (the real gate).
test-opcodes: build-ss
	@echo ""
	@echo "=== Interpreter Determinism Harness (does NOT exercise JIT codegen) ==="
	@cd jit-test && bash run.sh 2>&1 | grep "METRIC\|FAIL\|ERROR" | head -20

# test-jit: the JIT correctness gate.  Runs every vector through BOTH the
# interpreter (reference) and the aarch64 JIT, and diffs the REGDUMPs
# (SS_HARNESS_MODE=jit).  THIS is what proves a codegen change is correct —
# run it on every change to ppc-jit.cpp.  Includes the RA register-pressure /
# eviction stress vector (lmw_stmw_wide).
test-jit: build-ss
	@echo ""
	@echo "=== JIT<->Interpreter Equivalence Harness (codegen correctness gate) ==="
	@cd jit-test && SS_HARNESS_MODE=jit bash run.sh 2>&1 | \
		grep -E "HARNESS mode=|METRIC (pass|fail|total|score)=|opcode_[a-z0-9_]+=(0|-1)|FAIL|ERROR"

test-rom: build-rom-harness
	@echo ""
	@echo "=== ROM Harness (10K blocks) ==="
	@timeout 120 $(ROM_HARNESS) $(ROM) \
		--count=10000 --min-insns=1 --max-insns=64 2>&1 | \
		grep "Score:\|Passed:\|Failed:\|Testable\|JIT compile\|SIGSEGV"

ensure-xvfb:
	@pgrep -x Xvfb > /dev/null 2>&1 || \
		{ Xvfb :99 -screen 0 800x600x24 & sleep 1; echo "Started Xvfb :99"; }

ensure-prefs:
	@mkdir -p $(PREFS_DIR)
	@printf 'rom $(ROM)\ndisk $(DISK)\nramsize 134217728\nscreen win/640/480\ndisplaycolordepth 8\nnosound true\nnocdrom true\nnogui true\nignoreillegal true\nvncserver true\nvncport $(VNC_PORT)\ncpuclock 500\n' > $(PREFS_FILE)
	@if [ -f "$(BENCH_DISK)" ]; then \
		grep -q "Benchmark" $(PREFS_FILE) || \
		echo "disk $(BENCH_DISK)" >> $(PREFS_FILE); \
	fi

run: build-ss ensure-xvfb ensure-prefs kill
	@echo "=== Starting SheepShaver (interpreter) on VNC port $(VNC_PORT) ==="
	@GDK_BACKEND=x11 SDL_VIDEODRIVER=x11 DISPLAY=:99 HOME=$(PREFS_DIR) \
		$(SS_BIN) > /dev/null 2>/tmp/ss-stderr.log &
	@sleep 2
	@echo "PID: $$(pgrep -x SheepShaver | head -1)"
	@echo "VNC: vnc://localhost:$(VNC_PORT)"

run-jit: build-ss ensure-xvfb ensure-prefs kill
	@echo "=== Starting SheepShaver (JIT) on VNC port $(VNC_PORT) ==="
	@SS_USE_JIT=1 GDK_BACKEND=x11 SDL_VIDEODRIVER=x11 DISPLAY=:99 HOME=$(PREFS_DIR) \
		$(SS_BIN) > /dev/null 2>/tmp/ss-jit-stderr.log &
	@sleep 2
	@echo "PID: $$(pgrep -x SheepShaver | head -1)"
	@echo "VNC: vnc://localhost:$(VNC_PORT)"

run-tmux: build-ss ensure-xvfb ensure-prefs
	@echo "=== Starting SheepShaver in tmux session $(TMUX_SESSION) ==="
	@tmux has-session -t $(TMUX_SESSION) 2>/dev/null && tmux kill-session -t $(TMUX_SESSION) || true
	@rm -f $(TMUX_LOG)
	@tmux new-session -d -s $(TMUX_SESSION) \
		"cd '$(CURDIR)/$(SS_DIR)' && \
		echo '[start '$$(date -Is)'] interpreter' >> '$(TMUX_LOG)' && \
		env GDK_BACKEND=x11 SDL_VIDEODRIVER=x11 DISPLAY=:99 HOME='$(PREFS_DIR)' ./SheepShaver >> '$(TMUX_LOG)' 2>&1; \
		rc=\$$?; echo '[exit '\$$rc' '$$(date -Is)']' >> '$(TMUX_LOG)'; exec bash"
	@sleep 2
	@echo "tmux: $(TMUX_SESSION)"
	@echo "log:  $(TMUX_LOG)"
	@echo "VNC:  vnc://localhost:$(VNC_PORT)"
	@tmux has-session -t $(TMUX_SESSION) >/dev/null 2>&1 && echo "session alive" || echo "session exited"

run-jit-tmux: build-ss ensure-xvfb ensure-prefs
	@echo "=== Starting SheepShaver (JIT) in tmux session $(TMUX_SESSION) ==="
	@tmux has-session -t $(TMUX_SESSION) 2>/dev/null && tmux kill-session -t $(TMUX_SESSION) || true
	@rm -f $(TMUX_LOG)
	@tmux new-session -d -s $(TMUX_SESSION) \
		"cd '$(CURDIR)/$(SS_DIR)' && \
		echo '[start '$$(date -Is)'] jit' >> '$(TMUX_LOG)' && \
		env SS_USE_JIT=1 GDK_BACKEND=x11 SDL_VIDEODRIVER=x11 DISPLAY=:99 HOME='$(PREFS_DIR)' ./SheepShaver >> '$(TMUX_LOG)' 2>&1; \
		rc=\$$?; echo '[exit '\$$rc' '$$(date -Is)']' >> '$(TMUX_LOG)'; exec bash"
	@sleep 2
	@echo "tmux: $(TMUX_SESSION)"
	@echo "log:  $(TMUX_LOG)"
	@echo "VNC:  vnc://localhost:$(VNC_PORT)"
	@tmux has-session -t $(TMUX_SESSION) >/dev/null 2>&1 && echo "session alive" || echo "session exited"

tmux-attach:
	@tmux attach -t $(TMUX_SESSION)

tmux-log:
	@cat $(TMUX_LOG)

tmux-stop:
	@tmux has-session -t $(TMUX_SESSION) 2>/dev/null && tmux kill-session -t $(TMUX_SESSION) || true
	@echo "Stopped tmux session: $(TMUX_SESSION)"

screenshot:
	@export PATH="$$HOME/.local/bin:$$PATH"; \
	vncdotool -s 127.0.0.1::$(VNC_PORT) capture /workspace/tmp/ss-screenshot-$$(date +%H%M%S).png 2>&1 && \
		echo "Saved: /workspace/tmp/ss-screenshot-$$(date +%H%M%S).png" || \
		echo "VNC not available on port $(VNC_PORT)"

kill:
	@pkill -9 -x SheepShaver 2>/dev/null; true
	@sleep 0.3
	@echo "All SheepShaver processes killed"

clean: kill
	@cd $(SS_DIR) && rm -f obj/ppc-jit.o SheepShaver 2>/dev/null; true
	@cd rom-harness && $(MAKE) clean 2>/dev/null; true
	@echo "Clean complete"
