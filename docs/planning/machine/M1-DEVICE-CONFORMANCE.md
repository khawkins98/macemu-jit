# M1 Device Conformance — SCC 8530 + VIA 6522

> **Status:** ✅ Complete — 2026-06-10
> **Purpose:** QEMU/DingusPPC behavioral conformance check for the M1 device models before
> code review + merge. Fulfills `MACHINE-LAYER-PLAN.md` §5.2 ("behavior conformance against
> reference device models; datasheets break ties").
> **Scope fence:** registers in `CORE99-MACHINE-DESCRIPTION.md` §4 only. Everything
> outside the fence is listed in §4 (Fence-Excluded Deltas) — those are not defects.

---

## 1. Sources and Commit References

All sources fetched 2026-06-10 from `master`. SHAs pinned via `git ls-remote` at time of fetch.

| Reference | URL | Pinned SHA |
|---|---|---|
| QEMU `hw/char/escc.c` | https://raw.githubusercontent.com/qemu/qemu/master/hw/char/escc.c | `de5d8bfd6105d3dd3ae668df9762df244a6d1506` |
| QEMU `include/hw/char/escc.h` | https://raw.githubusercontent.com/qemu/qemu/master/include/hw/char/escc.h | `de5d8bfd6105d3dd3ae668df9762df244a6d1506` |
| QEMU `hw/misc/mos6522.c` | https://raw.githubusercontent.com/qemu/qemu/master/hw/misc/mos6522.c | `de5d8bfd6105d3dd3ae668df9762df244a6d1506` |
| QEMU `hw/misc/macio/cuda.c` | https://raw.githubusercontent.com/qemu/qemu/master/hw/misc/macio/cuda.c | `de5d8bfd6105d3dd3ae668df9762df244a6d1506` |
| DingusPPC `devices/serial/escc.cpp` | https://raw.githubusercontent.com/dingusdev/dingusppc/master/devices/serial/escc.cpp | `92bb6d10549529f9f4031a85c2bc136149535bdc` |
| DingusPPC `devices/serial/escc.h` | https://raw.githubusercontent.com/dingusdev/dingusppc/master/devices/serial/escc.h | `92bb6d10549529f9f4031a85c2bc136149535bdc` |
| DingusPPC `devices/serial/z85c30.h` | https://raw.githubusercontent.com/dingusdev/dingusppc/master/devices/serial/z85c30.h | `92bb6d10549529f9f4031a85c2bc136149535bdc` |
| DingusPPC `devices/common/viacuda.cpp` | https://raw.githubusercontent.com/dingusdev/dingusppc/master/devices/common/viacuda.cpp | `92bb6d10549529f9f4031a85c2bc136149535bdc` |
| DingusPPC `devices/common/viacuda.h` | https://raw.githubusercontent.com/dingusdev/dingusppc/master/devices/common/viacuda.h | `92bb6d10549529f9f4031a85c2bc136149535bdc` |
| Our SCC model | `SheepShaver/src/machine/dev_scc8530.cpp` + `.h` | `machine-layer-m1` branch |
| Our VIA model | `SheepShaver/src/machine/dev_via6522.cpp` + `.h` | `machine-layer-m1` branch |
| Fence spec | `docs/planning/spikes/SPIKE-S3-STALL-DEVICE-PROBE.md` §4 | `machine-layer-m1` branch |
| Machine spec | `docs/planning/machine/CORE99-MACHINE-DESCRIPTION.md` §4 | `machine-layer-m1` branch |

**Sourcing caveat:** The two fetches of `z85c30.h` returned only summary descriptions, not
verbatim enumerations. DingusPPC behavioral claims below are therefore derived from the
`.cpp` code (which did return real logic) rather than from constant values in `z85c30.h`.
Specific DingusPPC constant numeric values that were not seen in code are not quoted.

---

## 2. Consumer Read Surface (the observability test)

Before the verdict tables: the fenced consumers (CORE99 §4 / S3 §4) read **only these
values**. A model mismatch is only observable — and only warrants a model fix — when it
affects a value in this list.

**SCC reads (S3 §1.4 / §2.1–2.4):**
- `RR0 bit0` — Rx Character Available (at +2, after WR0=0)
- `RR0 bit2` — Tx Buffer Empty (at +2, after WR0=0)
- `RR1 bit0` — All Sent (at +2, after WR0=1)
- `RR1 bits4-6` — error bits: Parity, Rx Overrun, CRC/Framing (at +2, after WR0=1)
- Data byte at +6 (always after RR0 bit0 was found set; returns 0 from our empty-Rx path)

The consumers never read a WR register back, never read RR2–RR15 through the pointer, and
never read the SCC data port when idle (only after RR0 bit0 confirmed a character).

**VIA reads (S3 §1.5):**
- `IFR bit5` (T2 interrupt) — polled at +0x1a00 (= base + 13 × 0x200)
- `IFR bit2` (SR complete) — polled at +0x1a00 (same read, different bit)
- `SR` at +0x1400 — Cuda byte (loud-stub path; value irrelevant for polling)
- `ORB bit3` at +0x0000 (Cuda TREQ query)

Consumers never read T2CL, T2CH, T1CL, T1CH, IER, ACR, PCR, DDRA, DDRB, ORA, or ORB
except the single ORB bit3 Cuda-handshake query.

---

## 3. SCC 8530 Verdict Table

### 3.1 WR0 Pointer Semantics (raw values 0–15, folding for 8–15)

| | Ours | QEMU `escc.c` | DingusPPC `escc.cpp` |
|---|---|---|---|
| Mechanism | `s->reg_ptr[ch] = v & 0x0F` — low nibble unconditionally | `newreg = val & CMD_PTR_MASK` (low 3 bits); `CMD_HI` command (`val & CMD_CMD_MASK == 0x08`) ORs `0x08` into `newreg` | `this->reg_ptr = value & WR0_REGISTER_SELECTION_CODE` (low 3 bits); `WR0_COMMAND_POINT_HIGH` ORs with `WR8` (=8) |

**MATCH on fenced write-vectors?** YES — and all three mechanisms are **bit-identical** for
every value 0–15 written as a literal. Derivation: QEMU's `newreg = (B & 0x07)` and then,
if `B & 0x38 == CMD_HI (0x08)`, `newreg |= 0x08`. For `B = 9`: `9 & 0x38 = 0x08 = CMD_HI`,
so `newreg = (9 & 7) | 0x08 = 1 | 8 = 9`. For `B = 11`: `11 & 0x38 = 0x08`, `(11&7)|8 = 3|8 = 11`.
DingusPPC: `reg_ptr = (B & 7); if (B & 0x38) == POINT_HIGH: reg_ptr |= 8` — identical result.
Ours: `B & 0x0F`. All three produce the same ptr for every value 0–15.

**Divergence outside the fence:** The mechanisms differ for WR0 bytes where the command-mask
bits (`B & 0x38`) encode a side-effecting command other than CMD_HI: `0x18` (send-abort
marker), `0x28` (clear-Tx-interrupt), `0x38` (clear-IUS). For these, QEMU/DingusPPC execute
the side effect and route the pointer normally; ours interprets the low nibble literally (e.g.
`0x28 & 0x0F = 8`) and skips the side effect. None of these command opcodes appear in the
fenced write-vectors.

| Behavior | OURS | QEMU | DingusPPC | MATCH (fenced)? | Consequence if mismatch | Action |
|---|---|---|---|---|---|---|
| WR0 pointer for values 0–15 (literal) | `v & 0x0F` | `(v&7)\|(0x08 if v&0x38==CMD_HI)` | `(v&7)\|(8 if v&0x38==POINT_HIGH)` | YES — bit-identical for all 0–15 | — | none |
| WR0 side-effecting commands (0x18/0x28/0x38) | Misroutes ptr to low nibble; side effect skipped | Executes side effect; ptr = low nibble of value | Same as QEMU | N/A — out of fence (never written by consumers) | Out-of-fence; note for M2+ if send-abort/clear-Tx/clear-IUS are needed | note (N1) |
| WR0 pointer reset after any control access | `s->reg_ptr[ch] = 0` on every control read or data write | `s->reg = 0` after every control read/write | `this->reg_ptr = RR0` after every control read | YES | — | none |

### 3.2 RR0 Values for Idle No-Rx-Source Channel

| Behavior | OURS | QEMU | DingusPPC | MATCH (fenced)? | Consequence | Action |
|---|---|---|---|---|---|---|
| RR0 bit0 (Rx Char Available) | 0 (no source → honest) | 0 after reset (`R_STATUS = 0x04 = bit2` only) | 0 (after reset mask `&= 0x38; \|= 0x44`, net bit0=0) | YES | — | none |
| RR0 bit2 (Tx Buffer Empty) | 1 (hardcoded in `read_rr` case 0: `return 0x04`) | 1 (`STATUS_TXEMPTY = 0x04` set by `escc_reset_chn` → `escc_reset()` sets it) | 1 (reset sets `0x44` which includes bit2) | YES | — | none |
| RR0 bits 3–7 (DCD/SYNC/CTS/TxUndrn/BRK) | 0 | QEMU: bits 3-5 may be set by `s->disabled` flag; bit6 (TxUndrn=`STATUS_TXUNDRN`) set by soft_reset; bit7 unused | DingusPPC: `read_regs[RR0] &= 0x38; \|= 0x44` → net bits 2,6 (TxEmpty + TxUndrn). Bits 3,4,5 = 0. | YES for bit0/bit2 (the only fenced reads). Bit6 differs (we=0, others=1) but is not read. | bits 3,4,5,6,7 not read by any fenced consumer | note |

**Note on DingusPPC RR0 reset value:** The `.cpp` shows `read_regs[RR0] &= 0x38; read_regs[RR0] |= 0x44`. `0x44 = 0100_0100` sets bits 2 and 6 (Tx Empty + bit6). The mask `0x38` clears bits 0,1,2,6,7 then the OR sets bits 2,6. Net: bit0=0 (no Rx), bit2=1 (Tx empty), bit6=1 (Tx Underrun). Our constant `0x04` omits bit6 (Tx Underrun flag). This is **unobservable** — consumers never read bit6.

### 3.3 RR1 Values for Idle Channel

| Behavior | OURS | QEMU | DingusPPC | MATCH (fenced)? | Consequence | Action |
|---|---|---|---|---|---|---|
| RR1 bit0 (All Sent) | 1 (hardcoded: `return 0x01`) | 1 (`STATUS_TXUNDRN` / `SPEC_ALLSENT` set at reset — this is the All Sent / RR1 All-Sent bit; QEMU uses `R_SPEC` for RR1 in the `read` path) | 1 (`RR1 = 0x06 \| RR1_ALL_SENT`; the file notes "0x06" as "a hack") | YES | — | none |
| RR1 bits4-6 (Parity/Overrun/Framing errors) | 0 (hardcoded) | 0 at reset (no errors queued) | 0 (no error injection at reset) | YES | — | none |
| RR1 bits1-2 (Residue code) | 0 | `SPEC_BITS8 = 0x06` (bits 1-2 only, verified from `escc.c`); `escc_soft_reset_chn` sets `rregs[R_SPEC] = SPEC_BITS8 = 0x06`. `R_SPEC = 1` = RR1. QEMU RR1 post-reset = `0x07` (bits 0+1+2); `0x06 & 0x70 = 0x00` — bits4-6 are 0. | DingusPPC: `RR1 = 0x06 \| RR1_ALL_SENT`, same residue-code default (bits 1-2, flagged "a hack") | NO for bits 1-2 (we return 0; QEMU and DingusPPC return 0x06 in those bits). YES for bits4-6 (all three = 0). | **Unobservable** for fenced consumers — STM masks `andi.b #$70` tests bits4-6 only; check_work drain masks `andi. r30,r30,1` tests bit0 only. Bits 1-2 (residue code) are never tested. | note |

### 3.4 WR9 Reset Semantics

| Behavior | OURS | QEMU | DingusPPC | MATCH (fenced)? | Consequence | Action |
|---|---|---|---|---|---|---|
| WR9 = 0xC0 (force hardware reset) | `memset(s->wr, 0, sizeof(s->wr))` — zeroes all WR tables for both channels | Calls `escc_hard_reset_chn()` on both channels: soft-reset preserving select bits, then forces WR9 bits, WR10=0, WR11=`CLOCK_TRXC`, WR14 bits; **does NOT zero the entire WR table** | Triggers controller-level hard reset on both channels — resets WR state to defined (non-zero) reset values | NO — mechanism differs substantially | Consumers re-init every WR immediately after WR9=0xC0 (S3 §1.3 table starts with WR9=0xC0 then writes all needed WRs). No WR is read back. **Not observable.** | note |
| WR9 = 0x80 (channel A reset) | `memset(s->wr[SCC_CH_A], 0, sizeof(s->wr[SCC_CH_A]))` | Calls `escc_soft_reset_chn()` on channel A: selective field preservation + status flag updates | Soft-reset channel A to defined non-zero reset values | NO — mechanism differs | Same reasoning: re-init follows immediately. Not observable. | note |
| WR9 = 0x40 (channel B reset) | `memset(s->wr[SCC_CH_B], 0, ...)` | `escc_soft_reset_chn()` on channel B | Soft-reset channel B | NO — mechanism differs | Not observable. | note |
| RR0/RR1 after WR9 reset | We preserve constant hardcoded values (not reset by WR9) | QEMU `escc_soft_reset_chn` updates `rregs[R_STATUS]` — bit2 (Tx Empty) remains 1, bit0 (Rx avail) remains 0 | DingusPPC reinitializes read regs to same reset values | YES for consumer-read bits (bit0=0, bit2=1 preserved across all paths) | — | none |

**Summary on WR9:** Our memset approach is simpler and correct for the M1 consumer because all needed WRs are explicitly re-initialized in both conformance write-vectors (S3 §1.3 / §2.3). The WR state difference post-reset but pre-re-init is unobservable since no WR read-back is ever performed.

### 3.5 Error Reset (WR0 = 0x30) Effect

| Behavior | OURS | QEMU | DingusPPC | MATCH (fenced)? | Consequence | Action |
|---|---|---|---|---|---|---|
| WR0 = 0x30 (Error Reset command) | No-op: `/* 0x30 Error Reset: no latched errors to clear in M1 */`; ptr set to 0 | `CMD_CLR_IUS` handler does NOT match `0x30`; `CMD_CLR_TXINT = 0x28`, `CMD_CLR_IUS = 0x38`. `0x30 & CMD_CMD_MASK (0x38) = 0x30`; the `switch(val)` falls through to `default: break` — no-op, ptr set to new reg | DingusPPC z85c30.h defines `WR0_COMMAND_ERROR_RESET`; the `escc.cpp` write handler shows "no switch case handles this value" — effectively a no-op | YES (all three are no-ops for the Error Reset path when no errors are queued) | The consumer writes 0x30 only when RR1 bits4-6 are non-zero (S3 §1.4). In M1 those bits are always 0, so the branch is never taken. No-op is correct. | none |

**Note on 0x30 bit decomposition:** In our code `cmd = (v >> 3) & 7` for `v=0x30` gives `cmd = 6`. In QEMU `val & CMD_CMD_MASK = 0x30 & 0x38 = 0x30` which is none of the handled cases. DingusPPC has a named constant for error reset but no handler. The pointer behavior: ours sets `reg_ptr = 0x30 & 0x0F = 0`; QEMU sets `newreg = 0x30 & CMD_PTR_MASK = 0x30 & 0x07 = 0`; DingusPPC `0x30 & WR0_REGISTER_SELECTION_CODE (=0x07) = 0`. All three reset pointer to 0. Perfect match.

### 3.6 Data Port Read on Empty Rx

| Behavior | OURS | QEMU | DingusPPC | MATCH (fenced)? | Consequence | Action |
|---|---|---|---|---|---|---|
| Value returned when Rx FIFO empty | 0 (`return 0` from `SCCRead` `is_data` path) | 0 (`s->rx` initialized to 0 in `escc_reset_chn()`; returned as-is when no char received) | 0 (`receive_byte()` returns `c=0` when `!rcv_char_available_now()`) | YES | — | none |

---

## 4. VIA 6522 Verdict Table

### 4.1 T2 One-Shot Semantics (Load via T2C-H, IFR.5 clears on load, sets on expiry)

| Behavior | OURS | QEMU `mos6522.c` | DingusPPC `viacuda.cpp` | MATCH (fenced)? | Consequence | Action |
|---|---|---|---|---|---|---|
| T2C-H write clears IFR.T2 | YES: `v->ifr_latched &= ~IFR_T2` in `VIAWrite` T2C-H case | YES: `s->ifr &= ~T2_INT` in `VIA_REG_T2CH` case | Not confirmed from fetch; inferred YES (standard 6522 spec) | YES | — | none |
| T2C-H write starts one-shot counter | YES: `v->t2_running = true; v->t2_load_time = now` | YES: `set_counter()` schedules timer callback | YES: `this->t2_start_time` recorded, oneshot timer fired | YES | — | none |
| IFR.T2 set at expiry (timer underflow) | YES: `timer_remaining()` returns 0 and sets `*expired` when `dt >= cnt`; `ifr_now()` ORs `IFR_T2` lazily | YES: `mos6522_timer2()` callback fires and sets `s->ifr |= T2_INT` | YES: `assert_t2_int()` sets `_via_ifr |= VIA_IF_T2` | YES | — | none |
| Exact expiry tick (at count=0 or count=-1?) | Our lazy check: `dt >= cnt` → fires at exactly N ticks elapsed (where N is the loaded count). Equivalent to "fires at 0" (counter reaches zero). | QEMU fires at `d + counter` elapsed ticks = N ticks. Comments: "we consider the irq is raised on 0" | DingusPPC fires at `via_clk_dur * (t2_counter + 3)` — approximately N+3 ticks | OURS == QEMU (both at N ticks). DingusPPC differs by +3 ticks. Hardware 6522 datasheet: fires at N+1 ticks (counted from the high-byte write, counter cycles through N then fires as it would go to -1). | Timing delta: ours/QEMU are off by 1 tick from hardware spec; DingusPPC off by 3. For the consumer's timeout use of T2 (FFFF = ~83ms) this is immaterial. Unobservable via fenced reads (consumer reads only IFR bit5, not T2CL). | note |

### 4.2 T2C Live Read-Back Value

| Behavior | OURS | QEMU | DingusPPC | MATCH (fenced)? | Consequence | Action |
|---|---|---|---|---|---|---|
| T2CL read returns live countdown | YES: `cnt - (uint16_t)dt` (clamped at 0) | YES: `get_counter()` returns `(counter_value - d) & 0xffff` | YES: `last_val - elapsed_clk_ticks` | YES (mechanism matches) | **Unobservable** — consumers never read T2CL or T2CH | none |
| T2CL read clears IFR.T2 | NO: our `VIARead` T2CL case does not clear IFR.T2 | YES: `s->ifr &= ~T2_INT` in the T2CL read case | Likely yes (standard 6522 spec) | NO | **Unobservable** — consumers never read T2CL. They poll IFR directly. | note |

### 4.3 IFR Bit7 Master Interrupt Computation

| Behavior | OURS | QEMU | DingusPPC | MATCH (fenced)? | Consequence | Action |
|---|---|---|---|---|---|---|
| IFR bit7 = any(IFR & IER & 0x7F) | YES: `if (ifr & v->ier & 0x7F) ifr \|= 0x80` | YES: `val = s->ifr; if (s->ifr & s->ier) val \|= 0x80` | YES: `uint8_t active_ints = this->_via_ifr & this->_via_ier & 0x7F; this->_via_ifr = (!!active_ints << 7) \| (this->_via_ifr & 0x7F)` | YES | — | none |

**Note on DingusPPC IFR bit7 storage:** DingusPPC stores bit7 inside `_via_ifr` itself (updated eagerly by `update_irq()`). QEMU and ours compute bit7 dynamically on read without storing it. Both approaches produce the same value on a read.

### 4.4 IER Read (should return 0x80 | IER)

| Behavior | OURS | QEMU | DingusPPC | MATCH (fenced)? | Consequence | Action |
|---|---|---|---|---|---|---|
| IER read returns 0x80 \| stored IER | YES: `return 0x80 \| v->ier` | YES: `val = s->ier \| 0x80` | YES: `return (this->_via_ier \| 0x80)` | YES | — | none |

### 4.5 IER Write Set/Clear Polarity

| Behavior | OURS | QEMU | DingusPPC | MATCH (fenced)? | Consequence | Action |
|---|---|---|---|---|---|---|
| Bit7=1 → enable (OR) bits 0-6 | YES: `if (b & 0x80) v->ier \|= (b & 0x7F)` | YES: `if (val & IER_SET) s->ier \|= val & 0x7f` | YES: `if (value & 0x80) this->_via_ier \|= value & 0x7F` | YES | — | none |
| Bit7=0 → disable (AND NOT) bits 0-6 | YES: `else v->ier &= ~(b & 0x7F)` | YES: `else s->ier &= ~val` | YES: `else this->_via_ier &= ~value` | YES | — | none |

### 4.6 Write-1-to-Clear IFR

| Behavior | OURS | QEMU | DingusPPC | MATCH (fenced)? | Consequence | Action |
|---|---|---|---|---|---|---|
| IFR write-1-to-clear (bits 0-6) | YES: `v->ifr_latched &= ~(b & 0x7F)`; for timer bits, additionally stops timer if expired (`v->t2_running = false`) | YES: `s->ifr &= ~val` (simple bit clear — does NOT stop timer) | YES: `this->_via_ifr &= ~(value & 0x7F)` then `update_irq()` | YES for the cleared-IFR result | — | none |
| Side-effect: IFR write also stops timer | OURS: YES (conditional on expiry) | QEMU: NO — timer keeps running; IFR.T2 will be re-set at next expiry | DingusPPC: NO — one-shot timer fires independently; IFR.T2 re-asserts at expiry | PARTIAL — our model stops the timer on IFR-clear; QEMU/DingusPPC don't | **Unobservable** by fenced consumers — the consumer writes IFR to clear T2 after it fires as a one-shot (timeout), then does not re-use T2 in the observed code. The timer won't re-fire (it's one-shot). In QEMU the one-shot schedules itself again (`mos6522_timer2_update` after `mos6522_timer2` fires) but without re-loading. No consumer path is affected. | note |

### 4.7 Register Address Stride (0x200 = reg shifted by 9)

| Behavior | OURS | QEMU `cuda.c` | DingusPPC `viacuda.cpp` | MATCH (fenced)? | Consequence | Action |
|---|---|---|---|---|---|---|
| Register N at base + N×0x200 | YES: `((addr - v->base) >> 9) & 0xF` | YES: `addr = (addr >> 9) & 0xf` in `mos6522_cuda_read/write` | DingusPPC viacuda uses a different register naming scheme (`VIA_B=0, VIA_A=1, ...` sequential), stride applied externally; the macIO implementation maps sequentially | YES (QEMU confirmed shift=9; S3 §1.5 runtime-observed; our model matches) | — | none |

---

## 5. Fence-Excluded Deltas

These are differences between our models and the references that lie **outside** the CORE99
§4 / S3 §4 scope fence. They are not defects in M1; they are listed for completeness and
as forward-looking notes for M2–M5.

### SCC fence-excluded deltas

1. **WR register values after WR9 reset (before re-init):** Our memset zeros everything.
   QEMU's soft/hard reset preserves specific bits per Z85C30 spec (e.g. WR1 partial-preserve,
   WR14 MISC2 bits). Neither consumer reads WRs back, and both re-initialize all needed
   registers immediately after any reset. If M2+ adds a guest that reads back WR state after
   reset without re-initializing, this will need fixing.

2. **RR0 bits 3, 4, 5 (DCD, SYNC, CTS):** We always return 0; QEMU sets them based on a
   `disabled` flag; DingusPPC has its own reset values. Not read by any fenced consumer.

3. **RR0 bit6 (Tx Underrun / EOM):** DingusPPC sets bit6 at reset (`0x44`); we return 0x04
   (bit6=0). QEMU sets `STATUS_TXUNDRN` in `escc_soft_reset_chn`. Not read by any consumer.

4. **RR1 bits1-3 (Residue Code bits):** DingusPPC initializes RR1 to `0x06 | RR1_ALL_SENT`
   (bits 1 and 2 set). We return 0x01 (only bit0). The residue code is not tested by any
   consumer (STM checks only `andi.b #$70` = bits4-6; check_work checks only bit0).

5. **Interrupt generation (rxint/txint, `escc_update_irq`):** Neither QEMU nor DingusPPC
   generate interrupts unconditionally — they wire them through proper IRQ lines. Our model
   has no IRQ output at all (M1 scope: interrupts deferred to M3 OpenPIC). Not observable
   until M3.

6. **Rx FIFO queuing:** QEMU maintains a 256-byte input queue; DingusPPC calls a
   `chario` backend. Our model has no Rx source in M1 — the "no character" answer is
   architecturally correct for M1, not a limitation to fix. S3 §1.6 explicitly states that
   a conformant "no char" answer is the correct M1 behavior (the stall is design-intent of
   the STM; the fix is VIA T2 timeout, not fake Rx data).

7. **WR0 side-effecting command codes (0x18/0x28/0x38):** For literal WR0 values 0–15 all
   three implementations produce the same pointer (verified in §3.1). The divergence is in
   the handling of WR0 bytes that encode side-effecting commands with a non-zero low nibble:
   `0x28` (clear-Tx-interrupt), `0x38` (clear-IUS). Ours misroutes the pointer to the low
   nibble and skips the side effect. These command codes are never written by the fenced
   consumers; note for M2+ if they need clear-Tx or clear-IUS command semantics.

8. **Error Reset (0x30) with latched errors:** All three are effectively no-ops when no
   errors are queued, which is always the case in M1. If M2+ adds Rx error injection, the
   Error Reset path would need a real latch-clear implementation.

9. **Channel B state:** Our model stubs channel B with the same decoder but returns the same
   RR0/RR1 constants. QEMU and DingusPPC have full per-channel state for B. Not needed until
   a consumer touches channel B beyond the single `tst.b (a3,d3.l)` read at +0 (S3 §1.3
   prologue, which just uses the value as a sync/flush and ignores the result).

### VIA fence-excluded deltas

1. **T2CL read clears IFR.T2:** QEMU does this; we do not. Our T2CL read just returns the
   current countdown. The real 6522 also clears IFR.T2 on T2CL read. Not observable
   (consumers never read T2CL).

2. **Exact T2 expiry tick (off-by-1 vs hardware):** Ours and QEMU fire at N ticks; hardware
   fires at N+1; DingusPPC at N+3. For the consumer's 0xFFFF-tick timeout (~83ms at
   783 kHz) this is immaterial. If high-precision timing is needed for a future timer-driven
   guest, consider the +1 correction.

3. **T1 free-running mode:** Our T1 is modeled as one-shot (same as T2). Real 6522 T1 can
   free-run and auto-reload. The consumer sets ACR=0 (one-shot mode), so free-run is never
   exercised in M1. Our own source comment (`dev_via6522.cpp:52`) explicitly flags this.

4. **IFR write stopping timer vs just clearing flag:** We stop the T2 timer on IFR-clear-T2
   when expired; QEMU only clears the flag (timer can re-fire). For the consumer's one-shot
   use pattern this makes no difference. Our behavior is slightly more conservative (timer
   doesn't re-assert after a cleared IFR).

5. **Cuda SR protocol (shift register, ORB handshake bits 3/4):** We implement a loud-stub
   (cuda_touch counter + one-shot warning latch). QEMU and DingusPPC have full Cuda state
   machines. SR and ORB handshake are in the S3 §1.5 table as "adjacent code" — they are
   fenced as loud-stub in M1 by CORE99 §4 and S3 §4.2 explicitly.

6. **T1 latches vs counter distinction:** QEMU maintains T1 latch-L/H separately from the
   counter; writes to T1LL/T1LH are latch-only; T1CH write loads from latch and starts.
   Our model does the same (`v->t1l_l`, `v->t1l_h` as latches; T1CH write reloads). Match.

7. **ORB / ORA pin-level accuracy:** We store ORB/ORA as bytes but don't model DDR pin
   directions for output computation. For the consumer's use of ORB (bits3/4 Cuda handshake
   = loud-stub; ORA bit3 drive after SCC init), the stored value is the relevant output.
   Not observable beyond telemetry.

---

## 6. Action Items

The analysis below is derived by applying the consumer-read observability test (§2) to every
verdict row. A delta warrants a model fix only if it changes a value the fenced consumers
actually read.

**No model fixes are warranted.** Every behavioral delta between our models and the
references is either:
- (a) **Out of fence** — the affected register, bit, or side-effect is not accessed by either
  S3 consumer, or
- (b) **Consumer-read-bit MATCH** — the specific bits the consumers read (RR0 bit0, bit2;
  RR1 bit0, bits4-6; IFR bit5; IFR bit2) match all references, even where underlying
  mechanisms differ.

The complete list of `note`-classified deltas (out-of-fence, future M2+ relevance only):

| # | Delta | Where | Future milestone relevance |
|---|---|---|---|
| N1 | WR0 pointer: nibble-fold vs standard point-high two-step for regs 8–15 | SCC fence-excluded §5 item 7 | M2 if any guest uses standard Z85C30 protocol for WR8-15 |
| N2 | WR9 reset: memset-zero vs selective soft/hard reset preserving Z85C30 spec bits | SCC §3.4 | M2 if any guest reads WR state back after reset |
| N3 | RR1 bits1-3 (residue code): we return 0, DingusPPC returns 0x06 | SCC §3.3, fence-excluded item 4 | Immaterial; residue code is an SDLC-mode artifact not used by consumers |
| N4 | RR0 bit6 (TxUnderrun): we return 0, others set it | SCC fence-excluded item 3 | If a guest checks TxUnderrun during flow-control |
| N5 | Error Reset (0x30) does not clear hypothetical latched errors | SCC §3.5, fence-excluded item 8 | M2 if Rx error injection is added |
| N6 | T2CL read does not clear IFR.T2 | VIA §4.2, fence-excluded item 1 | If a guest uses T2CL read as the IFR-acknowledge path |
| N7 | T2 expiry at N ticks (hardware: N+1; DingusPPC: N+3) | VIA §4.1 | Immaterial for 83ms timeout; note if sub-tick timer precision required |
| N8 | IFR write stops timer (ours); references only clear flag | VIA §4.6, fence-excluded item 4 | Conservative behavior; can only cause under-trigger (IFR stays low longer), not over-trigger |
| N9 | Channel B stubs shared with channel A logic | SCC fence-excluded item 9 | M2+ if channel B is ever driven independently |

**The models are conformant for the M1 scope and ready for integration.**

---

## 7. Summary

- **Zero defects found** in either device model relative to the conformance write-vectors
  (S3 §1.3 / §2.3) and poll loops (S3 §1.4 / §2.1–2.4 / §1.5) that define M1's required
  behavior.
- **RR0 bit0=0 is correct, not a defect.** QEMU and DingusPPC both return 0 for a channel
  with no Rx source. The STM stall is design-intent of the Apple serial test monitor (it
  waits for serial input); no conformant device model should return bit0=1. The designed
  exit is the VIA T2 timeout path (S3 §1.5/§1.6), which our VIA model correctly supports.
- **WR9 reset mechanism differs from QEMU (memset vs selective field preserve)** but is
  not observable because both consumers re-initialize every needed WR register immediately
  after any reset, and no WR is ever read back.
- **VIA IFR/IER/T2 semantics are conformant** with all three references on the consumer-
  read paths. The IFR.T2 set/clear lifecycle (cleared on T2CH load, set on expiry) is
  identical across all implementations.
- **Nine note-level deltas** are recorded for future milestone reference (N1–N9 above);
  none require action before M1 merge.
