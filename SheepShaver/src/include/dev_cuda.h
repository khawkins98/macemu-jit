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
 *      IFR.2" is communicated back synchronously via CUDA_SEAM_* return flags —
 *      the VIA owns the IFR.  The raise latch is consume-once: the first seam
 *      call that returns CUDA_SEAM_RAISE_SR_INT consumes it, so applying flags
 *      from mutators AND calling CudaSettle on the poll surfaces (ORB reads,
 *      IFR reads — the C2 lazy-settle rule) can never double-raise.
 *    - §2g: no stdio, no malloc on any seam path (fault-thread reachable).
 *      Unknown commands latch a one-shot warning (CudaTakePendingWarning),
 *      same pattern as the VIA's cuda_touch loud stub.
 *
 *  Timing (m10): lazy-only.  No EventScheduler one-shots are armed — the boot
 *  protocol is poll-driven (S3 §1.5: ORB TREQ poll + IFR bit-2 poll) and every
 *  state transition completes synchronously inside the seam call.  The byte
 *  timing constants (71/88/61/13 us, donor study §3.2) are deliberately not
 *  modeled; QEMU's 20 us SR_INT delay exists for an interrupt-delivery race
 *  ("MacOS 9 is racy", cuda.h) that cannot occur on the IFR-poll-only newworld
 *  profile.  Revisit if/when VIA SR interrupts are delivered for real (Wave 2+).
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
	uint8_t  sr_int_pending; // consume-once raise latch (see header contract)
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
	uint64_t resets_latched, powerdowns_latched;   // loud-log, no action
	// warning latch (unknown command / RESET / POWERDOWN; §2g latch-if-empty:
	// one pending at a time, taking it re-arms — VIATakePendingWarning pattern)
	uint8_t  warned;             // any warning ever latched (telemetry)
	const char *warn_what;       // static string, pending until taken
};

extern void CudaReset(CudaDevice *c,
                      uint32_t (*now_mac)(void *opaque), void *now_opaque);
extern void CudaBindADB(CudaDevice *c, CudaADBHandler fn, void *opaque);

// --- VIA seam entry points (all run under the VIA's region lock) -------------

// Host wrote ORB.  orb = full written byte (bits 4/5 = TACK/TIP), acr = current
// ACR (bit 4 = shift direction: 1 = host->Cuda).  Edge-triggered: byte capture /
// load on TACK/TIP transitions; packet commit + synchronous command processing
// when TIP negates with captured bytes (C2).  Returns CUDA_SEAM_* flags.
extern uint8_t CudaORBWritten(CudaDevice *c, uint8_t orb, uint8_t acr);

// Host wrote / read the shift register.  Both clear IFR.2 (M4) — flags say so.
extern uint8_t CudaSRWritten(CudaDevice *c, uint8_t value);
extern uint8_t CudaSRRead(CudaDevice *c, uint8_t *value);

// Derive the ORB value the host must read: stored_orb with bit 3 recomputed
// from Cuda state (C1 — idle => 1, response/sync pending => 0).  Pure; call
// CudaSettle first on the ORB-read path (or use both, order per Task 3).
extern uint8_t CudaDeriveORB(const CudaDevice *c, uint8_t stored_orb);

// Lazy settle for the two poll surfaces (ORB reads, IFR reads — C2).  Returns
// any unconsumed CUDA_SEAM_RAISE_SR_INT.  Safe to call anywhere, idempotent.
extern uint8_t CudaSettle(CudaDevice *c);

// Return-and-clear the pending one-shot warning (static string), or NULL.
// Same emission contract as VIATakePendingWarning (safe threads only).
extern const char *CudaTakePendingWarning(CudaDevice *c);

// Format telemetry as "packets=N responses=N ..." for the stats dump (snprintf
// into caller buffer; no FILE*).  Returns chars written.
extern size_t CudaFormatStats(const CudaDevice *c, char *buf, size_t buflen);

#endif
