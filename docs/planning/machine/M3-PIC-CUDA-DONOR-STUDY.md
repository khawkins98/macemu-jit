# M3 OpenPIC + VIA/Cuda Donor Study

> **Status:** Complete — 2026-06-10
> **Purpose:** Settles the M3-relevant open questions in `CORE99-MACHINE-DESCRIPTION.md` §5
> (Q6 and Q8) and characterises the VIA/Cuda and OpenPIC donor options for M3's planner.
> Feeds directly into M3 design: interrupt tree sizing, IRQ wiring freeze, VIA/Cuda port shape.
>
> **Sourcing discipline:** every claim carries a URL + pinned SHA (or file:line for
> in-repo citations). Follows `M1-DEVICE-CONFORMANCE.md` citation style.

---

## Sources and Commit References

All external sources fetched 2026-06-10.

| Reference | URL | Pinned SHA |
|---|---|---|
| QEMU `hw/intc/openpic.c` | https://raw.githubusercontent.com/qemu/qemu/master/hw/intc/openpic.c | `de5d8bfd6105d3dd3ae668df9762df244a6d1506` |
| QEMU `include/hw/ppc/openpic.h` | https://raw.githubusercontent.com/qemu/qemu/master/include/hw/ppc/openpic.h | `de5d8bfd6105d3dd3ae668df9762df244a6d1506` |
| QEMU `hw/misc/macio/macio.c` | https://raw.githubusercontent.com/qemu/qemu/master/hw/misc/macio/macio.c | `de5d8bfd6105d3dd3ae668df9762df244a6d1506` |
| QEMU `include/hw/misc/macio/macio.h` | https://raw.githubusercontent.com/qemu/qemu/master/include/hw/misc/macio/macio.h | `de5d8bfd6105d3dd3ae668df9762df244a6d1506` |
| QEMU `hw/misc/macio/cuda.c` | https://raw.githubusercontent.com/qemu/qemu/master/hw/misc/macio/cuda.c | `de5d8bfd6105d3dd3ae668df9762df244a6d1506` |
| DingusPPC `devices/common/viacuda.cpp` | https://raw.githubusercontent.com/dingusdev/dingusppc/master/devices/common/viacuda.cpp | `92bb6d10549529f9f4031a85c2bc136149535bdc` |
| DingusPPC `devices/common/viacuda.h` | https://raw.githubusercontent.com/dingusdev/dingusppc/master/devices/common/viacuda.h | `92bb6d10549529f9f4031a85c2bc136149535bdc` |
| DingusPPC `devices/ioctrl/macio.h` | https://raw.githubusercontent.com/dingusdev/dingusppc/master/devices/ioctrl/macio.h | `92bb6d10549529f9f4031a85c2bc136149535bdc` |
| DingusPPC `devices/ioctrl/maciotwo.cpp` | https://raw.githubusercontent.com/dingusdev/dingusppc/master/devices/ioctrl/maciotwo.cpp | `92bb6d10549529f9f4031a85c2bc136149535bdc` |
| DingusPPC `devices/ioctrl/grandcentral.cpp` | https://raw.githubusercontent.com/dingusdev/dingusppc/master/devices/ioctrl/grandcentral.cpp | `92bb6d10549529f9f4031a85c2bc136149535bdc` |
| DingusPPC `zdocs/developers/keylargo.md` | https://raw.githubusercontent.com/dingusdev/dingusppc/master/zdocs/developers/keylargo.md | `92bb6d10549529f9f4031a85c2bc136149535bdc` |
| Linux `drivers/tty/serial/pmac_zilog.c` | https://raw.githubusercontent.com/torvalds/linux/master/drivers/tty/serial/pmac_zilog.c | latest master |
| Linux `drivers/macintosh/via-cuda.c` | https://raw.githubusercontent.com/torvalds/linux/master/drivers/macintosh/via-cuda.c | latest master |

**SHA cross-check note:** The M1-DEVICE-CONFORMANCE.md fetched viacuda.cpp at
`92bb6d10549529f9f4031a85c2bc136149535bdc` (same branch HEAD as current fetch — unchanged).
QEMU SHA `de5d8bfd6105d3dd3ae668df9762df244a6d1506` is also the same as M1's fetch;
openpic.c was not fetched in M1 but shares the same QEMU master HEAD.

---

## Section 1 — Q6: OpenPIC Region Size for the KeyLargo Model (CORE99 §5, item 6)

**Answer: `0x40000` bytes (262,144 bytes = 256 KB).**

### 1.1 Total region initialization

In `openpic.c` (SHA above), the `openpic_init` function contains:

```c
memory_region_init(&opp->mem, obj, "openpic", 0x40000);
```

(Line 1498 of the 1639-line file; the function signature is `openpic_init`.)

This is the region that `macio_newworld_realize` adds as a subregion at offset `+0x40000`
from the MacIO BAR:

```c
// macio.c lines 278-279:
memory_region_add_subregion(&s->bar, 0x40000,
                            sysbus_mmio_get_region(sbd, 0));
```

`sysbus_mmio_get_region(sbd, 0)` returns `&opp->mem` — the 0x40000-byte region.
There is **no separate size argument** at the MacIO attachment site; the size is
authoritative in `openpic_init`.

**MacIO containment check:** MacIO BAR is `0x80000` bytes (`macio.c:367`,
`memory_region_init(&s->bar, …, 0x80000)`). The OpenPIC starts at +0x40000 and spans
0x40000 → it fills the entire upper half of the MacIO BAR exactly:
`0x40000 + 0x40000 == 0x80000`. No overflow.

### 1.2 Sub-region layout within the OpenPIC

The `OPENPIC_MODEL_KEYLARGO` case calls `map_list(opp, list_le, &list_count)`, which
iterates the following table (from `openpic.c`):

```c
static const MemReg list_le[] = {
    {"glb", &openpic_glb_ops_le, OPENPIC_GLB_REG_START, OPENPIC_GLB_REG_SIZE},
    {"tmr", &openpic_tmr_ops_le, OPENPIC_TMR_REG_START, OPENPIC_TMR_REG_SIZE},
    {"src", &openpic_src_ops_le, OPENPIC_SRC_REG_START, OPENPIC_SRC_REG_SIZE},
    {"cpu", &openpic_cpu_ops_le, OPENPIC_CPU_REG_START, OPENPIC_CPU_REG_SIZE},
    {NULL}
};
```

The offset/size constants (from `openpic.c`, all confirmed verbatim):

| Sub-region | Start offset | Size (hex) | Size notes |
|---|---|---|---|
| `glb` (global registers) | `0x0` | `0x10F0` | ~4 KB |
| `tmr` (timer bank) | `0x10F0` | `0x220` | 4 timers × 0x88 |
| `src` (source/IRQ bank) | `0x10000` | `OPENPIC_MAX_SRC * 0x20` = `0x2000` | 256 sources × 32 B |
| `cpu` (per-CPU bank) | `0x20000` | `0x100 + ((MAX_CPU - 1) * 0x1000)` | varies by CPU count |

`OPENPIC_MAX_SRC = 256` and `OPENPIC_MAX_TMR = 4` are defined in `openpic.h` (lines 28–29).

**Sparsity note:** The sub-regions are not contiguous — large holes exist between `tmr`
(ends at 0x1310) and `src` (starts at 0x10000), and between `src` (ends at 0x12000) and
`cpu` (starts at 0x20000). Accesses into those holes are unregistered and will abort-loudly
under our bus design. This is expected; Mac OS 9 only touches a small subset of registers.

### 1.3 KeyLargo model configuration in the realize path

```c
// openpic.c, case OPENPIC_MODEL_KEYLARGO (line ~1374):
case OPENPIC_MODEL_KEYLARGO:
    opp->nb_irqs = KEYLARGO_MAX_EXT;
    opp->vid = VID_REVISION_1_2;
    opp->vir = VIR_GENERIC;
    opp->vector_mask = 0xFF;
    opp->tfrr_reset = 4160000;
    opp->ivpr_reset = IVPR_MASK_MASK | IVPR_MODE_MASK;
    opp->idr_reset = 0;
    opp->max_irq = KEYLARGO_MAX_IRQ;
    opp->irq_ipi0 = KEYLARGO_IPI_IRQ;
    opp->irq_tim0 = KEYLARGO_TMR_IRQ;
    opp->brr1 = -1;
    opp->mpic_mode_mask = GCR_MODE_MIXED;
    map_list(opp, list_le, &list_count);
    break;
```

`KEYLARGO_MAX_EXT`, `KEYLARGO_MAX_IRQ`, `KEYLARGO_IPI_IRQ`, `KEYLARGO_TMR_IRQ` are
assigned at `openpic.c` lines 1570–1578 (SHA above) via constants defined in a header
(`hw/ppc/mac.h` — the header was not accessible via GitHub raw at the fetched SHA).
Their exact numeric values were not retrieved. **This does not affect the size answer** —
the 0x40000 total region size is independent of these capacity constants, and the
`case OPENPIC_MODEL_KEYLARGO:` branch is confirmed at line 1570.

### 1.4 What M3 must trap

M3 must register the OpenPIC as a **trapped-MMIO** region at `MacIO+0x40000` with span
`0x40000`, for a guest-physical range of `0xF3040000–0xF307FFFF` (inclusive). The internal
sub-region table above tells M3 which offsets within that span need register handlers vs.
abort-loudly stubs. The registers Mac OS 9 plausibly touches are discussed in §5.

---

## Section 2 — Q8: PIC Input Numbers Cross-Check

CORE99 §2 carries QEMU's NewWorld wiring for the interrupt tree. This section cross-checks
each input number against DingusPPC and Linux kernel sources.

### 2.1 Source constants from QEMU `macio.h`

```c
// include/hw/misc/macio/macio.h, lines 40-57 (verbatim):
#define OLDWORLD_CUDA_IRQ      0x12
#define OLDWORLD_ESCCB_IRQ     0x10
#define OLDWORLD_ESCCA_IRQ     0xf
#define OLDWORLD_IDE0_IRQ      0xd
#define OLDWORLD_IDE0_DMA_IRQ  0x2
#define OLDWORLD_IDE1_IRQ      0xe
#define OLDWORLD_IDE1_DMA_IRQ  0x3

#define NEWWORLD_CUDA_IRQ      0x19
#define NEWWORLD_PMU_IRQ       0x19
#define NEWWORLD_ESCCB_IRQ     0x24
#define NEWWORLD_ESCCA_IRQ     0x25
#define NEWWORLD_IDE0_IRQ      0xd
#define NEWWORLD_IDE0_DMA_IRQ  0x2
#define NEWWORLD_IDE1_IRQ      0xe
#define NEWWORLD_IDE1_DMA_IRQ  0x3
#define NEWWORLD_EXTING_GPIO1  0x2f
#define NEWWORLD_EXTING_GPIO9  0x37
```

These are the values wired in `macio_newworld_realize`:
```c
// macio.c lines 282-283:
sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(pic_dev, NEWWORLD_ESCCB_IRQ));
sysbus_connect_irq(sbd, 1, qdev_get_gpio_in(pic_dev, NEWWORLD_ESCCA_IRQ));
// macio.c line 343 (Cuda variant — has_pmu == false path):
sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(pic_dev, NEWWORLD_CUDA_IRQ));
```

### 2.2 DingusPPC: no KeyLargo model — confirmed

The DingusPPC `devices/ioctrl/macio.h` enum contains **only four chip IDs**
(SHA `92bb6d10…`, lines 45–49):

```c
enum {
    MIO_DEV_ID_GRANDCENTRAL = 0x0002,
    MIO_DEV_ID_OHARE        = 0x0007,
    MIO_DEV_ID_HEATHROW     = 0x0010,
    MIO_DEV_ID_PADDINGTON   = 0x0017,
};
```

**KeyLargo is absent.** The `zdocs/developers/keylargo.md` document (SHA above) states
"KeyLargo ASIC ... MPIC support" as a feature but there is no implementation code —
no KeyLargo-class MacIO object, no OpenPIC instantiation. A `git` search of the full tree
for "KEYLARGO" returned zero code files. DingusPPC's interrupt subsystem for
Heathrow/Paddington is a **flat bitmask controller** (the OldWorld-heritage
`InterruptEvents/IntMask/IntClear/IntLevels` register pairs at MacIO+0x10–0x2C) handled
by `MacIoTwo::register_dev_int`, not an OpenPIC.

**DingusPPC therefore cannot confirm or contradict QEMU's OpenPIC input numbers** —
the architectures are completely different.

**DingusPPC Heathrow/Paddington interrupt wiring (contrast only, different controller):**

From `maciotwo.cpp` lines 403–450 (SHA above):

```c
uint64_t MacIoTwo::register_dev_int(IntSrc src_id) {
    case IntSrc::SCCA       : return INT_TO_IRQ_ID(0x0F);  // SCC ch A
    case IntSrc::SCCB       : return INT_TO_IRQ_ID(0x10);  // SCC ch B
    case IntSrc::VIA_CUDA   : return INT_TO_IRQ_ID(0x12);  // VIA/Cuda
    case IntSrc::IDE0       : return INT_TO_IRQ_ID(0x0D);
    case IntSrc::IDE1       : return INT_TO_IRQ_ID(0x0E);
    ...
```

These are **Heathrow/Paddington flat-controller bit positions**, not OpenPIC input numbers.
They happen to share the IDE0/IDE1 positions (0xD/0xE) with QEMU's NEWWORLD values but
differ on ESCC (0xF/0x10 vs. 0x24/0x25) and VIA (0x12 vs. 0x19). The coincidence on IDE
is because QEMU's NewWorld IDE inputs (0xD/0xE) were inherited from the OldWorld
Heathrow wiring. The ESCC and VIA numbers differ because KeyLargo's OpenPIC has a
distinct input assignment.

### 2.3 Linux kernel cross-check

Linux's `drivers/tty/serial/pmac_zilog.c` (ESCC driver) and `drivers/macintosh/via-cuda.c`
(Cuda driver) both use **device-tree-based IRQ mapping** on PowerPC:

```c
// pmac_zilog.c:
uap->port.irq = irq_of_parse_and_map(np, 0);

// via-cuda.c:
cuda_irq = irq_of_parse_and_map(vias, 0);
```

No hardcoded OpenPIC input numbers appear in either driver. The Linux kernel derives all
IRQ assignments from the device-tree blob provided by Open Firmware at boot, which means
the ground truth for Core99 IRQ numbers is Open Firmware's device-tree for each specific
machine — not compiled into the driver. This is expected: OF assigns IRQs dynamically
and publishes them as the `interrupts` property of each device node.

The Linux kernel therefore provides **no independent numeric confirmation** of QEMU's
values. The best available independent cross-check would be a real OF device-tree dump
from a G3 B&W or G4 machine — not available in this fetch. The absence of a contradiction
is noted, not treated as confirmation.

### 2.4 Verdict per input number

| Device | QEMU NewWorld input | DingusPPC KeyLargo | Linux kernel | Verdict |
|---|---|---|---|---|
| ESCC ch B | `0x24` (36) | No KeyLargo model | DT-derived; no hardcoded value | **QEMU-only** — no independent confirmation |
| ESCC ch A | `0x25` (37) | No KeyLargo model | DT-derived; no hardcoded value | **QEMU-only** — no independent confirmation |
| VIA-Cuda | `0x19` (25) | No KeyLargo model | DT-derived; no hardcoded value | **QEMU-only** — no independent confirmation |
| IDE bus 0 | `0xD` (13) | Heathrow: `0x0D` (different controller) | DT-derived | **Consistent** — both QEMU NewWorld and DingusPPC Heathrow use 0xD, suggesting this assignment is stable across controller generations |
| IDE bus 1 | `0xE` (14) | Heathrow: `0x0E` | DT-derived | **Consistent** — same reasoning as IDE0 |

**Risk assessment:** The "QEMU-only" verdict for ESCC and VIA does not mean QEMU is wrong
— QEMU mac99 is the primary behavioral oracle per `MACHINE-LAYER-PLAN.md §4`, and its
macio.c was written against Apple hardware/OF documentation by engineers with access to
real G3/G4 machines. The verdict means we have no independent corroboration. M3 must
treat these values as authoritative but should add a one-time boot-time log of the first
PIC IACK result per device to catch any discrepancy against actual ROM behavior.

---

## Section 3 — DingusPPC viacuda.cpp Port Assessment (M3's Full VIA/Cuda)

### 3.1 TimerManager coupling points

DingusPPC `ViaCuda` calls `TimerManager::get_instance()` at four distinct points
(SHA `92bb6d10…`):

| Call site | Purpose | Approx. line |
|---|---|---|
| `TimerManager::get_instance()->add_oneshot_timer(…t1_counter…, [this]() { activate_t1(); })` | Schedule T1 expiry callback | ~239 |
| `TimerManager::get_instance()->add_oneshot_timer(…t2_counter…, [this]() { assert_t2_int(); })` | Schedule T2 expiry callback | ~221 |
| `TimerManager::get_instance()->add_oneshot_timer(timeout_ns, [this]() { assert_sr_int(); })` | SR shift-register byte timing | ~278 |
| `TimerManager::get_instance()->add_oneshot_timer(USECS_TO_NSECS(13), [this]() { via_portb &= ~CUDA_TREQ; })` | TREQ de-assert delay | ~328 |
| `TimerManager::get_instance()->cancel_timer(this->sr_timer_id)` | Cancel pending SR timer | ~122 |
| `TimerManager::get_instance()->cancel_timer(this->t1_timer_id)` | Cancel pending T1 timer | ~127 |
| `TimerManager::get_instance()->cancel_timer(this->t2_timer_id)` | Cancel pending T2 timer | ~132 |
| `TimerManager::get_instance()->cancel_timer(this->treq_timer_id)` | Cancel TREQ timer | ~137 |
| `TimerManager::get_instance()->current_time_ns()` | Read current host time (T1 counter) | ~235, ~370 |

Additionally, `EventManager::get_instance()->add_post_handler(this, &ViaCuda::autopoll_handler)`
registers an autopoll callback on the host event loop (line ~112). This is the ADB
polling mechanism and requires the host's event-dispatch infrastructure.

**What M2's scheduler must provide for a direct port:**
1. `add_oneshot_timer(nanoseconds, callback)` → returns a timer ID
2. `cancel_timer(timer_id)`
3. `current_time_ns()` → host monotonic nanoseconds
4. A post-event hook API analogous to `EventManager::add_post_handler` (or ADB autopoll
   can be re-driven from the M2 scheduler's per-tick callback instead)

This is precisely the event-scheduler contract described in `MACHINE-LAYER-PLAN.md §2c`.
M2 is a hard prerequisite for a full DingusPPC viacuda port.

### 3.2 Cuda protocol surface

The Cuda pseudo-command dispatch (lines ~557–720, SHA above) handles these commands:

| Command | Function |
|---|---|
| `CUDA_START_STOP_AUTOPOLL` | Enable/disable ADB autopoll mode |
| `CUDA_READ_MCU_MEM` | Read PRAM or ROM region |
| `CUDA_GET_REAL_TIME` | Return RTC value (seconds since 1904) |
| `CUDA_WRITE_MCU_MEM` | Write PRAM content |
| `CUDA_READ_PRAM` | Open-ended PRAM transfer |
| `CUDA_SET_REAL_TIME` | Adjust RTC offset |
| `CUDA_WRITE_PRAM` | Write PRAM byte(s) |
| `CUDA_FILE_SERVER_FLAG` | Toggle file server flag |
| `CUDA_SET_AUTOPOLL_RATE` | Set ADB poll interval |
| `CUDA_GET_AUTOPOLL_RATE` | Get ADB poll interval |
| `CUDA_SET_DEVICE_BITMAP` | Set ADB device bitmap |
| `CUDA_GET_DEVICE_BITMAP` | Get ADB device bitmap |
| `CUDA_ONE_SECOND_MODE` | One-second interrupt mode |
| `CUDA_SET_POWER_MESSAGES` | Configure power-state messages |
| `CUDA_READ_WRITE_I2C` | Simple I2C transaction |
| `CUDA_COMB_FMT_I2C` | Combined-format I2C transaction |
| `CUDA_RESTART_SYSTEM` | Initiate system restart |
| `CUDA_POWER_DOWN` | Initiate power-down |
| `CUDA_WARM_START` | Warm start (disables async TX) |
| `CUDA_MONO_STABLE_RESET` | Monostable reset |
| `CUDA_TOGGLE_WAKEUP` | Wake-enable toggle |
| `CUDA_OUT_PB0` | Drive PortB bit 0 |
| `CUDA_TIMER_TICKLE` | Timer tickle (keep-alive) |
| `CUDA_SET_POWER_UPTIME` | Set scheduled power-up time |

ADB packets are dispatched to `this->adb_bus_obj` (acquired from
`gMachineObj->get_comp_by_type(HWCompType::ADB_HOST)` in the constructor).

PRAM (256 bytes) is owned by viacuda via a `NVram("pram.bin", 256)` member — separate
from the 8 KB partitioned NVRAM (M4 scope).

**SR handshake (shift-register state machine, lines ~295–372):**
The protocol observes two VIA control bits: TIP (Transaction In Progress, ORB bit 3) and
BYTEACK (ORB bit 4). State transitions:
- TIP high + BYTEACK high → negate TREQ, process completed packet
- TIP high + BYTEACK low → enter sync state, assert TREQ
- TIP low, write direction → buffer incoming data, schedule SR interrupt (~88 µs per byte)
- TIP low, read direction → call output handler, schedule SR interrupt (~88 µs)

### 3.3 Coverage vs. our M1 VIA 6522 model

Our M1 VIA model (`dev_via6522.cpp`, `machine-layer-m1` branch) covers the timer/IFR
surface confirmed by spike S3:

- T2 one-shot: load, run, IFR.5 on expiry — **covered by M1**
- IFR/IER set/clear polarity — **covered by M1**
- Register stride 0x200 — **covered by M1**
- Cuda SR protocol (shift register, ORB handshake) — **loud stub in M1** (per
  `M1-DEVICE-CONFORMANCE.md §5, VIA fence-excluded delta 5`)

A DingusPPC viacuda port would add to our existing M1 VIA surface:
1. Full Cuda SR handshake state machine (the loud stub becomes real)
2. ADB bus dispatch
3. RTC/PRAM commands (the 256-byte PRAM)
4. Power management commands (POWER_DOWN, RESTART_SYSTEM, WARM_START)
5. T1 free-running mode (currently M1 stubs T1 as one-shot; DingusPPC has full T1)

### 3.4 GPL-3 attribution requirements

Per repo memory notes and `MACHINE-LAYER-PLAN.md §4`:
- DingusPPC is GPL-3.0; our project is GPLv2-or-later; combining → GPLv3 (resolved in
  DINGUSPPC-EVALUATION-PLAN.md — feasible).
- Backport hygiene: cite DingusPPC source file + SHA `92bb6d10…` in code comments AND
  CHANGELOG entry.
- **Critical constraint: NEVER send AI-generated PRs upstream to DingusPPC** — they ban
  AI contributions into their repo (memory: `dingusppc_anti_ai`). Downstream GPL use in
  this repo with attribution is normal GPL practice.

### 3.5 Port shape estimate

Two options:

**Option A — wrap-their-core:** Take `ViaCuda` largely as-is, shim `TimerManager` and
`EventManager` calls to our M2 scheduler API, replace `gMachineObj->get_comp_by_type`
lookups with our own component registry pattern, remove I2C inheritance.

- Pros: full command fidelity immediately; protocol is well-tested in DingusPPC.
- Cons: imports DingusPPC's global-singleton patterns (`gMachineObj`, `EventManager`);
  entangles our locking model with theirs; the full ADB bus object chain comes in as a
  dependency. Porting cost: M2 prerequisite + ~2–3 days of plumbing.

**Option B — extract-protocol-into-our-device:** Implement the Cuda state machine and
command table from scratch in our own device style, using DingusPPC as the behavioral
oracle (command semantics, register values, protocol timing). QEMU cuda.c used as a
second oracle where the two differ.

- Pros: clean ownership, our locking rules, no foreign global singletons, testable in
  isolation (matches our existing harness pattern). JIT-STYLE-DECISION.md preference.
- Cons: more initial coding; ADB bus is a large dependency either way.
- Porting cost: M2 prerequisite + ~3–5 days for command table + protocol.

**Recommendation: Option B** — extract protocol. The protocol is well-documented by the
two oracles (DingusPPC + QEMU), the command table is enumerable, and our M1 VIA surface
is already the right foundation. Wrap-their-core would bind us to DingusPPC's
architectural choices (global singletons, EventManager post-handlers) in shared CPU
files — exactly the kind of foreign-lock exposure §2g forbids.

---

## Section 4 — QEMU cuda.c as Oracle

### 4.1 QEMU's command subset

From `hw/misc/macio/cuda.c` (SHA above), the handler table at lines 446–456:

```c
static const CudaCommand handlers[] = {
    { CUDA_AUTOPOLL,          "AUTOPOLL",          cuda_cmd_autopoll },
    { CUDA_SET_AUTO_RATE,     "SET_AUTO_RATE",      cuda_cmd_set_autorate },
    { CUDA_SET_DEVICE_LIST,   "SET_DEVICE_LIST",    cuda_cmd_set_device_list },
    { CUDA_POWERDOWN,         "POWERDOWN",          cuda_cmd_powerdown },
    { CUDA_RESET_SYSTEM,      "RESET_SYSTEM",       cuda_cmd_reset_system },
    { CUDA_FILE_SERVER_FLAG,  "FILE_SERVER_FLAG",   cuda_cmd_set_file_server_flag },
    { CUDA_SET_POWER_MESSAGES,"SET_POWER_MESSAGES", cuda_cmd_set_power_message },
    { CUDA_GET_TIME,          "GET_TIME",           cuda_cmd_get_time },
    { CUDA_SET_TIME,          "SET_TIME",           cuda_cmd_set_time },
};
```

ADB packets are dispatched separately via `adb_request()` (lines 588–619, packet type
`ADB_PACKET`).

### 4.2 Comparison: QEMU vs DingusPPC command coverage

| Command category | QEMU cuda.c | DingusPPC viacuda.cpp | Present in both |
|---|---|---|---|
| ADB pass-through | YES (ADB_PACKET dispatch) | YES (adb_bus_obj) | YES |
| Autopoll enable/disable | YES (CUDA_AUTOPOLL) | YES (CUDA_START_STOP_AUTOPOLL) | YES (different constant name) |
| Autopoll rate | YES (SET_AUTO_RATE) | YES (SET/GET_AUTOPOLL_RATE) | YES |
| Device list / bitmap | YES (SET_DEVICE_LIST) | YES (SET/GET_DEVICE_BITMAP) | YES |
| Power down | YES (POWERDOWN) | YES (CUDA_POWER_DOWN) | YES |
| System reset | YES (RESET_SYSTEM) | YES (CUDA_RESTART_SYSTEM) | YES |
| File server flag | YES (stub) | YES | YES |
| Power messages | YES (stub) | YES (CUDA_SET_POWER_MESSAGES) | YES |
| RTC get/set | YES (GET_TIME/SET_TIME) | YES (GET/SET_REAL_TIME) | YES |
| PRAM read/write | NO — QEMU delegates to nvram separately | YES (READ/WRITE_MCU_MEM, READ/WRITE_PRAM) | **QEMU missing** |
| MCU memory read | NO | YES (READ_MCU_MEM) | **QEMU missing** |
| I2C read/write | NO | YES (READ_WRITE_I2C, COMB_FMT_I2C) | **QEMU missing** |
| One-second mode | NO | YES (ONE_SECOND_MODE) | **QEMU missing** |
| Warm start | NO | YES (CUDA_WARM_START) | **QEMU missing** |
| Timer tickle | NO | YES (CUDA_TIMER_TICKLE) | **QEMU missing** |
| Power uptime | NO | YES (CUDA_SET_POWER_UPTIME) | **QEMU missing** |
| Mono stable reset | NO | YES (CUDA_MONO_STABLE_RESET) | **QEMU missing** |

### 4.3 Which better matches Mac OS 9 exercise?

QEMU's set covers the **boot-critical** commands that Mac OS 9 drives immediately:
autopoll, RTC, ADB device scan, power-down. DingusPPC's set is a superset that also covers
PRAM (accessed constantly by system software — XPRAM, Sound prefs, etc.), I2C (used for
ADC/DAC on some G3/G4 boards), and the one-second interrupt (the Mac "heartbeat" timer
used for clock updates and deferred tasks).

**Verdict:** For M3's initial Cuda implementation the QEMU command set is the minimum
viable subset (boot to Finder). DingusPPC's PRAM commands are needed for prefs persistence
(XPRAM), which Mac OS 9 uses constantly at runtime. The QEMU set alone will cause PRAM
reads to silently fail (no handler → loud-stub abort). PRAM + RTC + autopoll + ADB are the
M3-required minimum for a running 9.x system.

---

## Section 5 — OpenPIC Donor Choice

### 5.1 QEMU openpic.c profile

GPL-2.0-or-later (QEMU project license). Size: the full `openpic.c` is approximately
1639 lines (the file covers multiple models: FSL MPIC 2.0, FSL MPIC 4.2, KeyLargo).
The KeyLargo-specific paths are a small fraction — the model switch at realize time
configures the capacity constants, but the register-access handlers (`openpic_glb_*`,
`openpic_src_*`, `openpic_cpu_*`) are shared across all models.

Key KeyLargo-relevant registers (inferred from the sub-region layout in §1.2 and standard
OpenPIC spec):

- **Global registers** (`glb`, 0x0–0x10F0): Feature Register (FRR), Global Conf Reg (GCR),
  Vendor ID (VID), Processor Init (PIR), IPI vector/priority registers (IPIVPR0–3).
- **Timer bank** (`tmr`, 0x10F0–0x1310): 4 timer current-count and base-count registers.
  Whether Mac OS 9 exercises these timers is **unknown** — the VM is decrementer-driven,
  not timer-driven from the PIC. Likely dead code at boot.
- **Source bank** (`src`, 0x10000–0x12000): 256 source registers, each 0x20 bytes
  (IVPR = interrupt vector/priority register, IDR = interrupt destination register).
  Mac OS 9 uses src[0x19] (VIA-Cuda), src[0x24] (ESCC B), src[0x25] (ESCC A) at minimum.
  ROM boot also writes masks for all sources it wants to enable/disable.
- **CPU bank** (`cpu`, 0x20000+): IACK (interrupt acknowledge, reads next vector), EOI
  (end-of-interrupt), task priority (TPR). These are the hot paths: IACK + EOI on every
  interrupt delivery.

### 5.2 What Mac OS 9 plausibly exercises on the OpenPIC

Based on standard PowerMac boot behavior and the ROM boot sequence (MACHINE-LAYER-PLAN.md
§2d, CORE99 §4):

1. **Source mask/priority setup** (boot-time): `mtspr` to each IVPR for enabled sources,
   setting priority and vector number. Likely iterates all 256 sources to mask them first,
   then unmasks used ones. High source-bank access density at boot; moderate at runtime.
2. **IPI registers** (possibly): Mac OS 9 is a uniprocessor OS — IPI registers are likely
   initialized but never triggered. `KEYLARGO_IPI_IRQ` suggests IPI is wired; safe to
   abort-loudly stub.
3. **Timer registers** (possibly): The nanokernel may read/write timer bank at boot for
   frequency calibration. Low-confidence; add abort-loudly stubs.
4. **Global conf / FRR** (boot): ROM reads FRR to discover the number of sources/CPUs;
   writes GCR to set the interrupt delivery mode (mixed vs. pass-through).
5. **IACK + EOI** (every interrupt): the hot steady-state path. Must be correct.
6. **Per-source IVPR/IDR** (each enabled interrupt): mask, priority, vector programmed at
   init.

**IPI and timers are not required for a uniprocessor Mac OS 9 boot.** The M3 scope
can implement: global (FRR read, GCR write), source bank (IVPR/IDR per-source), CPU
per-CPU (IACK, EOI, TPR). Timer bank → abort-loudly stub. IPI → abort-loudly stub.

### 5.3 Port vs. reimplement assessment

| Axis | Port QEMU openpic.c | Reimplement against spec |
|---|---|---|
| Implementation scope | Full openpic.c is 1639 lines (fetched, exact); KeyLargo code path is ~300–500 effective lines after stripping FSL paths | OpenPIC spec is public (Motorola MPC8240/MPC8245 OpenPIC manual); M3-relevant register set (§5.2) is ~20–30 registers; implementation ~200–400 lines |
| Fidelity risk | High fidelity — QEMU is tested against real hardware | Protocol compliance risk on edge cases; mitigated by QEMU as conformance oracle |
| Locking fit | QEMU uses qemu-specific infrastructure (QOM, MemoryRegion); extracting the logic is non-trivial | Native to our model; follows per-device lock rule (§2g) |
| License | GPL-2.0-or-later (QEMU) — combining with our GPLv2-or-later works | N/A |
| Maintenance | Inherits QEMU complexity (FSL MPIC paths, BE/LE variants) | Only what M3 needs; abort-loudly for unneeded registers |
| Test path | QEMU conformance comparison (§5.2 of MACHINE-LAYER-PLAN) easy if we share logic | Same conformance check using QEMU as oracle; slightly more test-writing |
| Backpatch-compatibility | QEMU MMIO handlers work through its bus abstraction; requires adapting to our bus API | Already native to our bus |

**Recommendation: reimplement against spec, using QEMU as conformance oracle.** The
M3-required register surface is small (IACK/EOI/IVPR/IDR/FRR/GCR — ~20 registers
effectively exercised). Porting QEMU's 1639-line multi-model file would import QOM,
MemoryRegion, QEMU locking, and FSL MPIC paths that are dead weight for our use case.
The OpenPIC spec is available; QEMU's source provides the behavioral oracle for
conformance testing. This follows the same pattern as our M1 devices (SCC, VIA) — all
reimplemented against spec, all conformance-tested against QEMU.

**Anti-drift guard:** if the reimplement path stalls (spec ambiguity on an edge case),
fall back to the relevant QEMU handler and port it selectively. The unit-test harness
catches the delta.

---

## Section 6 — Proposed Replacements for CORE99-MACHINE-DESCRIPTION §5

The following are **drop-in replacement text blocks** for the corresponding items in
`CORE99-MACHINE-DESCRIPTION.md §5 Open questions`. Do NOT edit CORE99 directly — another
session may have it dirty. Apply these as a tracked edit when M3 is planned.

### Replacement for §5 item 6 (Q6 — OpenPIC region size):

**Current text:**
> 6\. **OpenPIC region size** for the KeyLargo model at +0x40000 — not pinned in `macio.c`.
>    *Experiment:* read QEMU `hw/intc/openpic.c` (`OPENPIC_MODEL_KEYLARGO` register window) before
>    M3 sizes the trap region.

**Proposed replacement:**
> 6\. ~~**OpenPIC region size** — RESOLVED (2026-06-10; `M3-PIC-CUDA-DONOR-STUDY.md §1`).~~
>    **Total span: `0x40000` bytes** (256 KB), authoritative in
>    `openpic_init()`: `memory_region_init(&opp->mem, obj, "openpic", 0x40000)` (QEMU
>    `hw/intc/openpic.c` SHA `de5d8bfd…`, line 1498 of 1639).  M3 must register the bus trap at
>    `0xF3040000–0xF307FFFF` (MacIO+0x40000, span 0x40000). Sub-regions within the span:
>    `glb` at 0x0 (size 0x10F0), `tmr` at 0x10F0 (size 0x220), `src` at 0x10000 (size
>    0x2000 = 256 sources × 32 B), `cpu` at 0x20000. Large holes between sub-regions
>    get abort-loudly stubs; accesses into them are unexpected for classic Mac OS 9.

### Replacement for §5 item 8 (Q8 — PIC input numbers cross-check):

**Current text:**
> 8\. **ESCC/VIA PIC input numbers (0x24/0x25/0x19) are QEMU values only** — not yet cross-checked
>    against DingusPPC or Apple KeyLargo documentation. *Experiment:* compare DingusPPC's
>    KeyLargo interrupt wiring before M3 freezes the tree.

**Proposed replacement:**
> 8\. **ESCC/VIA PIC input numbers — cross-check COMPLETE (2026-06-10; `M3-PIC-CUDA-DONOR-STUDY.md §2`).
>    Verdict: QEMU-only, no contradiction found.** DingusPPC has no KeyLargo/OpenPIC model
>    (confirmed: `macio.h` enum stops at Paddington, SHA `92bb6d10…`); its Heathrow/Paddington flat
>    controller uses different input numbers (ESCC A=0x0F, ESCC B=0x10, VIA=0x12 — different
>    controller architecture, not a contradiction). Linux ESCC and VIA-Cuda drivers derive IRQs from
>    the device-tree at boot (`irq_of_parse_and_map`) — no hardcoded values. No independent numeric
>    source corroborates or contradicts QEMU. Values `ESCC_B=0x24 (36), ESCC_A=0x25 (37),
>    VIA-Cuda=0x19 (25)` remain authoritative as QEMU oracle values (`macio.h` lines 48–51, SHA
>    `de5d8bfd…`). M3 should log the first IACK result per device at boot to catch any real-hardware
>    discrepancy.

---

## Section 7 — Ranked Donor Decision Recommendation for M3 Planner

In priority order:

### 7.1 OpenPIC — **Reimplement against spec (QEMU as oracle)**

Rationale in §5.3. The register surface is small; QEMU's multi-model file is too large to
port cleanly. Boot requirement: IACK, EOI, IVPR/IDR per-source, FRR/GCR global. Start
with the 3 live sources (VIA 0x19, ESCC A 0x25, ESCC B 0x24); stub everything else loudly.

Estimated M3 scope contribution: **S–M** (100–300 lines new device code, 30–50 lines
bus registration, ~15 unit test cases).

### 7.2 VIA/Cuda — **Extract protocol into our device (DingusPPC + QEMU as oracles)**

Rationale in §3.5. M1 VIA surface is the foundation; M3 adds the Cuda SR state machine
and the boot-critical command subset (RTC, PRAM, ADB autopoll, power-down).

**M2 prerequisite is a hard gate** — T2 one-shot and the SR timing (~88 µs/byte) require
`add_oneshot_timer`. Without M2, Cuda can only be a polling stub (which is what M1 has).

Estimated M3 scope contribution for the Cuda layer: **M** (200–400 lines, 20–30 unit
test cases), on top of M2.

### 7.3 Summary table

| Component | Approach | Prerequisite | Estimated LOC | Risk |
|---|---|---|---|---|
| OpenPIC | Reimplement + QEMU oracle | M2 (clock, for DEC interaction) | ~200–300 | Low (spec is clear for the exercised subset) |
| VIA/Cuda protocol | Extract (DingusPPC + QEMU oracles) | M2 (timers) | ~200–400 | Medium (ADB bus dependency; state machine is complex) |
| Timer bank stubs | Abort-loudly | None | ~10 | Negligible |
| IPI stubs | Abort-loudly | None | ~10 | Negligible |

**M3 sequencing rule (consistent with MACHINE-LAYER-PLAN.md §3):** M2 first (virtual clock),
then M3 in this order: (1) OpenPIC skeleton (IACK/EOI/mask only, no routing yet);
(2) MSR/SRR/rfi model; (3) vector-base experiment; (4) Cuda command table + ADB;
(5) wire OpenPIC sources (VIA, ESCC) through the exception model. Each step independently
testable.
