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
};

extern void SCCReset(SCC8530 *s, uint32_t base);
// The MMIODevice trampolines (opaque = SCC8530*):
extern uint64_t SCCRead(void *opaque, uint32_t addr, unsigned size);
extern void SCCWrite(void *opaque, uint32_t addr, unsigned size, uint64_t value);
extern bool SCCReadIsIdle(void *opaque, uint32_t addr, uint64_t value);

// Debug/test injection API.  Enqueue one byte into channel ch (SCC_CH_A or SCC_CH_B).
// Caller holds the MMIOBus region lock.  No malloc, no stdio.
extern void SCCInjectRx(SCC8530 *s, int ch, uint8_t byte);

#endif
