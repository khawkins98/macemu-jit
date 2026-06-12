# VIA-IFR Surface — Recon & QEMU Rig Findings

> **Status:** Task-0 complete (2026-06-12). AGENT-CONTEXT description corrected.
> Tools: `SheepShaver/tools/qemu-rig.sh` + `qemu-mon.py`.
> Next: Task A (what sets $d94 / what $6e4 chain expects).

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

## 5. QEMU rig operational notes

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
