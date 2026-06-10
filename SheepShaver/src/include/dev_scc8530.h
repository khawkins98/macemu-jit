/*
 *  dev_scc8530.h - Zilog SCC 8530, legacy/compat MacIO port layout (M1 scope: SPIKE-S3 §4.1)
 *  Layout at base (= 0xF3012000 on Core99): +0 ch B ctrl, +2 ch A ctrl, +4 ch B data, +6 ch A data.
 *  No Rx source is connected in M1: RR0 bit0 always 0 (honest "no character").
 */

#ifndef DEV_SCC8530_H
#define DEV_SCC8530_H

#include "mmio_bus.h"

#define SCC_CH_B 0
#define SCC_CH_A 1

struct SCC8530 {
	uint32_t base;
	uint8_t  wr[2][16];     // stored write registers per channel
	uint8_t  reg_ptr[2];    // WR0-selected register pointer (0 after any access)
	uint64_t wr_writes;     // telemetry: total WR data writes (DoD register-traffic assert)
	uint64_t tx_bytes;      // bytes the guest transmitted (discarded)
	uint64_t rr0_polls;     // RR0 reads (the poll loops)
};

extern void SCCReset(SCC8530 *s, uint32_t base);
// The MMIODevice trampolines (opaque = SCC8530*):
extern uint64_t SCCRead(void *opaque, uint32_t addr, unsigned size);
extern void SCCWrite(void *opaque, uint32_t addr, unsigned size, uint64_t value);
extern bool SCCReadIsIdle(void *opaque, uint32_t addr, uint64_t value);

#endif
