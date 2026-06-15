/*
 *  adb_stub.h - Minimal ADB bus stub behind the Cuda (Machine Layer M3b Task 2).
 *
 *  Scope (donor study M3-PIC-CUDA-DONOR-STUDY.md §7.2, ADB scope DECISION 2026-06-10):
 *  just enough ADB for the Mac OS boot-time device scan to complete — a keyboard at
 *  default address 2 and a mouse at default address 3, Talk R3 identification replies,
 *  Listen R3 address-move/handler-change (the scan MOVES devices to resolve conflicts),
 *  SendReset, Flush, and autopoll register plumbing. Talk R0 always returns zero data:
 *  NO host input flows over ADB in this stub — input stays on the existing
 *  paravirtual/HLE event path (ADB_interrupt / SDL).
 *
 *  DEFERRED WORK: the full host-input-over-ADB implementation (real autopoll packets
 *  driving the guest's ADB stack) replaces this module behind this same interface;
 *  see donor study §7.2 for the pickup requirements and donor references.
 *
 *  Behavioral oracle (consulted, not ported — backport hygiene):
 *    QEMU hw/input/adb.c, adb-kbd.c, adb-mouse.c, adb-internal.h
 *      @ de5d8bfd6105d3dd3ae668df9762df244a6d1506
 *    DingusPPC devices/common/adb (all files) @ 92bb6d10549529f9f4031a85c2bc136149535bdc
 *      (donor reference for the future full implementation)
 *
 *  Oracle-verified protocol notes:
 *    - Command byte = [addr:4][cmd:2][reg:2]; low nibble 0x0 = SendReset (bus-wide,
 *      QEMU ADB_BUSRESET), 0x1 = Flush, 0x8|reg = Listen (ADB_WRITEREG),
 *      0xC|reg = Talk (ADB_READREG).
 *    - Talk R3, device present -> data [devaddr, handler_id] (adb-kbd.c/adb-mouse.c
 *      READREG case 3: obuf[0]=d->devaddr, obuf[1]=d->handler). After an address move,
 *      the NEW address is echoed.
 *    - Listen R3 handler-id byte (data byte 1): 0xFF ADB_CMD_SELF_TEST = ignore;
 *      0xFE ADB_CMD_CHANGE_ID / 0xFD ..._AND_ACT / 0x00 ..._AND_ENABLE = move device
 *      to (data byte 0 & 0xF). The oracle moves UNCONDITIONALLY for all three (no
 *      collision detection is modeled — real hardware gates 0xFE on no-collision, QEMU
 *      does not; the boot scan only moves to free addresses so behavior is identical).
 *      Any other value = move address AND set handler if the id is supported
 *      (kbd: 1/2/3; mouse: 1/2 — unsupported ids leave the handler unchanged).
 *    - Mouse Listen R3 with a payload length != 2 data bytes is IGNORED whole — the
 *      oracle's guard for a Mac OS 9 ADB-driver bug that sends an invalid-length reg-3
 *      write after configuring the bus (adb-mouse.c WRITEREG case 3, "len != 3").
 *    - Talk to an ABSENT address = bus timeout (QEMU ADB_RET_NOTPRESENT) -> -1 here;
 *      the Cuda frames it as status 0x02. Talk R0 with no input data -> 0 (success,
 *      zero data bytes; Cuda frames [ADB_PACKET, 0x00, cmd]).
 *    - Defaults (oracle reset fns): keyboard devaddr 2 handler 1 (Apple Standard
 *      Keyboard); mouse devaddr 3 handler 2 (Classic Apple Mouse Protocol, 200 cpi).
 *
 *  House style (dev_scc8530): plain struct + extern fns, telemetry counters, no
 *  stdio/malloc on the call paths — ADBStubCommand can run under a bus region lock on
 *  the Mach exception-handler thread (§2g).
 */

#ifndef ADB_STUB_H
#define ADB_STUB_H

#include <stdint.h>

#define ADB_STUB_MAX_REPLY 8

struct ADBStub {
	uint8_t kbd_addr, kbd_handler;     // defaults 2 / per-oracle
	uint8_t mouse_addr, mouse_handler; // defaults 3 / per-oracle
	bool    autopoll_on; uint8_t autopoll_rate;
	uint64_t talks, listens, absent_talks, resets;   // telemetry

	// --- extra telemetry (appended after the pinned fields; M3b Task 2) ---
	uint64_t flushes;          // Flush commands to present devices (acked)
	uint64_t moves;            // Listen R3 address-set commands applied (counted
	                           //   even if the new address equals the old one)
	uint64_t handler_changes;  // Listen R3 handler-id changes applied
	uint64_t listens_ignored;  // Listen R3 payloads rejected (bad length)
	uint64_t absent_other;     // non-Talk commands addressed to absent devices
};

// Reset the stub to power-on state: devices at default addresses/handlers,
// autopoll off, all telemetry zeroed. (The bus SendReset command — cmd byte low
// nibble 0x0 — restores addresses/handlers and bumps `resets` but does NOT touch
// the autopoll plumbing or telemetry.)
extern void ADBStubReset(ADBStub *a);

// One ADB command (cmd byte = [addr:4][cmd:2][reg:2]) + optional Listen payload.
// Fills reply[] with DATA bytes only (no Cuda framing). Returns:
//   >=0  data length (0 = success, no data — e.g. Talk R0 with no input)
//   -1   no device at address (Cuda frames as timeout status 0x02)
// Writes at most reply_max bytes (longer replies are clamped); a negative
// reply_max is treated as 0 so it can never alias the -1 sentinel.
extern int ADBStubCommand(ADBStub *a, uint8_t cmd,
                          const uint8_t *listen_data, int listen_len,
                          uint8_t *reply, int reply_max);

#endif
