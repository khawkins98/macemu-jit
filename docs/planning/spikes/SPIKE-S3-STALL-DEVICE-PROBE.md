# Spike S3 — Stall-loop device probe: pinning SCC vs VIA behind the two polling consumers

> **Status:** ✅ Complete (2026-06-10) · **Plan:** `docs/planning/MACHINE-LAYER-PLAN.md` §3 (pre-M0 spikes), decision 5 rev-3 caveat
> **Question:** Which device(s) — SCC 8530, VIA 6522, Cuda — and which register offsets are behind
> (a) the Mac OS 9.2.1 post-splash polling stall on the 1.1 ROM, and (b) the 9.0.1 nanokernel
> `check_work` poll? The project flip-flopped on this twice (commit `e0e8640e`: "VIA";
> HANDOFF §1.7.1 #3: "SCC"; our own AddrMap patch assigns 0xF3016000→VIA, 0xF3012000→SCC).

## Verdict (short form)

| Consumer | Device polled in the stall/poll loop | Other devices touched by the same routine cluster |
|---|---|---|
| **(a) 9.2.1 post-splash stall** — 68k serial test monitor at ROM 0xcc600–0xccb00 (1.1 ROM) | **SCC 8530, channel A** — RR0 bit 0 (Rx Character Available) at `0xF3012002`, data at `0xF3012006` | **VIA 6522 at 0xF3016000** (0x200 register stride): T2 timeout, IFR/IER, shift-register + ORB-handshake **Cuda** transactions — used by sibling routines in the *same module* (timeout-bounded probe, Cuda byte transfer) |
| **(b) nanokernel `check_work`** — PPC at 0x50326880 (9.0.1 parcels ROM) | **SCC 8530 only** — base from `[KDP-0x900]`, control at +2, data at +6; RR0 bit 0 read, RR0 bit 2 (Tx Buffer Empty) Tx poll, full WR init sequence | **DEC (decrementer, SPR 22)** — the timeout path at 0x50326520–0x5032656c reads DEC; no VIA, no Cuda |

**Flip-flop resolution:** HANDOFF §1.7.1 #3 is **correct** (`[KDP-0x900]` is an **SCC** base —
the disassembly below shows a textbook Zilog WR-register init through control offset +2).
Commit `e0e8640e`'s "this is the VIA base address" message is **wrong on identity** (its
*behavioral* finding — keep `[KDP-0x900]=0` so check_work returns −1 — remains valid).
The AddrMap patch (`rom_patches.cpp:1903–1905`) is **consistent with both**: slot `lp[2]`
(struct offset +8) = VIA1 = 0xF3016000, `lp[3]`/+0xC = SCCRd = 0xF3012000, `lp[4]`/+0x10 =
SCCWr = 0xF3012000 — and the 68k stall code is observed reading exactly those three slots
(see §1.2). SYSTEM-BOOT-GATES §5's register table is **mislabeled**: r18 (= 68k a2) is the
**VIA**, not "SCC channel A"; r19 (= 68k a3) is the SCC **read base** (offsets +2/+6 are
channel **A** control/data in the classic layout: B at +0/+4, A at +2/+6).

**Method note:** no emulator runs were needed. Disassembly inputs: `/tmp/rom_11_full.bin`
(post-`PatchROM` SS_DUMP_ROM image of the 1998 v1.1 ROM — bytes at 0xcc998 match the
documented stall) and `/tmp/rom901_decompressed.bin` (9.0.1 parcels ROM — `check_work`
prologue `mfsprg r1` at 0x326880 matches HANDOFF). Capstone M68K/PPC big-endian. The 68k→PPC
register mapping used to read the runtime probe data: d0–d7 → r8–r15, a0–a7 → r16–r23
(LEARNINGS "A3 = r19/gpr[19]"). Caveat: the 1.1 image is post-patch; none of SheepShaver's
ROM patches target the 0xca/0xcc module (verified: `scc_init`/`via_init*` patterns land
elsewhere), so the disassembly is original ROM code. The AddrMap *data* the module reads at
runtime, however, IS SheepShaver's constructed patch at `ADDR_MAP_PATCH_SPACE` (0x2fd140).

---

## 1. Consumer (a): the 9.2.1 post-splash stall — 68k serial test monitor

### 1.1 What this code actually is

The stall PC (0x500cc998, ROM offset 0xcc998) is inside a self-contained, threaded-code
(continuation in a5/a6, `jmp (a6)` returns) **serial diagnostic console** in the 1.1 ROM —
its banner string sits in the middle of the code at 0xcc8f8:

```
"\r\nSTM Version 2.2, Aaron Ludtke\r\nCTE Version 2.1\r\nROM Version ..."
```

i.e. Apple's factory **Serial Test Manager / Cold Test Engine**. It is an interactive
command monitor: init SCC → (optionally print banner) → poll for a serial character →
read a 2-byte command word → dispatch via jump tables at 0xcc150/0xcc17c. By design it
**waits for serial input**; a device model that merely answers "no character, no errors"
keeps it polling forever (see §1.6).

### 1.2 Where a2/a3/d3 come from — the AddrMap, directly

Module hardware-init entry at 0xcc81a (annotated):

```
0x500cc81a  26 0e            move.l a6,d3            ; save continuation
0x500cc81c  74 00            moveq  #$0,d2
0x500cc81e  4d fa 00 08      lea    0xcc828(pc),a6
0x500cc822  60 ff ...        bra.l  0xca7ca           ; → ori #$700,sr; bra.l 0x1130e
                                                      ;   = "get hardware config record" → a0
0x500cc828  2c 43            movea.l d3,a6
0x500cc82a  24 68 00 08      movea.l $8(a0),a2        ; a2 = rec+0x08 = VIA1Addr  = 0xF3016000
0x500cc82e  26 68 00 0c      movea.l $c(a0),a3        ; a3 = rec+0x0C = SCCRdAddr = 0xF3012000
0x500cc832  26 28 00 10      move.l $10(a0),d3        ; d3 = rec+0x10 = SCCWrAddr
0x500cc836  96 8b            sub.l  a3,d3             ; d3 = SCCWr − SCCRd  (= 0 on our map)
0x500cc838  4e d6            jmp    (a6)
```

The record offsets +0x08/+0x0C/+0x10 are exactly the AddrMap slots SheepShaver constructs
(`rom_patches.cpp` AddrMap: `lp[2]=0xf3016000, lp[3]=lp[4]=0xf3012000`), and the runtime
probe at the stall (`SS_PROBE_PC`, SYSTEM-BOOT-GATES §5) shows r18(a2)=0xF3016000,
r19(a3)=0xF3012000 — a perfect three-way match. So the polled addresses are *our own*
AddrMap values, which themselves match the real Gossamer/Heathrow MacIO map (§3).

### 1.3 SCC init sequence (the M1 device model's required write set, 68k side)

Table-driven init at 0xcc83c–0xcc850 writes (reg#, value) pairs to **control = write-base+2**
(`$2(a3,d3.l)`), terminator ≥0x80; on completion sets d7 bit 17 ("SCC initialized" flag):

```
0x500cc83c  4a 33 38 00      tst.b  (a3,d3.l)         ; touch write base
0x500cc840  43 fa 00 2c      lea    0xcc86e(pc),a1    ; init table
0x500cc844  10 19            move.b (a1)+,d0          ; reg #
0x500cc846  6b 0a            bmi.b  0xcc852           ; 0xFF terminator → done
0x500cc848  17 80 38 02      move.b d0,$2(a3,d3.l)    ; write register pointer (WR0)
0x500cc84c  17 99 38 02      move.b (a1)+,$2(a3,d3.l) ; write register data
0x500cc850  60 f2            bra.b  0xcc844
0x500cc852  08 c7 00 11      bset.b #$11,d7           ; flag: SCC inited (bit 17)
0x500cc856  10 2b 00 06      move.b $6(a3),d0         ; flush Rx data reg (ch A data)
```

Init table at 0xcc86e — pure Zilog 8530 programming:

| WR# | Value | Meaning |
|---|---|---|
| 9 | 0xC0 | Force hardware reset |
| 15 | 0x00 | No ext/status interrupts |
| 4 | 0x4C | x16 clock, 2 stop bits, no parity |
| 11 | 0x50 | Rx/Tx clock = BRG |
| 14 | 0x00 | BRG off (while loading) |
| 12 | 0x04 | BRG time-constant low |
| 13 | 0x00 | BRG time-constant high |
| 14 | 0x01 | BRG enable |
| 10 | 0x00 | NRZ |
| 3 | 0xC1 | Rx 8 bits/char, **Rx enable** |
| 5 | 0xEA | Tx 8 bits, **Tx enable**, DTR, RTS |
| 1 | 0x00 | No interrupts (polled mode) |

### 1.4 The stall loop itself — SCC ch A Rx poll

"Get char (non-blocking probe)" at 0xcc992 — **this is the observed HOT-PC region**:

```
0x500cc992  30 3c 80 00      move.w #$8000,d0         ; default: "no char" (bit 15 set)
0x500cc996  08 07 00 11      btst.b #$11,d7           ; SCC inited?
0x500cc99a  67 24            beq.b  0xcc9c0           ;   no → return $8000
0x500cc99c  08 2b 00 00 00 02 btst.b #$0,$2(a3)       ; SCC RR0 bit 0 = Rx Char Available
0x500cc9a2  67 1c            beq.b  0xcc9c0           ;   none → return $8000
0x500cc9a4  70 01            moveq  #$1,d0
0x500cc9a6  17 80 38 02      move.b d0,$2(a3,d3.l)    ; WR0 ← 1 (point at RR1)
0x500cc9aa  10 2b 00 02      move.b $2(a3),d0         ; read RR1
0x500cc9ae  02 00 00 70      andi.b #$70,d0           ; RR1 bits 4-6: parity/Rx-overrun/framing err
0x500cc9b2  67 06            beq.b  0xcc9ba
0x500cc9b4  17 bc 00 30 38 02 move.b #$30,$2(a3,d3.l) ; WR0 ← 0x30 (Error Reset cmd)
0x500cc9ba  e1 88            lsl.l  #$8,d0
0x500cc9bc  10 2b 00 06      move.b $6(a3),d0         ; read data (ch A data reg)
0x500cc9c0  4e d6            jmp    (a6)              ; return (d0 ≥ 0 ⇔ got char)
```

Two callers loop on it:

1. **Bounded probe** (module entry path, 0xcc670): a4 = retry counter, `subq.w #1` each
   pass, exit to 0xcc2d8 with d5='T'<<16 (timeout code) when exhausted.
2. **Unbounded blocking read** (0xcc9e0, "read d2 bytes"): `0xcc9f2: lea 0xcc9f8,a6; bra 0xcc992`
   → `0xcc9f8: tst.w d0; bmi.b 0xcc9f2` — **loops forever** until RR0 bit 0 = 1 and a data
   byte arrives. Called from the command loop with d2=2 (0xcc6c8) to fetch the next command
   word. This is the steady-state stall.

So the stall poll reads exactly two SCC registers: **RR0 (control, +2) bit 0** and, on
success, **RR1 error bits** + **data (+6)**.

### 1.5 VIA/Cuda usage in the same module (the "possibly VIA too" answer)

The module's sibling routines use a2 = VIA1 = 0xF3016000 with the canonical 6522 map at
**0x200 stride** (reg N at base + N*0x200):

| Code | VIA access | 6522 register |
|---|---|---|
| 0xcc85a `bset #3,$600(a2)`; 0xcc860–868 RMW `$1e00(a2)` clearing bit 3 | DDRA, ORA-no-handshake | drive PA3 low after SCC init |
| 0xcc94e `clr.b $1600(a2)` | ACR = 0 | one-shot T2, SR off |
| 0xcc952 `move.b #$20,$1c00(a2)` | IER ← 0x20 | disable T2 interrupt |
| 0xcc958/95e `#$ff → $1000/$1200(a2)` | T2C-L / T2C-H ← 0xFFFF | arm timer 2 |
| 0xcc96c `btst #5,$1a00(a2)` (poll, helper at 0xcc966 with d4 epoch countdown) | IFR bit 5 | **T2 timeout poll** |
| 0xcca8e/0xccabc `btst #2,$1a00(a2)` | IFR bit 2 | shift-register complete |
| 0xcca9a / 0xccab4 `$1400(a2)` | SR read / write | **Cuda byte transfer** |
| 0xcca8a/96/c8 `bclr/bset #4,(a2)`, 0xccaac `btst #3,(a2)` | ORB bits 4, 3 | Cuda handshake (TIP/byteack, TREQ) |

The **observed** stall loop touches none of these — but the module's timeout helper
(0xcc966, VIA T2 + IFR) and Cuda transfer (0xcca88) are live code on adjacent paths of the
same monitor (e.g. the bounded probe path and any Cuda-involving commands). A VIA whose IFR
T2 bit never sets would convert the *bounded* probe into an effectively unbounded one.

### 1.6 Why SheepShaver spins today, and what "fixed" looks like

Today 0xF3012002/0xF3016000 are unmapped; faults land in the sigsegv handler. The legacy
serial-skip hacks (`sheepshaver_glue.cpp:951–964`, `main_unix.cpp:2314–2331`) only match
SCC addresses in **gpr(8)/gpr(16)/gpr(20)** at specific PCs — the stall's pointer is in
**gpr(19)** (68k a3), so the access is eaten by the generic `ignoresegv` path instead,
leaving the destination register stale → RR0 bit 0 never reads 1 → infinite loop.

**Honest caveat on M1 sufficiency:** an SCC model that truthfully answers "Rx empty"
does **not** un-stall this loop — the monitor is *waiting for a human on a serial cable*.
Real progress requires one of: (i) understanding **why** 9.2.1's boot enters the STM at all
(entry path not traced in this spike — most plausibly a boot-time probe/error path; the
bounded-probe entry at 0xcc636 suggests the designed flow is probe → timeout → continue,
which a working VIA T2 + bounded path would satisfy), (ii) the VIA-T2 timeout path firing,
or (iii) feeding the expected protocol bytes. **The pre-stall escape is plausibly the
VIA T2 timeout, not an SCC behavior** — which is exactly why M1 should include the VIA
timer surface (see §4).

---

## 2. Consumer (b): nanokernel `check_work` — 9.0.1 parcels ROM

Applies to the **9.0.1 ROM** (the addresses 0x326880/0x32751c/0x3272e0 don't exist as code
in the 1.1 ROM — that region is zero there). Annotated disassembly
(`/tmp/rom901_decompressed.bin`):

### 2.1 The poll (0x50326880)

```
0x50326880  mfsprg r1            ; r1 = KDP
0x50326884  stmw   r24,-0x108(r1); save r24-r31
...
0x5032689c  lwz    r28,-0x900(r1); r28 = [KDP-0x900]  ← the device base
0x503268a0  cmpwi  cr7,r28,0
0x503268a4  li     r8,-1
0x503268a8  beq    cr7,0x503265cc; base==0 → return -1 ("no work")
0x503268b4  addi   r8,r1,-0xaf0  ; param block
0x503268b8  bl     0x50312700    ; BAT3 setup for the device page
0x503268c4  bl     0x50326ae8    ; (BAT helper: saves SRR0/1, masks r28 to segment, PVR check)
0x503268c8  ori    r30,r31,0x10  ; MSR.DR on
0x503268cc  mtmsr  r30 ; isync
0x503268d4  lbz    r30,2(r28)    ; ★ read control reg (+2) = SCC RR0
0x503268d8  eieio
0x503268dc  andi.  r30,r30,1     ; ★ bit 0 = Rx Character Available
0x503268e0  beq    0x50326520    ;   no char → timeout/return path
0x503268e4  lbz    r8,6(r28)     ; ★ read data reg (+6) — the character
0x503268e8  b      0x50326520
```

### 2.2 The Tx variant (0x503268ec entry, poll at 0x5032695c)

```
0x5032695c  lbz   r30,2(r28)     ; RR0
0x50326964  andi. r30,r30,4      ; bit 2 = Tx Buffer Empty
0x50326968  beq   0x5032695c     ; spin until empty
0x5032696c  stb   r8,6(r28)      ; write data byte
```

### 2.3 The init subroutine (0x50326980) — alternating reg#/data writes to +2

Exactly the pattern HANDOFF §1.7.1 #3 described, now fully decoded. Every write is
`stb rX, 2(r28)` + `eieio`, with an `lbz 2(r28)` (reset register pointer) before each pair:

| WR# | Value | | WR# | Value |
|---|---|---|---|---|
| 9 | 0x80 | reset channel A | 11 | 0x50 | clocks = BRG |
| 4 | 0x48 | x16 clock, 1.5/2 stop | 12 | 0x0C | BRG TC low |
| 3 | 0xC0 | Rx 8 bits (not yet enabled) | 13 | 0x00 | BRG TC high |
| 5 | 0x60 | Tx 8 bits | 14 | 0x01 | BRG enable |
| 9 | 0x00 | no interrupt vector | 3 | 0xC1 | Rx 8 bits, **Rx enable** |
| 10 | 0x00 | NRZ | 5 | 0xEA | Tx enable, DTR, RTS |

WR3=0xC1 / WR5=0xEA are byte-identical with the 68k monitor's table (§1.3) — same Apple
serial-debug lineage. **This is conclusively a Zilog SCC 8530, channel A of the legacy
(SCCRd-style) layout: control +2, data +6.** A VIA 6522 has no register-pointer protocol,
no registers at +2/+6 (its stride is 0x200 on MacIO), and no 0x80-reset/BRG semantics.

### 2.4 The timeout path uses DEC (0x50326520–0x5032656c)

```
0x5032652c  lwz   r29,-0x438(r1) ; timeout constant from KDP
0x50326530  srwi  r29,r29,8
0x50326534  mfspr r30,22         ; ★ DEC (decrementer)
0x50326538  subf  r29,r29,r30    ; deadline = DEC - timeout
0x50326548  mfspr r30,22         ; loop: read DEC
0x5032654c  subf. r30,r29,r30
0x50326550  ble   0x50326570     ; deadline passed → give up
0x50326554  li r30,1; stb r30,2(r28)  ; WR0 ← 1 (point at RR1)
0x50326560  lbz r30,2(r28)       ; read RR1
0x50326568  andi. r30,r30,1      ; bit 0 = All Sent
0x5032656c  beq   0x50326548
```

→ `check_work`'s drain/timeout logic **requires a ticking DEC** (today DEC reads return 0
or `SS_SYNTH_DEC` [post-M2: deprecated alias — see MACHINE-LAYER-PLAN M2]). This couples consumer (b) to the M2 virtual clock: a real SCC model at
`[KDP-0x900]` without a moving DEC turns this loop into a second spin site.

### 2.5 The idle-loop consumer (0x5032751c, prologue 0x503272e0)

Confirmed as documented (HANDOFF §1.7.1 #4): full machine-state save (GPRs/FPRs/SRs into
KDP+0x700..0x8fc), then:

```
0x50327518  bne  cr1,0x50327540  ; cr1eq clear = explicit char-processing path
0x5032751c  lwz r1,0(0); addi r1,r1,1; stw r1,0(0)   ; counter at addr 0
0x50327530  bl   0x50326880      ; check_work
0x50327534  cmpwi r8,-1
0x50327538  bne  0x50327540      ; got char → set KDP+0xedc bit 1 → console at 0x5032756c
0x5032753c  b    0x5032751c      ; idle spin
```

(String "noKernel deb…" immediately after = the Thud nanokernel debug console.) The idle
loop polls **only** the SCC via check_work — no VIA, no timer, confirming that on the
fidelity profile its wake-up must come from real SCC state + interrupt delivery, and that
M1's DoD item "check_work polls a real device at an unmapped F3 address" is SCC-only.

---

## 3. Cross-check: canonical MacIO map vs the AddrMap patch

Gossamer/Heathrow-class MacIO at 0xF3000000 (Heathrow datasheet / QEMU `macio` / DingusPPC
`heathrow.cpp` agree):

| Offset | Device | AddrMap patch slot (`rom_patches.cpp:1895ff`) |
|---|---|---|
| +0x00000 | MacIO interrupt regs | `lp[43] = 0xf3000000` ✓ |
| +0x10000 | SCSI (MESH) | `lp[25] = 0xf3010000` ✓ |
| +0x11000 | BMAC ethernet | `lp[34] = 0xf3011000` ✓ |
| +0x12000 | **ESCC compat port (legacy SCC layout: chB ctl +0, chA ctl +2, chB data +4, chA data +6)** | `lp[3]=lp[4] = 0xf3012000` (SCCRd=SCCWr) ✓ |
| +0x13000 | ESCC MacRISC port (0x10 stride) | not used by these consumers |
| +0x14000 | AWACS sound | `lp[39] = 0xf3014000` ✓ |
| +0x15000 | SWIM3 floppy | `lp[38] = 0xf3015000` ✓ |
| +0x16000 | **VIA-Cuda (6522, reg stride 0x200)** | `lp[2] = 0xf3016000` ✓ |
| +0x18000 | IDE | `lp[24] = 0xf3018000` ✓ |

Both consumers use the **compat/legacy ESCC port** addressing (+2/+6 = channel A), not the
MacRISC 0x10-stride port. All polled offsets are 16 KB-alignable for the MMIO bus (the
whole 0xF3000000–0xF3020000 block is device space) — consistent with MACHINE-LAYER-PLAN
§2b's constraint.

---

## 4. Recommended M1 device scope

1. **SCC 8530 model — required, both consumers.** Minimum register surface:
   - Legacy/compat layout at SCCRd/SCCWr base (one base, read=write is fine — the 68k code
     computes `d3 = SCCWr − SCCRd` and handles 0): **channel A control +2, channel A data +6**.
     (Channel B at +0/+4 untouched by either consumer; stub it anyway — same decoder.)
   - WR register-pointer state machine (WR0 write selects register; any read/write resets
     pointer to 0), WR0 commands (0x30 Error Reset, 0x80 ch-A reset, 0xC0 hw reset).
   - RR0: bit 0 Rx Char Available, bit 2 Tx Buffer Empty (return 1 — Tx always ready).
   - RR1: bit 0 All Sent (return 1), bits 4–6 error bits (return 0).
   - WR3/WR4/WR5/WR9/WR10/WR11/WR12/WR13/WR14/WR15/WR1 as stored state (init sequences in
     §1.3/§2.3 are the conformance test vectors).
2. **VIA 6522 model at 0xF3016000 — include the timer/IFR surface in M1.** The same 68k
   module arms **T2** and polls **IFR bit 5** as its timeout escape (§1.5) — this is the
   most plausible designed exit from the bounded probe path, i.e. possibly the actual fix
   for the 9.2.1 stall (an SCC alone that truthfully says "no char" does not terminate the
   blocking read). Scope: registers at 0x200 stride; ORA/ORB/DDRA/DDRB as stored state;
   T1/T2 counters decrementing off the virtual clock; IFR/IER semantics. **Cuda
   shift-register protocol (SR + ORB handshake) can be a loud stub in M1** — it's adjacent
   code, not the observed poll — but the register *decode* must exist so a touch is visible
   telemetry, not a silent fault-skip.
3. **DEC must tick** for consumer (b)'s drain/timeout loop (§2.4). M1 can keep `SS_SYNTH_DEC`
   as the interim; the real fix is M2 — note the dependency explicitly in M1's DoD. [post-M2: `SS_SYNTH_DEC` is a deprecated alias — see MACHINE-LAYER-PLAN M2; the M2 virtual clock is the live implementation]
4. **Retire the gpr-pattern serial-skip hacks on the fidelity/named-third config** (they
   never matched this stall anyway — wrong register, §1.6) per the plan's M1 DoD.

## 5. Remaining ambiguity (flagged honestly)

- **Why 9.2.1 enters the STM monitor at all is not traced.** This spike pins what the loop
  polls, not the entry path from the 9.2.1 boot code into 0xcc6xx. If entry is via the
  bounded probe (0xcc636, retry count in a4 + VIA-T2 timeout helper), an SCC+VIA model lets
  it time out and continue. If 9.2.1 deliberately blocks on a 2-byte command read
  (0xcc9e0), no honest device model unblocks it and the fix is upstream of the monitor.
  Recommend a cheap follow-up: SS_PROBE_PC on 0xcc636/0xcc686/0xcc2d8 (+ mirror
  ROMBase+0x480000 equivalents) during one 9.2.1 boot to see which entry/exit fires.
- The a0 config record fetched via 0x1130e was confirmed by *runtime values* (a2/a3 match
  the AddrMap slots exactly) rather than by statically tracing 0x1130e's table walk to the
  AddrMap bytes; a different ROM data structure carrying identical addresses would be
  indistinguishable — and immaterial for device scope.
- The 1.1-ROM disassembly is from a post-`PatchROM` dump. Spot checks show no SheepShaver
  patch touches 0xca2c8–0xccbff, but a raw-decompressed-1.1 diff was not produced.
- LEARNINGS line 31 ("r19 = SCC channel B") and SYSTEM-BOOT-GATES §5 ("r18 = SCC channel A")
  carry the stale mislabels; correct them when next touched (r18=VIA base, r19=SCC base,
  +2/+6 = channel A).
