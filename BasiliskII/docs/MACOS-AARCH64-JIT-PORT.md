# BasiliskII — macOS arm64 build/JIT port status (deferred work)

**As of 2026-06-04.** Captured while integrating upstream backports; the BasiliskII
macOS arm64 build was found to be broken in layers. Two layers fixed; the JIT-backend
port is deferred to a dedicated effort (boot verification required — see below).

> ⚠️ **CLAUDE.md's "BasiliskII … harness 301/301, score=100" is stale on macOS.** The B2
> AArch64 JIT backend does not currently compile on macOS arm64 (see Remaining, below), so
> that figure reflects a Linux/older build, not the current macOS arm64 tree. SheepShaver is
> unaffected and fully working (SDL3 + VDE, harness 257/257).

## Fixed (committed on `integration/upstream-backports`)

1. **Configure mis-routed Apple Silicon to the 32-bit ARM asm path** (`c71100d0`).
   `config.guess` reports `target_cpu=arm` on Apple Silicon (`arm-apple-darwin`), so
   `configure.ac` selected the `HAVE_ARM` (32-bit ARM/ARMv6) branch — emitting
   `-DARM_ASSEMBLY -DARMV6_ASSEMBLY` 32-bit inline asm that fails to compile on arm64
   (`cpuemu.cpp`). Fix: route Darwin `arm` → `HAVE_AARCH64` (there is no 32-bit ARM macOS).
   Side effect: restored `Use JIT compiler: yes` and `Assembly optimizations: AArch64`
   (the JIT was silently disabled before because the 32-bit branch keyed off
   `--enable-arm-jit-experimental`, not `--enable-aarch64-jit-experimental`).

2. **Linux-only code in `main_unix.cpp`** (`2f967f4b`): `sigill_handler_diag` used the
   Linux `uc_mcontext.pc/.regs[]` layout (added Darwin `__ss` branch); the DIRECT_ADDRESSING
   I/O mmap used Linux-only `MAP_FIXED_NOREPLACE` (ported with a map-at-hint + unmap-if-not-
   honored fallback preserving no-clobber semantics). `main_unix.o` now compiles.
   These Linux-isms were introduced by **`3c6c2106`** ("ARM64 JIT: critical fixes …
   I/O mmap, ROM patch"). *(Errata: the VDE commit message `3bfa680a` mis-cited the
   introducing commit as `c7fb557c` — that commit is "self-disable ASLR on aarch64 Linux"
   and is unrelated; `3c6c2106` is correct, as `2f967f4b` states.)*
   *(Runtime placement of the I/O ranges on macOS is unverified — needs a boot.)*

## Remaining (deferred — the B2 AArch64 JIT backend is unported to macOS)

With the routing fixed, the build now reaches and fails in the JIT backend
`BasiliskII/src/uae_cpu_2026/compiler/compemu_support_arm.cpp` with Linux-isms:

| Site | Error |
|------|-------|
| `compemu_support_arm.cpp:122,127` | `uc_mcontext.pc` / `.regs` — Linux mcontext layout; macOS is `uc->uc_mcontext->__ss.__pc` / `__x[]`. Same fix pattern as `main_unix.cpp` above. |
| `compemu_support_arm.cpp:2303,2318` | `uae_vm_jit_write_protect` undeclared — the **W^X / MAP_JIT toggle**. macOS needs `pthread_jit_write_protect_np(bool)` + `sys_icache_invalidate()`. |
| `compemu_support_arm.cpp:4780` | `uae_vm_page_size` undeclared — needs a macOS page-size accessor (`sysconf(_SC_PAGESIZE)` / `getpagesize()`). |
| `<sys/ucontext.h>:51` | "deprecated ucontext routines require `_XOPEN_SOURCE`" — define `_XOPEN_SOURCE` before the ucontext include on Darwin. |

(Expect further cascades after these — this file has never compiled on macOS arm64.)

### Why this is a dedicated effort, not a quick fix

The mcontext / `_XOPEN_SOURCE` / page-size items are mechanical. **`uae_vm_jit_write_protect`
is not:** it is the executable-memory W^X toggle, and on Apple Silicon getting the
`pthread_jit_write_protect_np` + `sys_icache_invalidate` sequence wrong makes the JIT
**silently emit/execute garbage** rather than fail loudly. That correctness **cannot be
validated by the opcode harness** (which only diffs interpreter REGDUMPs) — it requires a
real **boot with `B2_JIT_MAX_OPTLEV=2`** and exercising native codegen. So a clean compile
of this backend would still be **UNVALIDATED**; do not treat "it builds" as "it works."

### Suggested approach when picked up

1. Apply the mechanical fixes (mcontext Darwin branch, `_XOPEN_SOURCE`, page-size).
2. Provide the macOS W^X implementation for `uae_vm_jit_write_protect` (mirror SheepShaver's
   `jit-target-cache.hpp` MAP_JIT handling — that path is proven on this fork).
3. Resolve the cascade; get a clean compile + link.
4. **Boot-verify** with `B2_JIT_MAX_OPTLEV=2` (and `=1`, `=0` for comparison); run the 301
   harness; only then update CLAUDE.md's B2 status to reflect a real macOS arm64 build.

### Interpreter-only fallback (alternative)

A `--disable-jit-compiler` (interpreter-core) build may give a running B2 + passing 301
harness on macOS sooner, deferring the JIT-backend port. Not yet attempted; the interpreter
path's own macOS-build cleanliness is unconfirmed.
