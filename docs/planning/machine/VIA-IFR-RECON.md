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

## 4. The real question for VIA-IFR task A

Since $d94 is zero even in QEMU's fully-booted Mac OS 9.2.1 at Finder, the $d94/$6e4
dispatch path in the ROM is the BOOT-TIME STUB (installed by the ROM, used before Mac OS
installs its own handler). Mac OS 9.2.1 replaces it with its own handler at 0x47d0ba
(in RAM) which uses a different mechanism (`jsr $47c526(pc)` — a system-managed dispatch).

**Confirmed at Finder steady-state (from rig run, 2026-06-12):**

| Address | QEMU value | Meaning |
|---|---|---|
| `0x64` level-1 vector | `0x0047d0ba` | Mac OS system-installed handler (not ROM stub) |
| `0x168` Ticks | non-zero, updating | 60 Hz tick is live |
| `0x0d94` dispatch flag | `0x00000000` | Zero — ROM stub path never dispatches |
| `0x06e4` VBL chain | `0x00493dfe` | **Non-zero** — VBL chain IS set up at Finder |

The $6e4 chain being non-zero confirms that once Mac OS is running, the VBL dispatch
infrastructure exists and would work — the issue is the upstream path setting $d94.

**The real level-1 handler in a booted Mac OS 9.2.1 (`0x47d0ba`, from rig disassembly):**
```
0x0047d0ba  cmpi.w   #$64, $6(a7)     ; check: is this a level-1 interrupt?
0x0047d0c0  bne.b    $47d0b4           ; if not, rte
0x0047d0c2  movem.l  d0-d1/a0-a1,-(a7); save registers
0x0047d0c6  move.w   $10(a7), d0      ; get stacked SR
0x0047d0ca  andi.w   #$e700, d0       ; mask to supervisor/IPL bits
0x0047d0ce  bne.b    $47d0b0           ; if interrupted supervisor code, branch
0x0047d0d0  jsr      $47c526(pc)       ; identify interrupt source → returns pointer in d0
0x0047d0d4  movea.l  d0, a0
0x0047d0d6  tst.b    (a0)             ; test source flag
0x0047d0d8  bne.b    $47d0b0           ; if already-handling, branch
0x0047d0da  move.l   $12(a7), d0      ; save return address
0x0047d0de  move     usp, a0          ; get user sp
0x0047d0e0  move.l   d0, -(a0)        ; push return addr to user stack
0x0047d0e2  move     a0, usp          ; update usp
0x0047d0e4  move.l   $47d124(pc),$12(a7) ; patch stacked return PC → to handler body
0x0047d0ea  bra.b    $47d0b0           ; rte with patched return → jumps to handler
```
Key: source identification is via `jsr $47c526(pc)` (returns a pointer to a source-flag
byte), NOT via direct VIA IFR MMIO read. The handler memory address (0x47d0ba) is
**heap-allocated per boot** — different code may be at that address in different boots.

**Two parallel questions for Task A:**

1. **During our early-boot SheepShaver scenario (PROGRAM#5 frontier / 60 Hz interrupts
   reaching 68k world):** what is the level-1 interrupt vector at 0x64? Does it point to
   the ROM's stub at 0x5000ee58, the system-installed handler, or somewhere else?
   → Use `SS_PROBE_68K` or a slot boot with `SS_DR_R24_RING` to check 0x64 in our running guest.

2. **What does `jsr $47c526(pc)` call**, and what does it return as the interrupt source?
   Since this is heap code, the address 0x47c526 (relative to 0x47d0ba) also shifts per
   boot. In the reference boot: `$47c526(pc)` where `pc` after `4eba` = 0x47d0d2,
   offset 0xf454 (signed -2988) → target = 0x47d0d2 - 0x0BAC = **0x47c526**.
   This function returns a pointer to a source-flag byte in d0. Once we know what byte
   this is, we know what our Machine Layer needs to set at tick time.

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
