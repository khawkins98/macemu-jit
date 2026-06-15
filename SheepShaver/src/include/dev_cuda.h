/*
 *  dev_cuda.h - Cuda (Apple MCU) protocol state machine + command dispatcher
 *  (Machine Layer M3b Wave 1, Task 1).  Pure module: NOT an MMIO device — it sits
 *  behind the VIA 6522's shift-register/port-B surface via narrow seam functions
 *  (Task 3 binds it with VIABindCuda; this module has no VIA dependency).
 *
 *  Behavioral extraction (NOT a code port) from two oracles, per backport hygiene:
 *    - QEMU hw/misc/macio/cuda.c + include/hw/misc/macio/cuda.h
 *      @ de5d8bfd6105d3dd3ae668df9762df244a6d1506 (cuda_update() handshake/sync
 *      semantics, command table, packet/error framing, RTC offset model)
 *    - DingusPPC devices/common/viacuda.cpp + viacuda.h
 *      @ 92bb6d10549529f9f4031a85c2bc136149535bdc (PRAM/MCU_MEM commands,
 *      GET_AUTOPOLL_RATE/GET_DEVICE_BITMAP, error codes, end-of-transaction
 *      discard semantics)
 *  DingusPPC is GPL-3.0; importing behavior here with citation is fine — never
 *  PR anything upstream to them (repo memory: dingusppc_anti_ai).
 *
 *  Handshake lines (M3b plan rev 2, BINDING C3 — donor study §3.2 prose was wrong;
 *  ROM-verified + boot polarity probe 2026-06-11):
 *    ORB bit 3 = TREQ  (Cuda -> host, INPUT to host)   } all
 *    ORB bit 4 = TACK / BYTEACK (host output)          } active
 *    ORB bit 5 = TIP   (host output)                   } LOW
 *    Observed DDRB = 0x30 (bits 4/5 outputs, bit 3 input).
 *
 *  Seam contract (C1/C2/M4/M6):
 *    - The VIA derives ORB bit 3 from Cuda state on EVERY ORB read
 *      (CudaDeriveORB) — never echoes a stored bit 3 (the ROM does RMW on ORB).
 *    - Command processing is SYNCHRONOUS at packet commit (TIP negated with
 *      captured bytes): the response is queued and TREQ asserted before
 *      CudaORBWritten returns.
 *    - All seam calls run under the VIA's bus region lock; this module is
 *      lock-free and NEVER takes locks or calls locked_call (M6). "Raise/clear
 *      IFR.2" is communicated back via CUDA_SEAM_* return flags — the VIA owns
 *      the IFR.  Raise delivery is DEFERRED (CV-10): mutators latch
 *      sr_int_pending but return no raise; CudaSettle — called from the VIA's
 *      R_IFR read path ONLY — is the single delivery point, consume-once (no
 *      double-raise).  SR accesses clear the latched IFR.2 but never consume
 *      an undelivered pending raise.
 *    - §2g: no stdio, no malloc on any seam path (fault-thread reachable).
 *      Unknown commands latch a one-shot warning (CudaTakePendingWarning),
 *      same pattern as the VIA's cuda_touch loud stub.
 *
 *  Timing (m10, revised by CV-10): lazy-only.  No EventScheduler one-shots are
 *  armed — the boot protocol is poll-driven (S3 §1.5: ORB TREQ poll + IFR
 *  bit-2 poll) and state transitions complete synchronously inside the seam
 *  call, EXCEPT SR-int delivery, which is deferred to the next IFR read
 *  (CudaSettle).  m10's original claim that QEMU's 20 us SR_INT delay
 *  ("MacOS 9 is racy", cuda.h) cannot matter on a poll-only profile was WRONG:
 *  ROM 9.0.1's startup sync (68k @ 0x9584) reads SR between the TACK-negate
 *  edge and its 15000-budget IFR.2 wait — eager delivery lets that SR read
 *  clear the int before the wait starts, and the boot parks at 0x9754
 *  (M3b Wave-1 acceptance root cause).  Deferred-to-IFR-read delivery is the
 *  deterministic lazy equivalent of QEMU's delay.  The byte timing constants
 *  (71/88/61/13 us, donor study §3.2) remain unmodeled.
 *
 *  RTC (m8): GET_TIME returns Mac-epoch (1904) seconds in the LOCAL-time
 *  convention of macos_util.cpp:TimeToMacTime().  The time source is an
 *  injected callback so the module stays clock-free and testable; prod binds
 *  a wrapper that returns TimeToMacTime(time(NULL)).  SET_TIME keeps a delta
 *  against the injected source (QEMU tick_offset model).
 *
 *  PRAM (m9): 256-byte in-memory zero-init scratch.  READ_PRAM/READ_MCU_MEM
 *  replies are well-formed FULL-LENGTH (addr..0xFF) — the ROM caches all 256
 *  bytes at init and malformed short replies wedge its read loop.  Persistence
 *  is M4's NVRAM scope (deliberate dual-PRAM window until then; see M3b Task 5).
 *
 *  ADB packets route through an injected handler (no compile-time dependency on
 *  adb_stub.h — Task 2 builds in parallel; Task 3 binds the real stub).
 *  Reply framing (QEMU cuda_receive_packet_from_host + M3b pinned encodings):
 *    success -> [ADB_PACKET, 0x00, cmd, data...]
 *    absent  -> [ADB_PACKET, 0x02, cmd]            (timeout status, no data)
 *  Future autopoll-generated packets carry status flag 0x40 ("polled data",
 *  both oracles); Wave 1 never self-initiates packets (CV-4: autopoll on +
 *  no events => silent).  ONE_SECOND_MODE is deliberately absent (plan m2);
 *  it lands in cmd_unknown telemetry if the boot asks for it.
 */

#ifndef DEV_CUDA_H
#define DEV_CUDA_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>   // FILE* for CudaDumpPacketTrace (diagnostic dump only)

// ORB handshake bits (active-LOW; C3-corrected assignments)
#define CUDA_TREQ 0x08
#define CUDA_TACK 0x10
#define CUDA_TIP  0x20

// Packet types (1st byte of every host<->Cuda packet)
#define CUDA_PKT_ADB    0
#define CUDA_PKT_PSEUDO 1
#define CUDA_PKT_ERROR  2

// Error codes (2nd byte of an ERROR packet; DingusPPC viacuda.h values 1-4,
// QEMU cuda_receive_packet "bad parameters" = 5 for its strict-arg commands)
#define CUDA_ERR_BAD_PKT   1
#define CUDA_ERR_BAD_CMD   2
#define CUDA_ERR_BAD_SIZE  3
#define CUDA_ERR_BAD_PAR   4
#define CUDA_ERR_BAD_ARGS  5
// DingusPPC viacuda.h names the same value CUDA_ERR_I2C ("invalid I2C data or
// no acknowledge") — identical on the wire, kept distinct for readability.
#define CUDA_ERR_I2C       5

// Pseudo commands (2nd byte of a PSEUDO packet; QEMU cuda.h constants)
#define CUDA_CMD_AUTOPOLL          0x01
#define CUDA_CMD_READ_MCU_MEM      0x02
#define CUDA_CMD_GET_TIME          0x03
#define CUDA_CMD_READ_PRAM         0x07
#define CUDA_CMD_WRITE_MCU_MEM     0x08
#define CUDA_CMD_SET_TIME          0x09
#define CUDA_CMD_POWERDOWN         0x0A
#define CUDA_CMD_WRITE_PRAM        0x0C
#define CUDA_CMD_RESET_SYSTEM      0x11
#define CUDA_CMD_FILE_SERVER_FLAG  0x13
#define CUDA_CMD_SET_AUTO_RATE     0x14
#define CUDA_CMD_GET_AUTO_RATE     0x16
#define CUDA_CMD_SET_DEVICE_LIST   0x19
#define CUDA_CMD_GET_DEVICE_LIST   0x1A
#define CUDA_CMD_SET_POWER_MESSAGES 0x21
// I2C pseudo commands (DingusPPC viacuda.h; QEMU does NOT implement them):
//   READ_WRITE_I2C: [01 22 addr data...], addr = (7-bit dev addr << 1) | RW
//     (1 = read).
//   COMB_FMT_I2C:   [01 25 dev_addr sub_addr dev_addr1 data...]; dev_addr and
//     dev_addr1 must match in bits 7:1 (the live 9.0.1 boot issues this once
//     per I2C probe cycle).
// No I2C devices are modeled (M3b stop-rule) — every transaction answers
// "Unsupported I2C device" (error 5, DingusPPC's start_transaction-failed
// path); the probe map (i2c_addrs) records what the boot asked for.
#define CUDA_CMD_READ_WRITE_I2C    0x22
#define CUDA_CMD_COMB_FMT_I2C      0x25

#define CUDA_PRAM_SIZE 256
// MCU memory map (DingusPPC): PRAM window 0x100..0x1FF, Cuda FW ROM at 0xF00+.
#define CUDA_MCU_PRAM_START 0x100
#define CUDA_MCU_ROM_START  0xF00

#define CUDA_IN_BUF_SIZE  264   // host->Cuda packet (WRITE_PRAM worst case: 4+256)
#define CUDA_OUT_BUF_SIZE 264   // Cuda->host response (READ_PRAM: 3+256)

// Seam return flags: the VIA (which owns the IFR) must apply these on return.
enum {
	CUDA_SEAM_NONE         = 0,
	CUDA_SEAM_CLEAR_SR_INT = 1,   // clear IFR bit 2 (SR access, M4)
	CUDA_SEAM_RAISE_SR_INT = 2,   // set IFR bit 2 (shift complete / attention)
};

// --- S4 timer-delayed SR-int delivery seam (Operation NewSheep) -----------------
// Behavioral port of DingusPPC ViaCuda's interrupt model — devices/common/viacuda.cpp
// @ b2660e29201730efc2179a43ec6a0a5fb22ad120 (GPLv3; cite, never PR upstream):
//   assert_sr_int()  -> sets VIA IFR.SR
//   schedule_sr_int(timeout_ns) -> posts a one-shot that LATER raises the SR int
//   update_irq()     -> active = _via_ifr & _via_ier & 0x7F (the IFR&IER gate our
//                       via_update_irq already implements)
// The lazy-only CudaSettle model (delivery on the guest's IFR read) was M14's wall:
// the NewWorld NK enables IER.SR and waits for an INTERRUPT-driven SR int — it does
// NOT poll IFR — so the latched sr_int_pending was never converted to IFR.SR (65,539
// IER reads vs 2 IFR reads; M14-FINDINGS §3/§4b). This seam reproduces DingusPPC's
// schedule_sr_int: each Cuda edge that latches sr_int_pending arms a one-shot
// CUDA_SR_DELAY_NS later; the callback delivers IFR.SR via the VIA latch callback,
// independent of any guest IFR read. The 20µs delay matches QEMU's cuda_delay_set_sr_int
// (sr_delay_ns) and preserves CV-10 (the int lands AFTER the guest's follow-up SR read).
//
// PROSPECTIVE — needs validation against a live S4 boot. Offline unit tests prove the
// VIA-layer mechanism (IFR.SR set without an IFR read, IER masking honored, the
// assert/clear edges). M14 Smoke H showed the live VIA→PIC edge fires; the remaining
// NK→DR forwarding gap is OUT OF SCOPE here (NK/supervisor layer, not this device).
//
// Gated/opt-in: default UNBOUND => lazy-only CudaSettle path, byte-identical. Prod
// binds this only under the NewWorld machine profile. dev_cuda stays lock-free (§2g):
// the scheduling adapter (prod: main_unix) is responsible for running the delivery
// callback under the VIA bus region lock, exactly like the VIA's own timer_arm.
#define CUDA_SR_DELAY_NS 20000u   // QEMU cuda.h sr_delay_ns; DingusPPC schedule_sr_int

// Schedule a one-shot `delay_ns` in the future that invokes cb(cb_opaque). Prod wraps
// EventScheduler::add_oneshot_timer (+ locked_call); tests inject a fake scheduler.
typedef void (*CudaScheduleFn)(void *sched_opaque, uint64_t delay_ns,
                               void (*cb)(void *), void *cb_opaque);
// Set the given IFR bits on the bound VIA and recompute its IRQ summary output.
// Prod binds VIALatchIFRBits; runs under the VIA region lock (via the adapter).
typedef void (*CudaLatchFn)(void *via_opaque, uint8_t ifr_bits);

// ADB handler (Task 2's adb_stub binds here in Task 3; tests bind a mock).
// cmd byte = [addr:4][cmd:2][reg:2]; listen_data/listen_len = Listen payload.
// Fills reply[] with DATA bytes only (no Cuda framing).  Returns:
//   >=0  data length (0 = success, no data)
//   -1   no device at address (framed as timeout status 0x02)
typedef int (*CudaADBHandler)(void *opaque, uint8_t cmd,
                              const uint8_t *listen_data, int listen_len,
                              uint8_t *reply, int reply_max);

struct CudaDevice {
	// --- handshake state ---
	uint8_t  sr;             // shift-register byte in flight (the VIA forwards SR
	                         // accesses here; its own stored sr goes unused)
	uint8_t  last_b;         // last host-written ORB image (TACK/TIP bits)
	uint8_t  treq_asserted;  // 1 => TREQ low => derived ORB bit 3 reads 0
	uint8_t  sr_int_pending; // consume-once raise latch; delivered ONLY by
	                         // CudaSettle on the IFR-read surface (CV-10)
	// --- host->Cuda packet capture ---
	uint8_t  in_buf[CUDA_IN_BUF_SIZE];
	int      in_count;
	// --- Cuda->host response ---
	uint8_t  out_buf[CUDA_OUT_BUF_SIZE];
	int      out_size, out_pos;
	// --- device state ---
	uint8_t  autopoll_on;
	uint8_t  autopoll_rate;      // ms (default 11, DingusPPC)
	uint16_t device_list;        // ADB autopoll device bitmap
	uint8_t  file_server_flag;
	uint32_t time_delta;         // SET_TIME adjustment vs injected now_mac
	uint8_t  pram[CUDA_PRAM_SIZE];
	// --- injected services ---
	uint32_t (*now_mac)(void *opaque);   // Mac-epoch LOCAL seconds (m8); may be NULL
	void     *now_opaque;
	CudaADBHandler adb_fn;               // may be NULL (=> all devices absent)
	void     *adb_opaque;
	// --- telemetry (plain counters; §2g capture-only) ---
	uint64_t orb_writes, sr_reads, sr_writes;
	uint64_t syncs;              // TACK-toggle attention sequences answered
	uint64_t bytes_in, bytes_out;
	uint64_t packets, responses;
	uint64_t in_overflows;       // host packet exceeded in_buf (bytes dropped)
	uint64_t cmd_adb, adb_absent;
	uint64_t cmd_get_time, cmd_set_time, cmd_autopoll;
	uint64_t cmd_pram_read, cmd_pram_write;
	uint64_t cmd_acked;          // other known commands acked (file server etc.)
	uint64_t cmd_bad_param;      // known command, wrong arg count/value
	uint64_t cmd_unknown;        // unknown pseudo command / packet type
	uint8_t  last_unknown_type;  // packet type of the most recent unknown
	uint8_t  last_unknown_cmd;   // command byte of the most recent unknown
	uint64_t resets_latched, powerdowns_latched;   // loud-log, no action
	// warning latch (unknown command / RESET / POWERDOWN; §2g latch-if-empty:
	// one pending at a time, taking it re-arms — VIATakePendingWarning pattern)
	uint8_t  warned;             // any warning ever latched (telemetry)
	const char *warn_what;       // static string, pending until taken
	// --- I2C telemetry (APPENDED LAST per the struct-layout rule: dev_cuda.h
	// is included by main_unix/sheepshaver_glue/dev_via6522 and the Unix build
	// has no header dep tracking — mid-struct inserts shift offsets in stale
	// .o files; see the 4e544aff review + the powerdowns=6114308096 incident) ---
	uint64_t cmd_i2c;            // READ_WRITE_I2C/COMB_FMT_I2C transactions
	uint64_t i2c_absent;         // ... answered "no such device" (currently all)
	// capture-only probe map (§2g telemetry): distinct raw I2C addr bytes seen.
	// Saturates at 16 — a sweep wider than that drops the tail (the stats line
	// shows what fit, not necessarily every probed address).
	uint8_t  i2c_addrs[16];
	int      i2c_addr_count;
	// MCU internal RAM 0x00..0xFF (APPENDED LAST per the struct-layout rule).
	// The 9.0.1 boot's probe cycle does READ_MCU_MEM 0x00A1 / WRITE / re-read
	// every cycle (SS_CUDA_TRACE evidence) — empty replies for sub-0x100
	// addresses were the probe-cycle loop-ender (the "boot never reads them"
	// assumption falsified).  Zero-init; distinct from the PRAM array.
	uint8_t  mcu_ram[256];
	// --- S4 timer-delayed SR-int delivery (APPENDED LAST; gated, default unbound
	// = lazy-only byte-identical).  See the seam contract above + CudaBindTimerDelivery. ---
	CudaScheduleFn sr_schedule;     // NULL => lazy-only (CudaSettle on IFR read)
	void          *sr_sched_opaque;
	CudaLatchFn    sr_latch;        // sets IFR bits + recomputes the VIA IRQ summary
	void          *sr_via_opaque;
	uint64_t       sr_timer_arms;   // telemetry: one-shots scheduled
	uint64_t       sr_timer_fires;  // telemetry: timer deliveries that raised IFR.SR
};

extern void CudaReset(CudaDevice *c,
                      uint32_t (*now_mac)(void *opaque), void *now_opaque);
extern void CudaBindADB(CudaDevice *c, CudaADBHandler fn, void *opaque);

// S4: bind the timer-delayed SR-int delivery seam (default unbound = lazy-only).
// schedule/sched_opaque post one-shots; latch/via_opaque deliver IFR.SR on fire.
// Pass NULLs (or never call) to keep the byte-identical lazy CudaSettle path.
extern void CudaBindTimerDelivery(CudaDevice *c,
                                  CudaScheduleFn schedule, void *sched_opaque,
                                  CudaLatchFn latch, void *via_opaque);

// --- VIA seam entry points (all run under the VIA's region lock) -------------

// Host wrote ORB.  orb = full written byte (bits 4/5 = TACK/TIP), acr = current
// ACR (bit 4 = shift direction: 1 = host->Cuda).  Edge-triggered: byte capture /
// load on TACK/TIP transitions; packet commit + synchronous command processing
// when TIP negates with captured bytes (C2).  Always returns CUDA_SEAM_NONE —
// raises are deferred to CudaSettle (CV-10).
extern uint8_t CudaORBWritten(CudaDevice *c, uint8_t orb, uint8_t acr);

// Host wrote / read the shift register.  Both clear the LATCHED IFR.2 (M4) —
// flags say so — but never an undelivered pending raise (CV-10).
extern uint8_t CudaSRWritten(CudaDevice *c, uint8_t value);
extern uint8_t CudaSRRead(CudaDevice *c, uint8_t *value);

// Derive the ORB value the host must read: stored_orb with bit 3 recomputed
// from Cuda state (C1 — idle => 1, response/sync pending => 0).  Pure; the
// ORB-read path uses this ALONE (no settle — CV-10 delivery is IFR-read-only).
extern uint8_t CudaDeriveORB(const CudaDevice *c, uint8_t stored_orb);

// Deferred raise delivery — call from the IFR-read surface ONLY (CV-10; the
// lazy analogue of QEMU cuda_delay_set_sr_int).  Returns any unconsumed
// CUDA_SEAM_RAISE_SR_INT.  Consume-once, idempotent.
extern uint8_t CudaSettle(CudaDevice *c);

// Return-and-clear the pending one-shot warning (static string), or NULL.
// Same emission contract as VIATakePendingWarning (safe threads only).
extern const char *CudaTakePendingWarning(CudaDevice *c);

// Format telemetry as "packets=N responses=N ..." for the stats dump (snprintf
// into caller buffer; no FILE*).  Returns chars written.
extern size_t CudaFormatStats(const CudaDevice *c, char *buf, size_t buflen);

// Register the prod Cuda instance for crash-path telemetry (M3b Task 3; same
// pattern as VIARegisterDiagInstance).  CudaFormatStatsRegistered formats the
// registered instance's counters — counters-read-only, snprintf into a caller
// buffer, safe on the SIGSEGV dump path; returns 0 when nothing is registered
// (paravirtual stays silent by construction).
extern void CudaRegisterDiagInstance(CudaDevice *c);
extern size_t CudaFormatStatsRegistered(char *buf, size_t buflen);

// Capture-only packet telemetry (SS_CUDA_TRACE=1): dump the first packets'
// raw in/out bytes.  Crash/term-dump thread only — never a seam path.
extern void CudaDumpPacketTrace(FILE *f);

#endif
