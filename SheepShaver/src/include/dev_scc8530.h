/*
 *  dev_scc8530.h - Zilog SCC 8530, legacy/compat MacIO port layout (M1 scope: SPIKE-S3 §4.1)
 *  Layout at base (= 0xF3012000 on Core99): +0 ch B ctrl, +2 ch A ctrl, +4 ch B data, +6 ch A data.
 *
 *  Rx queue: SCCInjectRx() enqueues bytes (up to SCC_RX_QUEUE_MAX per channel).  Used by:
 *    - SS_SCC_RX_INJECT env-var one-shot (debug/demo injection; NK console handshake)
 *    - unit tests (test_dev_scc8530.cpp)
 *    - M3b real serial input (future; will use the same queue)
 *  Caller must hold the owning MMIOBus region lock (same contract as SCCRead/SCCWrite).
 *  Drops silently when the queue is full (rx_dropped incremented).
 */

#ifndef DEV_SCC8530_H
#define DEV_SCC8530_H

#include "mmio_bus.h"

#define SCC_CH_B 0
#define SCC_CH_A 1

#define SCC_RX_QUEUE_MAX 16

struct SCC8530 {
	uint32_t base;
	uint8_t  wr[2][16];     // stored write registers per channel
	uint8_t  reg_ptr[2];    // WR0-selected register pointer (0 after any access)
	uint64_t wr_writes;     // telemetry: total WR data writes (DoD register-traffic assert)
	uint64_t tx_bytes;      // bytes the guest transmitted (discarded)
	uint64_t rr0_polls;     // RR0 reads (the poll loops)

	// Per-channel Rx queue (circular FIFO).
	uint8_t  rx_queue[2][SCC_RX_QUEUE_MAX];
	uint8_t  rx_head[2];    // index of next byte to pop
	uint8_t  rx_count[2];   // number of bytes currently queued
	uint64_t rx_injected;   // telemetry: total bytes enqueued via SCCInjectRx
	uint64_t rx_consumed;   // telemetry: total bytes popped by data reads
	uint64_t rx_dropped;    // telemetry: bytes dropped due to full queue

	// --- Wave-2 W2-3 (rev 2 F4): interrupt-condition state (NEW model code;
	// the M1 model stored raw WR bytes only). Fields APPENDED LAST (struct rule).
	//
	// Per-channel level condition:
	//   irq_cond[ch] = Rx-available  (rx_count[ch] > 0)
	//                ∧ WR1 Rx-int-enabled  (wr[ch][1] & 0x18 — modes 01/10/11;
	//                  00 = Rx int disabled)
	//                ∧ WR9 MIE  (wr9_shared & 0x08)
	// LEVEL semantics by design (a simplification of the 8530's per-character
	// interrupt latching, documented here): the condition holds while data is
	// queued and enabled — matching the OpenPIC LEVEL-sensitive input it feeds
	// (raise on assert, lower on deassert; the line drops when the guest drains
	// the queue or disables the source).
	//
	// WR9 is CHIP-WIDE on real silicon (one register, addressable through either
	// channel's control port). The M1 model stores wr[ch][9] per channel for
	// read-back compat (unchanged); the interrupt predicate uses wr9_shared,
	// updated on ANY WR9 data write and cleared by the WR9 force-hardware-reset
	// command (channel resets leave it, as on hardware).
	uint8_t  wr9_shared;    // chip-wide WR9 copy (MIE bit 3 is what we consult)
	uint8_t  irq_cond[2];   // current per-channel condition level (0/1)
	// Output seam (the OpenPICBindOutput idiom): fires on per-channel condition
	// TRANSITIONS only, under whatever lock the caller of the mutating entry
	// point held (in prod: the SCC bus region lock). Cross-region lock order
	// (documented, BINDING): device -> pic — the callback may take the PIC
	// region lock; the PIC's own output callback must never take a device lock.
	void   (*irq_fn)(void *opaque, int ch, bool asserted);
	void    *irq_opaque;
	uint64_t irq_raises[2]; // telemetry: assert edges per channel
	uint64_t irq_lowers[2]; // telemetry: deassert edges per channel
	// (append any future fields HERE, last)
};

extern void SCCReset(SCC8530 *s, uint32_t base);
// The MMIODevice trampolines (opaque = SCC8530*):
extern uint64_t SCCRead(void *opaque, uint32_t addr, unsigned size);
extern void SCCWrite(void *opaque, uint32_t addr, unsigned size, uint64_t value);
extern bool SCCReadIsIdle(void *opaque, uint32_t addr, uint64_t value);

// Debug/test injection API.  Enqueue one byte into channel ch (SCC_CH_A or SCC_CH_B).
// Caller holds the MMIOBus region lock.  No malloc, no stdio.
extern void SCCInjectRx(SCC8530 *s, int ch, uint8_t byte);

// Wave-2 W2-3 (F4): bind the interrupt-output seam AFTER SCCReset (reset
// clears the binding — the OpenPICBindOutput order contract). fn fires on
// per-channel condition transitions (see the struct comment for the predicate
// and the device->pic lock-order rule). NULL fn unbinds.
extern void SCCBindIRQOutput(SCC8530 *s,
                             void (*fn)(void *opaque, int ch, bool asserted),
                             void *opaque);

// The per-channel condition predicate (recomputed from current state; does
// not fire the seam). Exposed for unit tests and host-side diagnostics.
extern bool SCCIRQCondition(const SCC8530 *s, int ch);

#endif
