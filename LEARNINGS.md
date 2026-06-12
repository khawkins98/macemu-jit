# LEARNINGS — macOS ARM64 JIT work

Durable lessons that prevent repeating expensive mistakes. Newest entries at the top.
For the full historical session journal: `docs/archive/2026-06/LEARNINGS-2026-06.md`.

---

## AltiVec bottom line (⭐ read first)

Real-app AltiVec WORKS end-to-end as of 2026-06-07. The AArch64 JIT always compiles
PPC AltiVec→ARM64 NEON (validated by `make test-jit`, 353/353). The only gap was
detection: under the OldWorld 1.1 ROM the guest never registers the `'ppcf'` gestalt.
Fix = opt-in `altivec` pref → `SS_FORCE_ALTIVEC` env registers `'ppcf'` via `_NewGestalt
$A3AD` with bit `0x10` (= `1<<gestaltPowerPCHasVectorInstructions`, NOT `0x40` — the
0x40 typo caused a multi-hour "FC ignores gestalt" detour). Verified: Fractal Carbon
detects AltiVec and runs its vector kernel through the JIT (`AltiVec=160`). Opt-in/
default-off because 8.6/9.0 don't VR-context-switch (single-app-safe).

---

## 2026-06-12 — Use SS_PROBE_68K for 68k code paths

`SS_PROBE_PC` fires at PPC JIT block-entry addresses. It is **completely blind** to 68k
ROM addresses. When investigating a 68k interrupt handler (e.g. 0x5000ed08), the right
tool is:
```bash
SS_PROBE_68K=0x5000ed08:5 SheepShaver/tools/ss-slot-boot.sh --label probe --timeout 25
```
`SS_PROBE_68K=0xPC:N` fires at the DR dispatch hook, first N matches (linear), shows the
68k regfile (d0–d7, a0–a7) and PPC context.

**Rule: when the bug is in 68k code, start with SS_PROBE_68K. Use SS_PROBE_PC only for
PPC-world investigation.**

---

## 2026-06-12 — QEMU rig: five pitfalls + one stage-matching rule

Tools: `SheepShaver/tools/qemu-rig.sh` + `qemu-mon.py`. Full pitfall context also in
`docs/AGENT-CONTEXT.md` "Instruments" section.

1. **ANSI escape sequences in the monitor.** Strip with `re.sub(r'\x1b\[[0-9;]*[A-Za-z]',
   '', ...)` AND skip everything before the first `\n`. Use `qemu-mon.py` — it handles this.
   The monitor accepts only ONE connection at a time; `ConnectionRefusedError` after a stale
   session → kill and restart QEMU.

2. **`xp` reads physical RAM; `x` reads virtual (guest) address space.** After the NK
   enables the MMU, always use `x` for Mac OS guest memory. `xp /wx 0x168` returns zero;
   `x /wx 0x168` returns the live Ticks counter.

3. **QEMU mac99 MMIO addresses differ from real hardware.** MacIO BAR0=`0x80000000` in
   QEMU, `0xF3000000` on real G4. Our SheepShaver targets `0xF3xxx`. Use QEMU for
   **behavioral** reference, never for addresses.

4. **The level-1 interrupt handler address is heap-allocated.** Always read `x /1wx 0x64`
   first. Don't hardcode `0x47d0ba`. During boot the vector changes rapidly — read it once,
   reuse that value.

5. **`btst d6,(a4)` at 0x5000ee9a was WRONG.** `0x5000ee9a` is mid-word of a 4-byte
   `tst.l $d94.w` at `0x5000ee98`. Verify all runtime observations against the static ROM
   disassembly before building on them.

6. **Probe at the right boot stage.** QEMU 50s probes show Finder-steady-state. Our
   SheepShaver first-EXT-interrupt is equivalent to QEMU's 3–5s window. Compare like with
   like. The rig's `--ladder` option probes at 3s, 5s, 10s, 30s.

---

## 2026-06-05 — Rule out boring causes first

When a harness "doesn't work": rule out the cheap causes FIRST — wrong coordinates,
clicking empty desktop, a noisy signal (idle hook emits spurious one-off `frontApp='Finder'`
frames), a stale binary — with one clean functional test BEFORE theorizing about internals.
Three "deep bug" theories (RawMouse Toolbox bug, direct-ADB rewrite, hold-the-button) all
dissolved under one proper A/B. See also: VNC clicks were NEVER broken (LEARNINGS archive,
2026-06-05).

---

## 2026-06-02 (session 5 RETRACTION) — "STUCK"/HOT-PC detections are sampling artifacts

A "spin-wait deadlock" theory at 0x50313d34 was built from register-state inference without
disassembling the actual instructions. When the ROM was finally dumped and run through
capstone, every pillar collapsed:

1. **0x50313d34 is NOT a spin-wait.** It is the NK exception/interrupt dispatcher — a
   comparison chain that dispatches from a table. It is the hottest PC in the system.
2. **"STUCK at pc=50313d34" is a sampling artifact.** Two consecutive 5-second heartbeats
   on the same PC does NOT mean the guest is hung. The system was never stuck.
3. A guest-memory-corrupting "fix" (`a46cda99`) was committed on this theory and had to
   be reverted (`9f9e617e`).

**Methodology rule:** never infer code behavior from register state alone. Disassemble the
actual instructions first (single lldb attach + memory dump + capstone, then detach
immediately).

---

## 2026-06-02 (session 2) — VBL timer death from repeated lldb attach/detach

Multiple `lldb -p PID … -o detach` invocations in sequence cause the macOS kernel to
defer the 60 Hz VBL timer. The ROM's early-boot spin-wait at 0x5031040c loops forever
without ticks. **Fix:** attach lldb AT MOST ONCE per run, do minimal investigation,
detach immediately. If the emulator gets stuck in a 2-block loop with all-zero a0-a7
registers in the trace ring → the VBL timer died → restart.

---

*For the full session journal (pre-archive), see `docs/archive/2026-06/LEARNINGS-2026-06.md`.*
