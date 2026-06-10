# M6a groundwork — the DR-Emulator handoff wall: dispatch-table contract, cold-start anatomy, and the road past the Thud console

> **Status:** 📋 Static-analysis memo + live probe forensics (no builds, no further boots) ·
> **Created:** 2026-06-10 · **Branch:** `macos-arm64`
> **Inputs:** decompressed 9.0.1 ROM `/tmp/rom901_decompressed.bin` (capstone PPC BE, base
> `0x50000000`; the runtime-staged NK is byte-identical at **static+0x100000** — probe-verified
> in `M3A-ENTRY-TABLE.md` — so 0x503xxxxx static addresses below are valid for the live
> 0x504xxxxx kernel; the dump file itself is exactly 4 MB, so 0x4xxxxx mirror addresses must be
> read at their 0x3xxxxx static sources); `M5-MMU-SR-WALL-ANALYSIS.md` (format template);
> `M3A-ENTRY-TABLE.md`; `HANDOFF-NEWWORLD-SUPERVISOR-MMU.md` §1.6/§1.7/§2;
> `SPIKE-S3-STALL-DEVICE-PROBE.md` §2.5; `sheepshaver_glue.cpp` `[NW-TRAMP]` block
> (~:1737–2010); `rom_patches.cpp` (`patch_nanokernel_boot` :787ff, jump68k redirect :1304ff,
> io_poll NOPs :1310-region, `PatchROM_NW_trampoline` :718ff); logs `/tmp/spike-macos9.log`,
> `/tmp/m3a-bootA2.log` (console-spin signature: `comp` frozen at 781, ~14M blocks/s,
> `exc=0/1/0`, `jDR` frozen at 8); **live probe forensics `/tmp/forensics1.log`
> (orchestrator-run, 2026-06-10)**.
> **Caveat:** static register *values* are reasoned from data flow; probe-verified facts are
> marked **[PROBE✓]**. Remaining unverifiables are tagged **[PROBE-n]** and collected in §7.
> Note the probe limitation confirmed this session: **probes fire only on dispatcher-entered
> blocks** — chained blocks (e.g. the staged yield prologue `0x504272e0`, idle entry
> `0x5042751c`) never trip, so a silent probe there is *inconclusive*, not negative evidence.

---

## 0. TL;DR — the verdict

**The NK handoff is NOT the gap.** The existing, scattered Trampoline synthesis — the
ConfigInfo `LA_*` patches at ROM+0x30d000 (`patch_nanokernel_boot`), the `sr_init` r13/r14/r15
seeds, and the `[NW-TRAMP]` KDP/ECB block — **demonstrably delivers the boot context into the
DR (68k) Emulator**: the probe at `0x5046e8c0` (DR entry table[0], the `PatchROM_NW_trampoline`
redirect target) shows **visit=1 at ~0.01 s [PROBE✓]** — dispatched exactly once. The 68k
world then **dies within 8 JIT blocks** (`jDR` frozen at 8 forever; `comp` frozen at 781;
region `0x50360000` never compiles in any boot [PROBE✓]), and the NK parks in its **designed**
idle/console path (the yield primitive's idle arm — 106 of 107 callers `crset cr1eq`;
`KDP+0xedc` bits are ConfigInfo-driven feature flags set by normal init, not a panic flag; the
`KDP−0x340` "−1 poison" is NK-written by design [PROBE✓ + static]).

**The smoking gun [PROBE✓]:** at `0x5046e974` (inside the DR cold-start), **r29 =
`0x5046e000`** — the hardcoded constant from the rom_patches register-fixup trampoline (:741).
The DR dispatcher computes `handler = (r29 & 0xFFF80007) | (opcode<<3)`
(`rlwimi r29,r27,3,0xd,0x1c; mtctr r29; bctr`, static `0x5036e9dc`, mirror `0x5046e9dc`).
**Three inconsistent dispatch-table values coexist** in our synthesis:

| Source | Value | Implied 512 KB table base (top 13 bits) |
|---|---|---|
| fixup trampoline r29 (rom_patches :741) | `0x5046e000` | **`0x50400000` ✗** (no table there — it's the mirror NK/emulator code) |
| ConfigInfo `LA_DispatchTable` (+0xa8, rom_patches :797) | `ROMBase+0x480000` | `0x50480000` ✓ (mirror table, verified fully populated) |
| `[KDP+0x648]` (`[NW-TRAMP]` log; live [PROBE✓]) | `0x50380000` | `0x50380000` ✓-shaped (primary-world table) |

With r29's base = `0x50400000`, the first 68k opcode (`0x4efa`, `jmp`, at
`guest[4]=0x5000002a` [PROBE✓: r24=0x5000002a, r31=0x68fff000, `[0x0]`=0x00100000,
`[0x4]`=0x5000002a at DR entry]) dispatches to a garbage handler address inside the mirror
NK — static arithmetic predicts `0x50400000|0x4efa<<3 = 0x504277d0`, which is a `bl` inside
the **Thud console help-text printer** ("id idvalue — Obtain opaque ID info", static
`0x503277d0`); [PROBE-1] chases the actual death block-by-block. Either way: the 68k boot
process never executes a single correct opcode handler.

**Also probe-refuted:** the jump68k redirect at `0x3126cc`/`0x4126cc` **never executes**
(zero visits at both [PROBE✓]) — it is not the diverter; but its patch *drops the `mtsrr1`*,
i.e. the EE=1 user-mode MSR transition (`UserModeMSR=0x0000D032`) — a recorded fidelity bug
in waiting now that M3a delivery respects EE (§5.3).

**M6a scope (smallest honest fix, rungs in §6):** reconcile the dispatch-table contract to a
single world (rung 1 = make r29/ctx-saved-r29/ctx-resume mirror-coherent with
`LA_DispatchTable=0x50480000` — a three-constant repair), then the named follow-on gaps:
ongoing-entry support (the rom_patches ":712 TODO"), the dropped-mtsrr1 EE transition, the
five `io_poll_beq` NOPs mutating the staged NK's SCC code (Path-A artifacts contradicting the
M1 SCC model), and the `patch_68k` shim desert (effectively zero HLE shims installed on
9.0.1) — the *named* next wall even after the dispatch is fixed. This matches
MACHINE-LAYER-PLAN's M6 row verbatim ("DR Emulator cold-start ECB/dispatch-table completion")
and HANDOFF §2.8 Phases 1–2.

---

## 1. Probe-verified state of the handoff (forensics, 2026-06-10)

From `/tmp/forensics1.log` (orchestrator-run boots) + the m3a-bootA2 heartbeat record:

1. **The dispatch fires, once.** `0x5046e8c0` (table[0]) visit=1 at ~0.01 s. The handoff
   chain — table[0] → fixup trampoline (`0x50429b40`, region-first-compile in the log) →
   DR cold-start (`0x5046e964`) — executes.
2. **The 68k world dies in <8 blocks.** `jDR` frozen at 8 across every heartbeat;
   `comp` frozen at 781; region `0x50360000` (primary emulator, incl. the *patched*
   SheepShaver entry `0x5036f900`) never compiles in any boot.
3. **r29 = `0x5046e000` at `0x5046e974`** — the fixup trampoline's constant, alive at the
   point where only the dispatch arithmetic remains before the first bctr.
4. **Seeds confirmed live at DR entry:** r24=`0x5000002a` (68k reset PC), r31=`0x68fff000`
   (ECB), `[0x0]`=`0x00100000` (68k SSP), `[0x4]`=`0x5000002a` — the fixup trampoline's
   writes all landed.
5. **The jump68k redirect never executes:** probes at `0x503126cc` and `0x504126cc` show
   zero visits. The "Returning to boot process" rfi-block (§2.1) is *designed-endgame
   documentation*, not the live route; how table[0] is actually reached is [PROBE-2].
6. **Console-entry identification stands:** the spin (staged `0x50426884–0x50426b1c`
   check_work cluster + `0x5042751c` idle loop) is the yield primitive's **designed idle
   arm**. `KDP+0xedc` bits = ConfigInfo-driven feature flags from normal init;
   `KDP−0x340 = −1` is NK-written by design. No panic, no corruption — the NK is parked
   honestly because its boot process died.
7. **Probe blind spot (methodology):** probes fire only on dispatcher-entered blocks;
   chained blocks (yield prologue, idle entry) never trip. Silent probes there prove
   nothing. (Same class as the M3A "alarm-killed boots skip the atexit stats dump" finding —
   see §5.5 telemetry follow-up.)

---

## 2. Static anatomy of the handoff

### 2.1 The NK's designed endgame (static; the live route differs — [PROBE-2])

Cold-init's two exits both funnel into one dispatch, announced by `bl`-over-string prints
(string address lands in LR for the print stub `0x503263e0`):

```
0x50312628  bl 0x50312660            ; LR → "Nanokernel replaced. Returning to boot process\n"
0x50312660  mflr r8 ; bl 0x503263e0  ; print
0x50312668  addi r9,r1,0x420 ; mtspr SPRG3,r9
0x50312670  b 0x503126cc             ; → dispatch tail
0x50312674  bl 0x50328a00            ; banner helper (region 0x50320000 first-compile in live log)
0x50312678  bl 0x503126b4            ; LR → "Nanokernel NOT replaced. Returning to boot process\n"
0x503126b4  mflr r8 ; bl 0x503263e0
0x503126bc  lwz r8,0x5a0(r1) ; mtspr SPRG0,r8       ; ctx
0x503126cc  lwz r4,0x648(r1)                        ; ← jump68k pattern (lwz r4,0x648; lwz r8,0x5a4; lwz r9,-0x964)
0x503126d0  lwz r8,0x5a4(r1)                        ; emulator code base
0x503126d4  lwz r9,-0x964(r1)                       ; UserModeMSR (0x0000d032)
0x503126d8  addi r8,r8,0x26e8                       ; entry = code_base + 0x26e8
0x503126dc  mtsrr0 r8   [patched → mtctr r8]
0x503126e0  mtsrr1 r9   [patched → bctr]            ; ★ the EE transition the patch DROPS (§5.3)
0x503126e4  rfi         [patched → nop]
0x503126e8  li r3,0xff ; mtlr r4 ; blrl             ; alternate call-with-LR entry (vector table in r4)
```

The replaced-vs-not decision is the `'Hnfo'` IRP-signature check at `0x503109f4–fc`
(`[[KDP−0x20]+0xf70] == 0x486e666f`); our zeroed IRP takes "NOT replaced" — correct for a
cold boot. **Live probes show this tail never executes** — yet the banner helper compiled
and table[0] was entered. The actual branch into `0x5046e8c0` is unidentified (candidates:
the `blrl` variant, another cold-init exit in the skipped/patched region, glue-side entry);
one LR/CTR probe at table[0] settles it ([PROBE-2]).

### 2.2 The ConfigInfo contract — the synthesis that already works

`init_emul_ppc` enters the NK with `r3 = ROMBase+0x30d000` (ConfigInfo) and
`r4 = KernelDataAddr+0x1000`; `patch_nanokernel_boot()` (rom_patches.cpp:792ff) writes the
block — this *is* the Trampoline-handoff synthesis, inherited from the OldWorld mechanism:

| ConfigInfo field | rom_patches writes | Static NK consumer | Produces |
|---|---|---|---|
| +0x9c `LA_InfoRecord` | `KernelDataAddr` | info record | — |
| +0xa0 `LA_KernelData` | `KernelDataAddr` | cold-init | — |
| +0xa4 `LA_EmulatorData` | `0x68fff000` | `0x5031099c` | ctx+0x94 |
| +0xa8 `LA_DispatchTable` | **`ROMBase+0x480000`** | `0x503109a4` | **ctx+0x9c** — the opcode<<3 table |
| +0xac `LA_EmulatorCode` | `ROMBase+0x460000` | `0x50310984`, `0x503107f0` | (+0x84=`0xe8c0`): `KDP+0x648 := 0x5046e8c0` — the entry *vector* table |
| +0xb0 (`0xff4`) | (in ROM) | pair-table loop `0x503109c8–e8` | low-memory globals (XLM block, `0x2800 'Baah'`) |
| +0xfd8 | `ROMBase+0x2a` | — | 68k reset vector |

Caveat from the live data: `[KDP+0x648]` was observed as **`0x50380000`** (glue's seed)
[PROBE✓], meaning the static writer `0x503107fc` (`KDP+0x648 = [r3+0xac]+[r3+0x84]`) did
**not** run on this boot — consistent with the boot patch at `0x310000+8` branching past
`0x31000c–0x3101af` (the "Skip SR/BAT/SDR init" patch), which also skips the architectural
writers of `KDP+0x5a0/+0x5a4` (`0x503100a0/a4`), `[KDP−0x900]=r5` (`0x503100b8`),
`[KDP−0x20]`, UserModeMSR, and SPRG0 — *why* the `[NW-TRAMP]` block must seed those fields,
and why glue's `[KDP−0x900]=0xF3012000` SCC base survives to be polled (Wave-0).

### 2.3 Dispatch-table geometry (item 1 — derived)

The dispatcher (cold-start tail, static `0x5036e9b4–e9e8`):

```
lwz  r1, 0(r24=0)        ; r1  = [guest 0]  = 68k SSP
lwz  r24, 4(r24)         ; r24 = [guest 4]  = 68k PC
...
lha  r27, 0(r24)         ; fetch 68k opcode (sign-extended)
rlwimi r29, r27, 3, 0xd, 0x1c   ; r29 = (r29 & 0xFFF80007) | ((op & 0xFFFF) << 3)
mtctr r29 ; lhau r27, 2(r24) ; bctr
```

**Geometry:** one 8-byte slot per 16-bit opcode → a 512 KB table, base = r29's top 13 bits
(bits 0–12, mask `0xFFF80000`), low 3 bits of r29 preserved (0 here). Each slot = 2
instructions (a setup op + a relative `b` into the emulator body). r29's base is **never
re-derived inside the cold start** (the only writers in `0x36dd74–0x36e964` are two
`rlwimi` inserts — supervisor-bit at `0x36df00`, opcode at `0x36df04`/`0x36e9dc`); it is
inherited from the entry path. The architectural suppliers:

- The ROM's own prologue at `0x5036e900` (`twui r31,0x10; … mr r31,r3; mr r29,r4;
  stw r29,0x74(r31)` then `b table[0]`) takes r4 = the **vector** table and stashes it in
  ED+0x74; the *opcode* table reaches r29 via the context-restore path from **ctx+0x9c =
  `LA_DispatchTable`** (NK init writes it at `0x503109a8`) / the saved-r29 slot on ongoing
  entry. Our table[0] redirect bypasses this prologue entirely.
- Our fixup trampoline supplies r29 = `0x5046e000` directly — the defect.

**What is at each candidate base:**

| Address (static) | Content (verified in dump) |
|---|---|
| `0x380000–0x400000` | **the opcode table, fully populated** — 131072/131072 nonzero words; entries like `0x380000`: `7d04db78 4bfe0bfc` (`mr; b 0x360c00`); op `0x4efa` slot `0x3a77d0`: `7f78daee 4bfc0490` (→ emulator body `0x367c64`); op `0x7100` (EMUL_OP) slot `0x3b8800` present. Relative branches survive the mirror copy → the **mirror table at `0x480000–0x500000` branches into the `0x46xxxx` emulator**: the two worlds are each self-consistent. |
| `0x46e000` region (≙ static `0x36e000`) | NOT a dispatch table: the 0x97-entry helper page, the entry **vector** table (`0x36e8c0`: `b 0x36f900 / b 0x36fa00 / …`), the real cold prologue `0x36e900`, the cold-start body `0x36e964`. r29=`0x5046e000` ⇒ base `0x50400000` ⇒ handlers land in the **mirror NK/emulator code**, not in any table. |
| `0x5036f900` (and mirror `0x5046f900`) | post-PatchROM this is **SheepShaver's patched emulator entry** (`patch_68k_emul`): increments `[0x2818]` (XLM_IRQ_NEST), saves r7–r13 into the ECB via `[KDP+0x65c]`, `mtlr [KDP+0x5f0]=0x50366080` (EMUL_RETURN), `blr`. Never compiled in any boot [PROBE✓ via region 0x50360000] — the classic interception route is currently dead on newworld. |

Static landing prediction for the observed defect: first opcode `0x4efa` →
`0x50400000|0x277d0 = 0x504277d0` = a `bl` in the Thud console help-printer chain (static
`0x503277d0`, string "id idvalue — Obtain opaque ID info"); printing drains via the M1 SCC
Tx-ready poll and falls into the prompt-wait loop — one *candidate* explanation for how the
death lands in the console cluster with so few new blocks. **Treat as prediction, not
fact** — [PROBE-1] chases the actual death from `0x5046e974`'s bctr arithmetic.

### 2.4 Scheduler / run-queue anatomy (for M3b/M6 reference)

The brief's Q1 structures, static-verified (all KDP-relative, r1=KDP inside the NK):

| Structure | Location | Evidence |
|---|---|---|
| Ready-mask (32 prio bits, `cntlzw`→index) | `[KDP−0x970]` | readers `0x50324334/0x503243f8/0x50323ea8/ef8`; writers `0x503237e8/0x50323ebc/0x50323f0c/0x50324418` |
| Priority queues (0x20 B/level: +0 mask bit, +8/+0xC dlist, +0x10 count, +0x14 weight) | `KDP−0x9f0` | `0x50324348`, `0x50323f08` |
| Blocked/re-queue list | `KDP−0xa34` | `0x5032443c` |
| Current task ptr | `[KDP−8]` | `0x503242e8` |
| Reschedule-needed byte | `[KDP−0x118]` | gate `0x503242a8`; set by unready `0x50323ed0` |
| Task block | node−8; +0x18 state(2=running), +0x19 prio, +0x64 flags | `0x50324394–438` |
| Enqueue (make-ready) | `0x50323ed8` (tail) / `0x50323ee0` (head) | `0x50323ee4–f3c` |
| Scheduler entry | `0x503242a8` ← `b` from exception-return tail `0x50312cc8` | branch scan |
| Dispatch/restore tail | `0x503244cc`: `mtctr [r6+0xfc]`, `b 0x50318000` → **`[0x2818]`−−** → restore r0/r7–r13 from r6 | disasm |

**The boot process is not a run-queue task** — it is dispatched directly (registers + KDP
fields). The run queue being empty at the console is *normal*. (Cold-init does make two
early `bl 0x50323ee0` enqueues at `0x50311e90/0x50311f9c` — unidentified NK-internal tasks;
side detail.) `check_work`'s −1 = "no SCC Rx char" (S3 §2.5); the idle arm loops on it; the
console is the char-arrived branch — all designed behavior once the boot process is dead.
The `[0x2818]` inc/dec pair (patched entry `0x5036f904` / restore `0x50318000`) is the
NK↔emulator interrupt-nest protocol and explains M3A's `XLM_IRQ_NEST=0xFFFFFFFF` (one
restore, zero entries — attribution [PROBE-4]).

---

## 3. Environment-needs table — what the handoff reads vs what we seed

| Resource | Consumer | Our seed | Verdict |
|---|---|---|---|
| ConfigInfo @ROM+0x30d000 | cold-init `0x503107e0–0x50310a00` | `patch_nanokernel_boot` :792ff | ✅ correct; **single source of truth — everything else should derive from it** |
| r3=ConfigInfo, r4=KDP+0x1000 at NK entry | cold-init prologue | `init_emul_ppc` GPR(3)/(4) | ✅ |
| **r29 at DR cold-start** (opcode-table base) | dispatcher `0x5036e9dc` | fixup trampoline `0x5046e000` (rom_patches :741) | ✗ **[PROBE✓] the smoking gun** — must be `0x50480000` (`LA_DispatchTable`) |
| `[KDP+0x648]` (vector table) | dispatch tail `0x503126cc`, blrl `0x503126ec` | glue `0x50380000` (labeled "opcode dispatch table") | ⚠ live=`0x50380000` [PROBE✓]; mislabeled, wrong-world, and unconsumed on the live route — reconcile or drop |
| `[KDP+0x5a4]` (code base) | dispatch tail (never ran [PROBE✓]) | glue `0x5036d218` (→ entry `0x5036f900`) | ❓ dormant; primary-world; route-dependent |
| `[KDP+0x5a0]`, `KDP−0x964` UserModeMSR, SPRG0, `[KDP−0x20]`, `[KDP−0x900]` SCC | various (architectural writers in the **skipped** `0x31000c–0x3101af` block) | glue seeds | ✅ (SCC base live-verified Wave-0) |
| `[KDP+0x65c]` ECB, `+0x5f0/5f4` EMUL_RETURN | patched entry `0x5036f920/f950` | glue `0x68fff000` / `0x50366080` | ✅ live (M3A) / dormant with the entry |
| ECB ctx+0xfc (scheduler resume), ctx+0x1ec (saved r29) | restore `0x5032448c` / ongoing entry | glue `0x5036f900` / `0x50380000` | ✗ primary-world constants in a mirror-world boot — rung-1 targets |
| guest[0]/[4], r24/r30/r31 at cold start | `0x5036e9b4`, `0x5036dbe8`, table build `0x5036dc0c` | trampoline `0x00100000`/`0x5000002a`/`0x50460000`/`0x68fff000` | ✅ all [PROBE✓]; the cold start rebuilds the 0x97-entry helper table from `r30\|hw`, so glue's `page_base=0x5036e000` version is dead code |
| Staged NK SCC code | `check_work` family | **mutated by 5 `io_poll_beq` NOPs** (§5.4) | ⚠ contradicts the M1 SCC model — retirement candidates |
| `patch_68k` HLE shims | 68k boot path | effectively **zero installed** on 9.0.1 (lenient skips; only the OldWorld-style `run_diags` BRA.L applied) | ✗ the named next wall (§5.5) |

---

## 4. Item 1 — reconciling the dispatch-table contract

The fix must collapse three values to one world. Two coherent options:

- **Mirror world (recommended — it is what ConfigInfo already declares):**
  `r29 := 0x50480000` at the fixup trampoline; glue `ctx+0x1ec := ROMBase+0x480000`;
  glue `ctx+0xfc := ROMBase+0x46f900`; `[KDP+0x648]` seed → `ROMBase+0x46e8c0` (or drop —
  the NK writer produces it when its block runs). Everything then matches
  `LA_DispatchTable/LA_EmulatorCode/LA_EmulatorData`, and the mirror table's relative
  branches (verified) keep execution inside the `0x46xxxx` emulator.
- **Primary world:** point everything at `0x380000/0x36xxxx` instead — but ConfigInfo, the
  fixup trampoline targets, table[0]'s observed execution, and the staged-NK reality are
  all already mirror-side; converting them is strictly more edits for no benefit.

Verification arithmetic for the fixed dispatch: op `0x4efa` → `0x50480000+0x277d0 =
0x504a77d0` → mirror slot of `0x3a77d0` (`7f78daee 4bfc0490`) → emulator body `0x467c64`.
Eight-byte slots exist for every opcode incl. `0x7100` EMUL_OP (`0x3b8800`), so the first
instructions of the 68k reset path (`jmp`, `move.w #$2700,SR` at `0x500000b6`, the `bsr.l`
diagnostics chain) all have real handlers. Success telemetry: first-compiles in regions
`0x50480000`/`0x50460000`+, `jDR` unfreezing past 8, `comp` past 781.

---

## 5. Items 2–5 — the rest of the M6a surface

### 5.1 (item 2) Ongoing-entry support is TODO

`PatchROM_NW_trampoline` is explicitly diagnostic: *"Diagnostic mode: always cold-starts
(re-inits handler table every context-switch). Production ongoing-entry support is TODO"*
(rom_patches.cpp:~706–712). The cold start works once; every subsequent NK→emulator
transition (interrupt return, context switch) should take the **ongoing** entry
(`table[0] → 0x46f900`), which is a full register restore from the ECB. Today table[0] is
redirected to the always-cold trampoline, so the first real interrupt round-trip after the
rung-1 fix will re-cold-start the emulator and destroy 68k state. **The re-entry contract is
the second half of M6a:** either restore table[0]'s `b +0x1040` once cold-start has run
(self-disarming patch), or implement the ECB save/restore convention the scheduler's
`[r6+0xfc]`/`+0x13c..0x16c` slots define (§2.4) so ongoing entry restores correctly. Note
the patched-entry/restore `[0x2818]` protocol must balance across this path.

### 5.2 (item 3) The dropped `mtsrr1` — an EE-transition fidelity bug in waiting

The jump68k redirect replaces `mtsrr0;mtsrr1;rfi` with `mtctr;bctr;nop`, discarding
`r9 = UserModeMSR = 0x0000D032` — i.e. the architectural transition to **EE=1, PR=1**
user mode. Pre-M3a this was harmless (MSR was fiction). Post-M3a, delivery honors EE: the
boot MSR is seeded `0x7072` (EE=0) and *"the OS enables interrupts when ready"* — and this
dropped write **is** one of the places the OS enables interrupts. The block never executes
today [PROBE✓], but on any route that reaches it (or any equivalent transition we
synthesize), the EE-edge must be applied (`msr := 0xD032`-family) or the M3a deferral
machinery will hold interrupts forever in the 68k world. Same applies to the fixup
trampoline path, which currently performs *no* MSR transition at all: post-fix, the 68k
world runs with the NK's EE=0 MSR unless M6a sets it. **Concrete task:** wherever M6a's
final handoff lives, write the user-MSR (from `[KDP−0x964]`) into the stored MSR so the
EE-edge re-raise (M3a Task 3) fires.

### 5.3 (item 4) The five `io_poll_beq` NOPs mutate the staged NK's SCC code

rom_patches (:1310-region) NOPs `beq $-0xC` at `0x326504, 0x3266f4, 0x326864, 0x326968,
0x326b60` — all inside the S3 §2 SCC code (`check_work` poll/Tx/init/BAT-helper cluster),
and the memcpy propagates them into the staged copy the live kernel actually runs. These
are Path-A "VIA/CUDA ready-wait" artifacts from the fake-page era; under M1 the SCC model
answers honestly and these waits would terminate by themselves (Tx-empty/All-Sent return 1
per the model). Removing them is required for the fidelity profile's "retire serial-skip
hacks" DoD and to keep the SCC conformance vectors meaningful — but **do it as its own
gated change with an A/B boot**, since the NOPs currently also neutralize loops in paths we
have not re-validated under the bus (`0x326b60` is in the BAT helper, not the SCC). Cost S;
risk: a reintroduced honest wait that the M2 clock must satisfy (DEC-based drain loop,
S3 §2.4 — already ticking post-M2).

### 5.4 (item 5) `patch_68k` = zero shims on 9.0.1 — the named next wall

Even with the dispatch fixed, the 68k boot path immediately needs the HLE shims that
OldWorld boots rely on: the live log shows every 9.0.1 `patch_68k` pattern skipped
(`nvram2–6`, `timek`, `powermac_id`, …; only the OldWorld-style `run_diags` BRA.L applied),
i.e. **effectively zero of the 84 shims are installed** — "missing 68k HLE shims are the
expected next wall" (rom_patches :658). Scope per `PATCH-68K-SHIM-INVENTORY.md` + HANDOFF
§2.8 Phase 2: **boot-path-first** — NVRAM read/write cluster, VIA init, reset vector,
run-diags — not all 84 up front (28 in-range / 31 relocated-needs-verify / 25 absent on
9.0.1). Expect M (days, incremental), driven by where the post-fix boot actually stalls;
each stall names its shim.

### 5.5 Telemetry follow-up (probe infrastructure)

Two confirmed gaps to fix before the next forensics session: (a) **clean-exit MMIO stats
never appear** — SIGTERM/alarm kills skip the atexit dump (verified); add the
region/bus counters to the periodic heartbeat line instead. (b) **chained blocks are
probe-invisible** — either probe with `SS_JIT_NO_CHAIN=1` for attribution runs (slower but
honest) or add a probe-aware chain break at probed PCs.

---

## 6. Rung ladder (M6a) — cheapest to most designed

| Rung | Work | Cost | Risk |
|---|---|---|---|
| **0** | Probe pack §7 (death chase, table[0] caller, `[0x2818]` attribution) — no rebuild | S (one sanctioned session) | none; decides rung-1 sufficiency |
| **1** | **Dispatch-contract repair** (§4): trampoline `r29 := 0x50480000`; glue ctx+0x1ec/+0xfc and `[KDP+0x648]` seeds to mirror values; + the user-MSR write at the handoff (§5.2) | S (hours + clean retest: `make test-jit`=100, OldWorld 8.6 boot green — all edits newworld-gated) | If [PROBE-2] shows the route into table[0] is itself an accident, the constants fix the landing but not the route |
| **2** | **Ongoing-entry contract** (§5.1): self-disarming cold-start redirect or ECB save/restore-conformant re-entry; balance the `[0x2818]` protocol | M (1–3 d) | First interrupt round-trip is the test; needs M3a delivery (already landed) |
| **3** | **Path-A artifact retirement** (§5.3 io_poll NOPs) + ConfigInfo-derived trampoline registers (no duplicated literals; one `lwz` chain via SPRG0) | S–M | re-exposed honest waits must be satisfied by M1/M2 models (designed to be) |
| **4** | **Boot-path shim wave** (§5.4) per inventory, stall-driven | M (days, incremental) | the long tail; per-pattern grind, low general-bug ROI |
| **5** | Full §2e synthesized-Trampoline component (ConfigInfo-complete object, both entries, kernel-call channel/M3b `syscall_entry`) | L (1–2 w) | **not needed for this wall**; right shape after rungs 1–4 inform it |

**Sequencing notes:** M6a-before-M3b stands — the handoff and its repair are direct
branches; no syscall/interrupt delivery is on the critical path to un-killing the 68k boot
process. M3b becomes load-bearing at rung 2+ (interrupt round-trips) and during the shim
wave (Ticks/VBL expectations). M3A's "plausibly the NK's designed end state" line should be
annotated: the console *is* designed, but it is reached because the boot process died at the
first opcode dispatch — not because a diskless boot ends there.

---

## 7. Unknowns remaining — probes for the next sanctioned session

All newworld diagnostic boots (`/tmp/m2accept.prefs`-style, `SS_ROM_LENIENT=1`, no
`SS_ROM_SKIP_JUMP68K`, perl-alarm ~25 s). Attribution runs add `SS_JIT_NO_CHAIN=1` (§5.5b).

**[PROBE-1] Chase the DR death block-by-block from the bctr arithmetic.** From the verified
r29=`0x5046e000` and r24=`0x5000002a`: first fetch is op `0x4efa` ⇒ predicted bctr target
`0x504277d0`; the 68k `jmp` then can't be taken correctly, so the next blocks are whatever
that code does. Probe the prediction and its first successors:
```bash
SS_JIT_NO_CHAIN=1 SS_PROBE_PC='0x5046e964:r29,r30,r31;0x504277d0;0x50427808;0x5042751c;0x50426884:[0x68ffd690],[0x68ffdff8],[0x68ffdee8],[0x2818]' \
  SS_ROM_LENIENT=1 ./SheepShaver --config /tmp/m2accept.prefs
```
Full dumps at `0x504277d0` (expect r27=`0x4efa`, CTR=`0x504277d0` if the prediction holds;
zero visits kills it and the trace ring / `SS_LOG_FIRST_BLOCKS` takes over). The `0x50426884`
fields give ready-mask `[KDP−0x970]`, current task `[KDP−8]`, resched byte, and
`XLM_IRQ_NEST` at the spin in one shot.

**[PROBE-2] Who branches to table[0]?** With chaining off, full-dump probe at `0x5046e8c0`:
LR/CTR identify the caller (dispatch-tail bctr ⇒ CTR=`0x5046e8c0`; the `blrl` variant ⇒
LR≈`0x503126f4`; glue/wander ⇒ neither). Also re-probe `0x503126b4` (the "NOT replaced"
print entry) — distinguishes "tail skipped" from "whole exit path skipped".

**[PROBE-3] Post-rung-1 regression run.** Repeat PROBE-1's command after the constant fix:
success = `0x504277d0` visits 0, first-compiles in region `0x50480000`, `jDR` > 8 and
growing, `comp` unfrozen; then capture where the 68k boot path stalls next (names the first
shim for rung 4).

**[PROBE-4] `XLM_IRQ_NEST` attribution.** `SS_JIT_WATCH_ADDR=10264` (=0x2818) — which PC
performs the single decrement to −1, and does the scheduler restore tail
(`0x503244cc`/`0x504244cc`) ever run.

**[PROBE-5] MMIO/region counters.** Blocked on §5.5a (heartbeat counters) — add before the
session; the atexit dump provably never fires under alarm-kill.

---

## 8. Honest residue

Four things remain unproven. (1) **The exact death trajectory** — r29's bad base is
probe-verified and the landing arithmetic is mechanical, but the specific block where the
68k world "dies in <8 blocks" and how execution threads from there into the designed
idle/console arm is reconstructed, not observed; PROBE-1 closes it. (2) **The route into
table[0]** — the designed dispatch tail never executes [PROBE✓], so something else branches
there; the rung-1 fix repairs the landing regardless, but rung 2's re-entry design needs the
true route (PROBE-2). (3) **Rung-1 sufficiency** — with a correct table the first opcodes
have real handlers, but `move.w #$2700,SR` and the diagnostics `bsr.l` chain immediately
test privileged-op handling, the EE transition (§5.2), and the empty shim surface (§5.4);
the wall may move only thousands of instructions — that is the forcing function working,
not the fix failing. (4) **Side details** — the two cold-init enqueues
(`0x50311e90/0x50311f9c`), the single `[0x2818]` decrement, and the `[KDP+0x648]`
writer-skip interaction are attributed only circumstantially. The mechanism core — the
dispatch-table geometry, the three-value contract conflict, the probe-verified r29, the
designed-console identification, and the scheduler anatomy — is as solid as static+probe
analysis gets without the rung-1 boot.

---

## Wave 1 results (2026-06-11, plan `2026-06-10-m6a-wave1-dr-dispatch.md`)

**Rung 1 verdict: the dispatch repair WORKS — the 68k world runs.**

- **Boot A (MSR off, the stall capture):** zero visits at the console help-text printer
  `0x504277d0` (was: every boot); first-compiles across the ENTIRE mirror handler table —
  regions 0x50480000/0x50490000/0x504a0000 (the 0x4efa slot itself at pc=0x504a77d0
  executed as code, correctly)/0x504b/0x504d/0x504e/0x504f — real 68k opcodes executing
  through the mirror emulator within 0.01s. **The next wall, captured:** SIGSEGV at guest
  `pc=0x50466ee0` (mirror emulator internals), `ea=0xffffaad0`, with `r1=0x25f8` (a live
  68k stack) — the 68k boot code dereferences garbage, the signature of the **patch_68k
  shim desert** (zero shims installed on 9.0.1): Wave 2's first target. Crash telemetry:
  `mtspr_dec=4, dec_expiries=1, pending=1 (EE never rose, correctly deferred)`, SCC
  counters 0 (the boot died long before the console path — consistent).
- **Boot B (`SS_M6A_USER_MSR=1`, the A/B recon):** dies fast at guest pc=0x50429b40 (the
  fixup trampoline) with no successful deliveries — the red-team rev-2 finding-1
  prediction verbatim: EE=1 routes the pending DEC through the unverified
  interrupt-save/scheduler-restore/always-cold-table[0] plumbing. The gate stays
  default-OFF; the ongoing-entry contract (rung 2) owns this.

Wave 2 scope confirmed: (1) identify + port the boot-path shim(s) behind pc=0x50466ee0 /
ea=0xffffaad0 (PATCH-68K-SHIM-INVENTORY + HANDOFF obstacle map); (2) the ongoing-entry
contract (un-blocks SS_M6A_USER_MSR + live interrupts in the 68k world); (3) io_poll NOP
retirement. The heartbeat now carries `mmio=S:N/V:N` (PROBE-5 unblocked).
