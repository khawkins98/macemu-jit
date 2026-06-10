# Spike S2 — Mach fault-decode keystone proof

> **Status:** ✅ COMPLETE — keystone VALIDATED end-to-end (2026-06-10)
> **Plan:** `docs/planning/MACHINE-LAYER-PLAN.md` §3 (pre-M0 spikes), proving §2b's JIT
> dispatch path before M1 designs the bus around it.
> **Code:** `spikes/s2-mach-fault-decode/` (`make && ./s2-fault-decode [N]`)
> **Environment:** Apple M5, macOS 26.4.1, SIP enabled, ad-hoc linker-signed binary
> (no entitlements, no hardened runtime).

## 1. What was tested

A standalone program (no SheepShaver code modified or linked) that reproduces the exact
§2b JIT-path chain:

1. **Trap** — a 3-page `mmap` reservation with the middle 16 KB page `PROT_NONE`
   (the "device hole in NATMEM" analogue); `EXC_BAD_ACCESS` routed to a dedicated Mach
   exception-handler thread via `thread_set_exception_ports(…, EXCEPTION_DEFAULT |
   MACH_EXCEPTION_CODES, ARM_THREAD_STATE64)` + a `mach_exc_server` loop — the same
   shape as `sigsegv.cpp` (`handleExceptions`, line ~727), with `mach_excServer.c`
   MIG-generated from the SDK's `mach_exc.defs` at build time.
2. **Decode** — the faulting access is the *exact JIT-emitted form* from
   `ppc-codegen-aarch64.h` / `ppc-jit.cpp` (lwz, case 32): `LDR Wt, [Xn, Wm, UXTW]`
   (`0xB8604800 | rm<<16 | rn<<5 | rt`, fixed-bits mask `0xFFE0FC00`) followed by
   `REV Wt, Wt` (`0x5AC00800`), hand-assembled via `.inst` so the bytes match the JIT.
   The handler reads `ARM_THREAD_STATE64` with `thread_get_state`, fetches the
   instruction word at PC (same task — direct read), and extracts Rt/Rn/Rm.
3. **Inject + resume** — writes **raw big-endian** `0x12345678` zero-extended into
   `__x[rt]`, advances PC by 4 (past the LDR only, NOT the REV), `thread_set_state`,
   returns `KERN_SUCCESS` from `catch_mach_exception_raise`.
4. **Observe** — the resumed thread executes the REV for real and the caller asserts it
   sees `bswap32(0x12345678) = 0x78563412`.
5. **Benchmark** — 100 k faulting loads vs 100 k mapped loads through the same code.
6. **MAP_JIT variant** — the same 3 instructions copied into a
   `MAP_JIT` + `pthread_jit_write_protect_np` + `sys_icache_invalidate` page and
   executed from there (the real JIT's home — this was a flagged known-unknown).

## 2. Results

| Test | Result |
|---|---|
| A. Mapped-load sanity (asm form correct) | PASS (`0xddccbbaa` from BE `0xAABBCCDD`) |
| B. **Keystone**: fault → decode → inject → resume → REV observed | **PASS** (`0x78563412`, exactly 1 fault serviced) |
| C. Same load executed from a MAP_JIT page (mapped + faulting) | **PASS / PASS** — identical behavior to linker-text |
| D. 100 k-fault benchmark | all 100 k faults serviced, zero decode failures |

**Measured cost (Apple M5, 4 runs of N=100 k + one N=20 k):**

| Path | ns per access |
|---|---|
| Mapped baseline (call + LDR + REV) | **0.5 – 1.1 ns** |
| Mach fault round-trip (message → get_state → fetch+decode → set_state → reply → resume) | **≈ 5.8 – 9.6 µs** (typical ~8.5–9 µs) |
| Slowdown | **~10⁴×** |

The round-trip is dominated by the kernel exception-message + thread-suspend machinery,
not by our decode (a single uncached instruction fetch + bit tests).

## 3. What worked / what didn't

**Worked, first try, with no platform friction:**
- The full chain (trap → decode → register writeback → PC advance → resume) on both a
  linker-text page and a MAP_JIT page. No difference between the two.
- The §2b **endianness contract** is confirmed mechanically: injecting raw BE and letting
  the resumed REV swap is correct and requires no special handling — just "skip the LDR,
  not the REV".
- `MACH_EXCEPTION_CODES` `code[1]` reliably carries the 64-bit faulting *data* address —
  the bus gets the device address for free, without recomputing `Xn + UXTW(Wm)` from
  registers (though Rn/Rm are decoded and available as a cross-check).
- MIG generation is trivial (`mig -arch arm64 $SDK/usr/include/mach/mach_exc.defs`);
  no SheepShaver build entanglement needed for the spike.

**Nothing in the chain failed.** No CRITICAL findings.

**Platform surprises: none of the feared ones materialized.**
- No entitlements needed: ad-hoc linker-signed binary, SIP enabled, no hardened runtime,
  no `com.apple.security.cs.debugger` — a process may handle its **own** thread's
  exceptions via a thread-level port without any privilege. (Task-level ports on *other*
  tasks are where entitlements bite; we don't need that.)
- `MAP_JIT` mmap + `pthread_jit_write_protect_np` worked unsigned/non-hardened (matches
  the existing SheepShaver JIT experience).
- One correctness subtlety pre-empted rather than hit: on arm64e-style states PC is
  PAC-signed in the thread state; using `arm_thread_state64_get_pc` /
  `arm_thread_state64_set_pc_fptr` (instead of poking `__pc`) keeps the handler correct
  regardless. Setting a W register means writing the **zero-extended 64-bit** `__x[rt]`
  (LDR W zero-extends), and `rt == 31` must be treated as WZR (skip the write).

## 4. Implications for M1

1. **Keystone VALIDATED.** The §2b JIT dispatch path (unmapped device pages + Mach fault
   decode + thread-state writeback) is buildable exactly as designed; the bus can be
   designed around it with confidence. The decoder contract — "the JIT memory emitters
   are a deliberately restricted, decodable set" — holds: one mask/value pair per
   width-direction pair covers every `a64_{ldr,str}{b,h,w,x}_reg` form
   (`{0x38,0x78,0xB8,0xF8}{60=load,20=store}4800` with mask `0xFFE0FC00`).
2. **Backpatch is MANDATORY for hot sites, confirmed by measurement.** At ~8.5 µs/fault,
   the observed ~2.6 M iter/s nanokernel idle poll through the fault path would cost
   ~22 wall-seconds per guest-second — the plan's rev-3 "hours-per-second" order-of-
   magnitude warning is the right call (it's tens of seconds, but equally fatal). The
   fault path is viable **only** as the discovery mechanism + cold-path; M1's
   "backpatch in scope, not reserve" and the idle-detection hook are both load-bearing.
   Budget guide: ~100 k faults/s saturates one core; a *boot-time* probe doing
   hundreds-to-thousands of device touches costs milliseconds — fine.
3. **Decoder gotchas to carry into M1:**
   - Skip the LDR only; the REV must execute (endianness contract). Same applies to
     REV16 after LDRH. For **stores**, the REV runs *before* the faulting STR, so the
     value in Wt at fault time is already raw BE — symmetric and equally convenient.
   - `rt == 31` is WZR/XZR on this form (register-offset loads): discard the value but
     still perform the device read (side effects), then skip.
   - Byte loads (`lbz`/LDRB) have **no** REV — the injected byte is used as-is; the
     decoder must key the inject format off the access width, which it gets from the
     size bits (31:30) of the same instruction word.
   - Use the `arm_thread_state64_{get,set}_pc*` accessors, never raw `__pc`.
   - The handler thread reads the faulting instruction via plain memory access — fine
     same-task, but the JIT must never *unmap* translation-cache pages concurrently
     (it doesn't; cache reuse is by reset, not unmap).
4. **§2g rules are exercised implicitly:** the handler ran with zero heap allocation and
   no foreign locks (one `fprintf` on the abort path only — production must ring-buffer
   even that). The spike's handler is single-fault-at-a-time by construction, same as
   `sigsegv.cpp`'s single handler thread.
5. **Bench caveat:** the loop faults at ONE site with a hot instruction cache and an
   idle system — this is a *floor* for the per-fault cost, not a model of mixed
   workloads. Real M1 telemetry (per-region fault-rate logging) remains necessary.

## 5. Reproduce

```bash
cd spikes/s2-mach-fault-decode
make            # mig + clang++, no autoconf
./s2-fault-decode          # default N=100000
./s2-fault-decode 20000    # custom fault count
```

Caveat from §2g rule 5: running under lldb contends the EXC_BAD_ACCESS port and will
perturb (or break) the spike — run it bare.
