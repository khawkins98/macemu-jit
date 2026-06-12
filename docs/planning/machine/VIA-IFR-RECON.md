# VIA-IFR Surface — Recon & QEMU Rig Findings

> **Status:** Task-0 and Task A both complete (2026-06-12 sessions 1–2).
> Tools: `SheepShaver/tools/qemu-rig.sh` + `qemu-mon.py`; `ss-slot-boot.sh` + `SS_PROBE_PC`.
> Fix target identified: ROM offset 0xed08. See §5 for session-2 findings, corrections,
> and implementation plan. Next: implement `SS_NW_VIA_IFR` gate.

---

## 1. Context — what the milestone needs

**Leg 8 of M8 slot-4 consumption is RED:** the 68k level-1 handler runs at 60 Hz but
`rte`s source-less. The handler's source-dispatch path finds no source bit and skips
the `$6e4` VBL/tick chain. Result: Ticks is never guest-claimed.

From `AGENT-CONTEXT.md` frontier block (pre-recon):
> the handler's source dispatch (`btst d6,(a4); beq; movea.l $6e4.w,a0; jsr (a0)`
> @0x5000ee9a) finds NO VIA IFR source bit in the via6522 model

**Task-0 questions:** what address is in a4 at 0x5000ee9a? what bit in d6? what does
the $6e4 chain expect on dismissal?

---

## 2. QEMU rig bringup (2026-06-12)

Built from Spike S1's working mac99 boot (9.0.1 ROM in 9.2.1 CD, QEMU 11.0.1).

Tools: `SheepShaver/tools/qemu-rig.sh` (boot + monitor), `qemu-mon.py` (ANSI-clean
monitor client with `--disasm` support). The monitor socket uses terminal-mode echo
(ANSI VT100 escape sequences in output); `qemu-mon.py` strips them before parsing.

**Key rig findings (all from the 9.0.1 ROM + Mac OS 9.2.1 reference boot):**

| Finding | Value | Notes |
|---|---|---|
| MacIO PCI BAR0 (QEMU) | `0x80000000` | NOT `0xF3000000` — QEMU uses PCI-assigned address; real hardware is hardwired. Behavioral oracle is valid; address oracle is NOT. |
| VIA (Cuda) base (QEMU) | `0x80016000` | MacIO+0x16000 |
| SCC (ESCC) base (QEMU) | `0x80012000` | MacIO+0x12000 |
| 60 Hz tick source | `ppc_irq_set pin 5 level 1` | OpenPIC external interrupt. Cuda's internal timer asserts without a Cuda packet. |
| VIA MMIO direct reads | **None detected (QEMU)** | Hardware watchpoints on 0x80016000-0x80018000 never fired during interrupt handling. QEMU mac99's Cuda device model handles the tick internally and deasserts the interrupt line without exposing IFR bits to the guest CPU. **Caveat:** this is a property of QEMU's model, not necessarily real hardware — on real G4 hardware the Cuda device asserts VIA IFR bit 3 (Cuda IRQ), which Mac OS's Cuda driver may read. In our SheepShaver machine layer the `dev_via6522` model DOES present an IFR; the finding means "QEMU's Cuda model never requires the guest to read the IFR" but does not rule out that Mac OS's Cuda driver reads it during interrupt handling on real HW. |
| 68k low memory mapping | Virtual address 0 is live | NK has MMU on but maps 68k low memory at virtual 0 (identity for the low region). `x` (virtual) reads succeed; `xp` (physical) returns zeros. |
| Level-1 interrupt vector (`x /2wx 0x64`) | `0x0047d0ba` | Mac OS 9.2.1 system-installed handler (RAM, not ROM). Installed during System file loading; not present at early boot. |
| Ticks at Finder (`x /2wx 0x168`) | non-zero, updating | Confirms 60 Hz tick is live and Ticks is incrementing in a working boot. |
| $d94 at Finder (`x /2wx 0xd90`) | `0x00000000` | Zero even at Finder steady state (see §3). |
| $6e4 at Finder (`x /2wx 0x6e0`) | `0x00000000` (word at 0x6e0); `0x00493dfe` (word at 0x6e4) | The `x /2wx 0x6e0` command returns two words: the word AT 0x6e0 then the word AT 0x6e4. The VBL chain pointer IS `$6e4` (the second word) = `0x00493dfe` — non-zero at Finder (see §4). The `0x00000000` in this column is 0x6e0's value, not 0x6e4's. |

---

## 3. AGENT-CONTEXT correction — the btst d6,(a4) claim is WRONG

**The claim:** `btst d6,(a4); beq; movea.l $6e4.w,a0; jsr (a0)` at `@0x5000ee9a`.

**Static ROM disassembly (9.0.1 ROM, capstone, from 0x5000ee98):**
```
0x5000ee98  4ab80d94    tst.l   $d94.w        ; ← 4-byte instruction
0x5000ee9c  6706        beq.b   $5000eea4     ; skip if $d94 == 0
0x5000ee9e  207806e4    movea.l $6e4.w, a0    ; load $6e4 handler
0x5000eea2  4e90        jsr     (a0)           ; call it
0x5000eea4  ...
```

**`0x5000ee9a` is the address of bytes 2-3 (`0d 94`) of `tst.l $d94.w` — mid-instruction.**
There is no `btst d6,(a4)` instruction in the ROM near this address. No `movea.l #$F3016xxx, a4`
exists anywhere in the ROM. The VIA IFR address is never hardcoded as a register load.

**The corrected description of the source-dispatch path:**

```
; Entry: ROM interrupt handler at 0x5000ee58
ori.w   #$700, sr                  ; mask all interrupts
movea.l $2b6.w, a1                 ; load interrupt manager base
tst.b   $2fa(a1)                   ; re-entrancy flag
bne.b   $5000eec0                  ; if set → cleanup + rte
...                                ; VBL/task queue checks via $26a(a1), $272(a1)
; --- at 0x5000ee98 ---
tst.l   $d94.w                     ; test pending-work flag in low memory
beq.b   $5000eea4                  ; if zero → skip $6e4 dispatch
movea.l $6e4.w, a0                 ; load $6e4 chain head
jsr     (a0)                       ; dispatch to registered handlers
; --- rte ---
rte                                ; at 0x5000eecc
```

The "VIA IFR source bit" issue is **NOT a direct MMIO read** — it is a LOW MEMORY FLAG
(`$0d94`) that is either zero (handler does nothing) or non-zero (handler dispatches via $6e4).

---

## 4. Task A findings — boot-stage ladder (2026-06-12 experiment)

Running the rig at 5s and 10s revealed when the handler transition happens and, more
importantly, what the ROM stub actually does.

### 4a. Handler timeline (from timeout ladder)

| Timeout | `0x64` | Stage |
|---|---|---|
| 5s | `0xffc0ec50` | ROM stub — interrupt dispatch table in ROM |
| 10s | `0x0047d0ba` | Mac OS system handler installed in RAM |
| 50s (CD boot) | `0x00000000` | **Unreliable** — installer CD reboots mid-run (see §5 limitations) |

The transition happens between 5s and 10s. Our SheepShaver guest at the PROGRAM#5
frontier is equivalent to the pre-5s stage — the ROM stub is almost certainly what's at
0x64 when our 60 Hz interrupts fire.

### 4b. The ROM stub — the actual early-boot source dispatch (NEW — contradicts prior understanding)

The ROM handler at `0xffc0ec50` (= `0x5000ec50` in ROMBase space) is a level-indexed
jump table that flows into a common handler. What the common handler does at `0xffc0ec7e`:

```
addq.l  #$1, ([$2b6], $31c)      ; increment interrupt counter via interrupt-mgr base
movea.l $68ffefd0.l, a2          ; load NK PIC descriptor block (KDP+0xFD0)
moveq   #$0, d1
move.l  $28(a2), d0              ; read interrupt pending bits
movea.l $14(a2), a0              ; read pointer to level-indexed source bit table
and.l   (a0, d3.l * 4), d0      ; mask by interrupt level (d3=1 for level-1)
bne.b   $ffc0ecaa                ; → source found, handle it
; --- no source found ---
moveq   #$20, d1
move.l  $2c(a2), d0              ; secondary pending check
and.l   $20(a0, d3.l), d0
beq.w   $ffc0ee58                ; → no source at all: fall to tst.l $d94.w path (rte's source-less)
```

**The mechanism:** the ROM handler checks an **NK PIC descriptor block at `0x68ffefd0`**
(= KDP+0xFD0, 0x30 bytes before ECB). This is the structure the ROM uses to identify
interrupt sources — NOT VIA MMIO, NOT `$d94` directly.

- `0x68ffefd0 + 0x28` (`0x68ffeff8`): **interrupt pending word** — which sources are pending
- `0x68ffefd0 + 0x14` (`0x68ffefe4`): **pointer to level-indexed source bit table**

If `pending & source_table[level]` is zero, the handler falls through to `0x5000ee58`
(the `tst.l $d94.w` path) and rte's source-less. **This is exactly our symptom.**

**What this means for our machine layer:** for the 68k handler to dispatch at tick time,
our PIC/`dev_via6522` layer needs to set the right bit in the NK PIC descriptor at
`0x68ffeff8` (and ensure the source table at `0x68ffefe4` has the matching bit for level
1). The `$d94` path is a dead-end fallback, not the fix target.

### 4c. The system handler's source-ID function — trivially simple

The system handler (`0x47d0ba`) calls `jsr $47c526(pc)` for source identification. That
function is two instructions:

```
move.l  $47c50c(pc), d0   ; load pointer from fixed address 0x47c50c
rts                        ; return it in d0
```

It just returns `*0x47c50c` — a pointer into the interrupt manager's data area (interrupt
manager base is at `$2b6` = `0x44be0`). The handler then tests the flag byte at that
pointer address to check for re-entrancy. There is no VIA MMIO read in this path either.

The address `0x47c50c` and the interrupt manager at `0x44be0` are heap-allocated per boot
and cannot be predicted in advance.

### 4d. Remaining Task A question

The ROM stub mechanism is now understood. The one open question before implementation:

**What initializes `0x68ffefd0` and when?** The NK must set up this descriptor block
(fields `+0x14` and `+0x28`) before the 68k world starts receiving interrupts. If our
machine layer writes the correct pending bit to `0x68ffeff8` at tick time but the source
table at `0x68ffefe4` is zero (never initialized), the AND still comes out zero. Task A
is: confirm the NK initializes this structure before PROGRAM#5, and identify what
value `0x68ffefe4` holds in a working boot.
→ Use `SS_PROBE_68K` on a slot boot to read `0x68ffefd0` through `0x68ffeffc` when the
first interrupt fires in our guest.

**Prior "two parallel questions" are now answered:**
- Q1 (what handler is at 0x64 in our early-boot guest): ROM stub at 0xffc0ec50 — confirmed
  by the 5s bracket
- Q2 (what does `jsr $47c526(pc)` return): `*0x47c50c` — trivial pointer load, confirmed
  by 50s disassembly

---

## 5. Session-2 findings — Task A complete (2026-06-12 probe campaign)

### 5a. Critical correction: Hnfo record base address

The QEMU RECON doc and AGENT-CONTEXT carried a structural error in the Hnfo record
addresses. `[KDP+0xfd0]` (= address 0x68ffefd0) is the **pointer field** — it contains
the 4-byte address of the Hnfo record, not the record itself. The 68k instruction
`movea.l $68ffefd0.l, a2` loads a2 with the VALUE at that address:

```
[KDP+0xfd0] = 0x68ffefd0 → VALUE = 0x68ff4f00  ← this is hnfo_rec (= irp_base + 0xf00)
```

All Hnfo field offsets are relative to **hnfo_rec = 0x68ff4f00**:

| Field | Address | Meaning |
|---|---|---|
| hnfo_rec + 0x08 | `0x68ff4f08` | Writable scratch pointer (set by trampoline) |
| hnfo_rec + 0x14 | `0x68ff4f14` | **Source table pointer** (NIL in our boot) |
| hnfo_rec + 0x18 | `0x68ff4f18` | Re-entrancy check gate |
| hnfo_rec + 0x28 | `0x68ff4f28` | **Pending interrupt bits** (0x80000000 in our boot) |
| hnfo_rec + 0x2c | `0x68ff4f2c` | Secondary pending bits |
| hnfo_rec + 0x70 | `0x68ff4f70` | `'Hnfo'` tag (set by trampoline) |
| hnfo_rec + 0x76 | `0x68ff4f76` | Machine id word (set by trampoline) |
| hnfo_rec + 0xa8 | `0x68ff4fa8` | Source-device table pointer (NIL in our boot) |

The AGENT-CONTEXT claims `pending=*(0x68ffeff8)` and source table at `*(0x68ffefe4)` are
WRONG — those are KDP+0xfd0 plus offset arithmetic against the POINTER FIELD, not the
record. The correct addresses are 0x68ff4f28 and 0x68ff4f14.

### 5b. *(0x64) diverges from the QEMU reference

| Boot | *(0x64) level-1 vector | Path |
|---|---|---|
| QEMU 9.2.1 (5s bracket) | `0x5000ec50` | **Primary dispatch table** — `jmp $5000ef20.l` → VIA device handler |
| Our SheepShaver 9.0.1 | `0x5000ed08` | **Secondary dispatch table** — NK PIC descriptor check |

These are different code paths with different mechanisms. The RECON doc's analysis of
"what the ROM stub does at 0x5000ec50" (§4b) does NOT apply to our boot — that code is
never reached. The active path in our boot is the secondary table at 0x5000ed08.

**Why the divergence:** the 9.0.1 ROM's 68k initialization code installs 0x5000ed08 at
*(0x64). QEMU boots Mac OS 9.2.1 which installs 0x5000ec50. Different ROM versions; the
QEMU rig is a behavioral oracle for the same ROM version only.

### 5c. Probe results — Hnfo record state at first EXT interrupt

All values captured at `SS_PROBE_PC=0x50314880` (NK EXT handler entry, before NK has
executed any instructions of this invocation):

| Field | Address | Value | Meaning |
|---|---|---|---|
| `*(0x64)` | — | `0x5000ed08` | Level-1 vector → secondary dispatch path |
| `*(0x0192)` | — | `0x50008180` | VIA device handler pointer (unreachable via our path) |
| `*(0x0000)` | — | `0x50010000` | Initial SSP reset vector |
| `*(0x0004)` | — | `0x50010000` | Initial PC reset vector (byte at [4] = 0x50) |
| hnfo_rec | `0x68ff4f00` | `0x00000000` | Record first word (tag is at +0x70) |
| hnfo_rec+0x14 | `0x68ff4f14` | **`0x00000000`** | **Source table pointer = NIL ← THE BUG** |
| hnfo_rec+0x18 | `0x68ff4f18` | `0x00000000` | Re-entrancy gate (zero → bypassed) |
| hnfo_rec+0x28 | `0x68ff4f28` | **`0x80000000`** | **Pending bits (bit 31 set by NK/init)** |
| hnfo_rec+0xa8 | `0x68ff4fa8` | `0x00000000` | Source-device table pointer = NIL |

### 5d. Source-dispatch trace through the secondary path

At entry to 0x5000ed08:
```
movem.l d0-d3/a0-a3, -(a7)
moveq   #$1, d3              ; level = 1
bra.b   $5000ed36            ; common handler
```

At 0x5000ed36 (common handler for secondary dispatch table):
```
addq.l  #$1, ([$2b6], $31c)  ; increment interrupt counter
movea.l $68ffefd0.l, a2      ; a2 = *(0x68ffefd0) = hnfo_rec = 0x68ff4f00
moveq   #$0, d1
move.l  $28(a2), d0          ; d0 = hnfo_rec+0x28 = 0x80000000 ✓ (pending bit set)
movea.l $14(a2), a0          ; a0 = hnfo_rec+0x14 = 0x00000000 ← NIL!
and.l   (a0, d3.l * 4), d0  ; d0 &= *(0 + 1*4) = *(4) = 0x50010000 as a word
                              ; d0 = 0x80000000 & [whatever is at addr 4] → unpredictable
bne.b   $5000ed62            ; source found? depends on *(4)
moveq   #$20, d1
move.l  $2c(a2), d0          ; secondary pending (hnfo_rec+0x2c) = 0
and.l   $20(a0, d3.l), d0   ; *(0 + 0x21) — another garbage read
beq.w   $5000980a            ; if both zero → hardware reset path
```

**The primary AND reads garbage from address 4 (byte 0x50) ANDed with 0x80000000 = 0.**
The secondary AND also reads garbage. Both land at 0x5000980a — the hardware reset path.
Yet the machine keeps running (observed). The probable explanation: a0=0 causes the `and.l`
to read from 68k address 4 as a full 32-bit word = 0x50010000; AND with 0x80000000 = 0
(bit 31 of 0x50010000 is clear). Both checks fail → the handler falls to 0x5000980a.

The code at 0x5000980a is a PPC-timing hardware reset sequence (or equivalent) — its exact
behavior in our emulation may differ from real hardware (it may just loop or effectively
rte). This branch explains why the machine doesn't visibly reset, and also why $d94 is
never set and the $6e4 chain is never dispatched: the handler never reaches the interrupt
source-ID path.

### 5e. Fix target

The fix-target is **not** the source table + pending fields (those are correct in concept
but the source-found path requires five more uninitialized Hnfo fields and a live Mac OS
interrupt manager table — too many unknowns for early boot).

**The correct fix for our boot stage: patch ROM offset 0xed08 to install OP_IRQ.**

The via_int3_dat pattern `{0x48, 0xe7, 0xf0, 0xf0, 0x76, 0x01, 0x60, 0x26}` is confirmed
present at ROM offset 0xed08 (= guest address 0x5000ed08). This is exactly the 8-byte
secondary dispatch entry for level-1. The existing `via_int3` patch in `rom_patches.cpp`
searched range 0x15000–0x19000 (miss) and was conditional on `via_int` also finding a
match (which it doesn't in the 9.0.1 ROM). Both conditions block the patch.

**Proposed patch (rom_patches.cpp, gated by `SS_NW_VIA_IFR`):**
```cpp
static const uint8 via_nw901_int_dat[] = {0x48,0xe7,0xf0,0xf0, 0x76,0x01,0x60,0x26};
base = find_rom_data(0xed00, 0xee00, via_nw901_int_dat, sizeof(via_nw901_int_dat));
if (base) {
    wp = (uint16 *)(ROMBaseHost + base);
    *wp++ = htons(M68K_EMUL_OP_IRQ);  // replaces movem.l
    *wp++ = htons(M68K_RTE);          // 0x4e73 — replaces moveq
    *wp++ = htons(M68K_NOP);          // padding (replaces bra.b)
    *wp++ = htons(M68K_NOP);          // padding (replaces bra.b operand word)
}
```

This replaces the 8-byte level-1 secondary dispatch entry with `OP_IRQ; rte; nop; nop`,
leaving the level-2 entry at 0x5000ed10 untouched. When the level-1 interrupt fires:
the 68k handler runs OP_IRQ (which in early boot sets d[0]=1 and returns; after Mac OS
loads it dispatches TimerInterrupt, VBL, etc.) then rte.

### 5f. Implementation prerequisite: KernelDataAddr+0x67c safety

`OP_IRQ` (emul_op.cpp line 812) always executes:
```cpp
WriteMacInt16(ReadMacInt32(KernelDataAddr + 0x67c), 0);
```
`KernelDataAddr = 0x68ffe000`; `KernelDataAddr+0x67c = 0x68ffe67c`. In the paravirtual
profile this holds a valid pointer; in our NewWorld early boot it is zero (pre-zeroed
pool) → `WriteMacInt16(0, 0)` → writes word 0 to 68k address 0x0000 (Initial SSP),
corrupting the exception vector table.

**Fix:** in the NewWorld trampoline init (sheepshaver_glue.cpp, near the hnfo_rec setup),
write a scratch address into this field before interrupts fire:
```cpp
// OP_IRQ unconditionally does WriteMacInt16(ReadMacInt32(KernelDataAddr+0x67c), 0).
// In early boot this field is zero → write to addr 0 → corrupts reset vector.
// Point it at hnfo_scratch+0xf8 (the last 8 bytes of the scratch reserve) — safely writable.
WriteMacInt32(KernelDataAddr + 0x67c, hnfo_scratch + 0xf8);
```

Gate this write inside the same `SS_NW_VIA_IFR` guard as the ROM patch.

### 5g. Tooling lesson

The wrong tool was used throughout this investigation. **SS_PROBE_PC fires at PPC
block-entry addresses and is probe-blind to 68k code.** All the address-deduction and
dispatch-path analysis could have been replaced by one probe:
```bash
SS_NW_IRQ_CONSUME=1 SS_NW_PIC=1 \
  SS_PROBE_68K=0x5000ed08:5 \
  SheepShaver/tools/ss-slot-boot.sh --label via-ifr-68k --timeout 20
```
That would have shown d3=1, a2=hnfo_rec, *(a2+0x14)=0 directly at the interrupt handler
entry, without any QEMU-vs-SheepShaver mapping confusion or address arithmetic.

**Rule for the next session:** when investigating a 68k code path, start with
`SS_PROBE_68K=0xPC:N` (fires at DR dispatch hook, first N matches, linear, edge-triggered).
`SS_PROBE_PC` is for PPC-world investigation only.

---

## 6. QEMU rig operational notes

### Boot the reference machine
```bash
cd SheepShaver
SheepShaver/tools/qemu-rig.sh --timeout 50
# → /tmp/qemu-rig-YYYYMMDD-HHMMSS/
```

### Query virtual memory after boot
```bash
RUNDIR=/tmp/qemu-rig-YYYYMMDD-HHMMSS
python3 SheepShaver/tools/qemu-mon.py --sock $RUNDIR/mon.sock "x /2wx 0x64"
python3 SheepShaver/tools/qemu-mon.py --sock $RUNDIR/mon.sock "x /2wx 0x168"
```

### Disassemble 68k code from guest virtual memory
```bash
# Always read the vector first — handler address varies per boot
VEC=$(python3 SheepShaver/tools/qemu-mon.py --sock $RUNDIR/mon.sock "x /1wx 0x64" \
      | grep -o '0x[0-9a-f]*' | head -1)
python3 SheepShaver/tools/qemu-mon.py --sock $RUNDIR/mon.sock \
    --disasm "$VEC" --count 128
```

### Capture device-level trace (slower boot)
```bash
# Create trace-events file first:
cat > /tmp/via_trace.txt << 'EOF'
macio_timer_write
macio_timer_read
cuda_receive_packet_cmd
ppc_decr_excp
ppc_irq_set
ppc_irq_set_state
EOF
SheepShaver/tools/qemu-rig.sh --trace /tmp/via_trace.txt --timeout 60
# Trace output: $RUNDIR/trace.log
```

### Known patterns in trace output (from 2026-06-12 run)
- `ppc_irq_set env [...] pin 5 level 1` → external interrupt asserted (60 Hz + Cuda events)
- `ppc_irq_set env [...] pin 5 level 0` → deasserted immediately (edge-triggered in QEMU)
- `Raise exception at 0xNNN => EXTERNAL (4)` → PPC takes the exception; 0xNNN = PPC PC (NK code)
- `ppc_decr_excp raise decrementer` → DEC interrupt (separate from 60 Hz tick)
- `macio_timer_read read addr 0x38 len 4` → MacIO timer counter (rare; not the 60 Hz source)
- `cuda_receive_packet_cmd handling command ...` → Cuda ADB/power protocol (not tick path)

### Limitations
- QEMU `x` requires the guest CPU to be halted (it reads from current PTE context).
  Avoid issuing commands during intensive guest activity — pause briefly if values look stale.
- MacIO device MMIO addresses (0x80016000 etc.) differ from SheepShaver's targets
  (0xF3016000). Use QEMU for BEHAVIORAL reference only, not address values.
- The gdbstub (`--gdbstub`) and the monitor share state; don't use both concurrently
  with Python clients — use a single lldb session OR the monitor, not both.
- **CD boot config is unreliable past ~30s.** The 9.2.1 installer CD causes a reboot
  partway through, making 50s+ probes inconsistent (observed: `0x64 = 0x00000000` at 50s
  in one run, `0x0047d0ba` in a prior run). For stable Finder-steady-state readings, the
  rig needs a hard-disk boot config (a pre-installed HD image), not the installer CD.
  Use the CD config only for early-boot observations (≤ 30s).
