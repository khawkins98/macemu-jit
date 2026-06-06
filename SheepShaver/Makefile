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
        test test-opcodes test-jit test-rom harness-count ui-introspect-test ui-introspect-serialize-test \
        e2e e2e-smoke e2e-bench e2e-workload e2e-test e2e-setup e2e-clean ui-dump \
        run run-jit run-tmux run-jit-tmux \
        tmux-attach tmux-log tmux-stop \
        screenshot kill clean help \
        ensure-xvfb ensure-prefs

all: build

help:
	@echo "SheepShaver AArch64 JIT — Build & Test Targets"
	@echo ""
	@echo "  Build:"
	@echo "    make build          Build SheepShaver + rom-harness"
	@echo "    make build-ss       Build SheepShaver only"
	@echo ""
	@echo "  Test (run after every ppc-jit.cpp change):"
	@echo "    make test-jit       JIT codegen gate — interp vs JIT diff (USE THIS)"
	@echo "    make test-opcodes   Interpreter determinism only (not a JIT test)"
	@echo "    make test           Run all tests (opcodes + jit + rom)"
	@echo ""
	@echo "  Benchmark:"
	@echo "    cd rom-harness && make bench                     Quick per-instruction ns"
	@echo "    cd rom-harness && make bench BARGS=--save-baseline=/tmp/b.txt   Save baseline"
	@echo "    cd rom-harness && make bench BARGS=--compare=/tmp/b.txt         Compare after change"
	@echo ""
	@echo "  Run:"
	@echo "    ./src/Unix/SheepShaver                           Normal boot"
	@echo "    SS_JIT_VERIFY=1 ./src/Unix/SheepShaver           Differential JIT verification"
	@echo "    ./src/Unix/SheepShaver 2>/tmp/jit.log            Capture JIT miss report on exit"
	@echo ""
	@echo "  See also: CONTRIBUTING.md, docs/planning/OPTIMIZATION-PLAN.md, docs/TESTING.md"

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
	@echo ""
	@echo "TIP: Run tests with:  make test-jit    (JIT codegen gate)"
	@echo "     Quick benchmark:  cd rom-harness && make bench"
	@echo "     Boot emulator:    ./src/Unix/SheepShaver"
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
	@echo ""
	@echo "TIP: For quick perf A/B, run:  cd rom-harness && make bench"
	@echo "     For differential JIT verify:  SS_JIT_VERIFY=1 ./src/Unix/SheepShaver"

# test-opcodes: DEFAULT mode = interpreter determinism (each vector run twice
# through the interpreter, REGDUMPs diffed).  This validates the interpreter, NOT
# the JIT codegen.  For codegen changes, use `make test-jit` (the real gate).
test-opcodes: build-ss
	@echo ""
	@echo "=== Interpreter Determinism Harness (does NOT exercise JIT codegen) ==="
	@cd jit-test && bash run.sh 2>&1 | grep "METRIC\|FAIL\|ERROR" | head -20
	@echo ""
	@echo "TIP: This only tests the interpreter. To test JIT codegen, run:"
	@echo "     make test-jit"

# test-jit: the JIT correctness gate.  Runs every vector through BOTH the
# interpreter (reference) and the aarch64 JIT, and diffs the REGDUMPs
# (SS_HARNESS_MODE=jit).  THIS is what proves a codegen change is correct —
# run it on every change to ppc-jit.cpp.  Includes the RA register-pressure /
# eviction stress vector (lmw_stmw_wide).
test-jit: build-ss
	@echo ""
	@echo "=== JIT<->Interpreter Equivalence Harness (codegen correctness gate) ==="
	@cd jit-test && SS_HARNESS_MODE=jit bash run.sh 2>&1 | \
		grep -E "HARNESS mode=|HARNESS quarantine:|METRIC (pass|fail|total|score)=|METRIC quarantine_(total|xfail|xpass)=|opcode_[a-z0-9_]+=(0|-1)|QUARANTINE-XPASS|FAIL|ERROR"
	@echo ""
	@echo "TIP: For deeper verification, boot with:  SS_JIT_VERIFY=1 ./src/Unix/SheepShaver"
	@echo "     For quick perf A/B:  cd rom-harness && make bench"

# --- End-to-end VNC harness (ROADMAP A5) -------------------------------------
#   make e2e         boot -> clean-shutdown smoke (the system-level regression gate)
#   make e2e-bench   boot Mac OS 9 + Speedometer, drive the suite over VNC, capture results
#   make e2e-test    offline unit tests (no emulator, no assets, no GUI)
#   make e2e-setup   guided setup check — venv, build, Homebrew libs, assets (offers to link them)
#   make e2e-clean   remove the venv
# The venv auto-bootstraps on first use (cached, like `npm install`) — no manual setup.
# e2e / e2e-bench need a logged-in macOS GUI session (SDL opens a real window). See e2e/README.md.

# One-time venv bootstrap: a marker file, regenerated only when requirements.txt changes.
e2e/.venv/.installed: e2e/requirements.txt
	@echo "=== Setting up the e2e Python venv (one-time; ~30s) ==="
	@cd e2e && python3 -m venv .venv \
		&& .venv/bin/pip install -q --upgrade pip \
		&& .venv/bin/pip install -q -r requirements.txt
	@touch $@

e2e-setup: e2e/.venv/.installed
	cd e2e && .venv/bin/python doctor.py

e2e: build-ss e2e/.venv/.installed
	cd e2e && .venv/bin/python run_smoke.py

e2e-smoke: e2e

e2e-bench: build-ss e2e/.venv/.installed
	cd e2e && .venv/bin/python run_benchmark.py

# Real-world-app workload: boot the workload disk + apps disk, launch a real app and time its
# render via screenshot/pHash, export a per-workload perf-history row. WL overrides the workload
# (default fractal-carbon). Needs a GUI session + the workload/apps disks (see e2e/README.md).
e2e-workload: build-ss e2e/.venv/.installed
	cd e2e && SS_INPUT_LOCKOUT=1 .venv/bin/python run_workload.py $(WL)

e2e-test: e2e/.venv/.installed
	cd e2e && .venv/bin/python -m pytest -q

e2e-clean:
	rm -rf e2e/.venv

ui-dump: e2e/.venv/.installed
	@test -n "$(SS_UI_DUMP_DIR)" || { echo "Set SS_UI_DUMP_DIR (the dir the running emulator uses)"; exit 1; }
	cd e2e && .venv/bin/python -m sse2e.uidump "$(SS_UI_DUMP_DIR)"

test-rom: build-rom-harness
	@echo ""
	@echo "=== ROM Harness (10K blocks) ==="
	@timeout 120 $(ROM_HARNESS) $(ROM) \
		--count=10000 --min-insns=1 --max-insns=64 2>&1 | \
		grep "Score:\|Passed:\|Failed:\|Testable\|JIT compile\|SIGSEGV"

# harness-count: the AUTHORITATIVE harness vector count, derived from the one real
# source — the TEST_ORDER entries in jit-test/run.sh. Docs cite dated snapshots
# that drift; this never does. Use it instead of trusting a hardcoded number.
harness-count:
	@printf 'harness vectors (TEST_ORDER in jit-test/run.sh): '
	@grep -cE '^TEST_ORDER\+=\(' jit-test/run.sh

ui-introspect-test: ui-introspect-serialize-test
	c++ -std=c++17 -Wall -o /tmp/ui_introspect_test src/ui_introspect_test.cpp
	/tmp/ui_introspect_test

# Offline unit test of the memory-walking serializers (ui_introspect.cpp) against a mock big-endian
# RAM via the stub headers in src/uitest/ (selected by -I order, real build untouched).
ui-introspect-serialize-test:
	c++ -std=c++17 -Wall -I src/uitest -o /tmp/ui_introspect_serialize_test src/ui_introspect_serialize_test.cpp
	/tmp/ui_introspect_serialize_test

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
