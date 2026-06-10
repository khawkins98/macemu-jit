/*
 *  dev_scc8530.cpp - Zilog SCC 8530 model, M1 scope (SPIKE-S3 §4.1; CORE99 §4 fence).
 *  Reachable from the Mach handler thread: no malloc/stdio (bus holds the lock).
 */

#include "dev_scc8530.h"
#include <string.h>

void SCCReset(SCC8530 *s, uint32_t base)
{
	memset(s, 0, sizeof(*s));
	s->base = base;
}

static int decode_ch(uint32_t off, bool *is_data)
{
	// +0 B ctrl, +2 A ctrl, +4 B data, +6 A data (legacy/compat layout)
	*is_data = (off & 4) != 0;
	return (off & 2) ? SCC_CH_A : SCC_CH_B;
}

static uint8_t read_rr(SCC8530 *s, int ch, uint8_t rr)
{
	switch (rr) {
	case 0:  return 0x04;                  // bit2 Tx Buffer Empty=1; bit0 Rx avail=0 (no source)
	case 1:  return 0x01;                  // bit0 All Sent=1; bits4-6 errors=0
	default: return s->wr[ch][rr & 15];    // stored state read-back (fence: nothing else probed)
	}
}

uint64_t SCCRead(void *opaque, uint32_t addr, unsigned size)
{
	SCC8530 *s = (SCC8530 *)opaque;
	(void)size;   // consumers are byte-wide; wider reads replicate the byte in the low bits
	bool is_data; int ch = decode_ch(addr - s->base, &is_data);
	if (is_data)
		return 0;                          // Rx FIFO empty in M1
	uint8_t ptr = s->reg_ptr[ch];
	s->reg_ptr[ch] = 0;                    // any control access resets the pointer
	if (ptr == 0) s->rr0_polls++;
	return read_rr(s, ch, ptr);
}

void SCCWrite(void *opaque, uint32_t addr, unsigned size, uint64_t value)
{
	SCC8530 *s = (SCC8530 *)opaque;
	(void)size;
	uint8_t v = (uint8_t)value;
	bool is_data; int ch = decode_ch(addr - s->base, &is_data);
	if (is_data) { s->tx_bytes++; return; } // Tx sink
	uint8_t ptr = s->reg_ptr[ch];
	if (ptr == 0) {
		// WR0: low 4 bits select the register (raw 8-15 values fold the point-high
		// command in, exactly as the ROM writes them); bits 5-3 = command.
		uint8_t cmd = (v >> 3) & 7;
		if (cmd == 6) { /* 0x30 Error Reset: no latched errors to clear in M1 */ }
		s->reg_ptr[ch] = v & 0x0F;
		return;
	}
	s->reg_ptr[ch] = 0;
	s->wr[ch][ptr] = v;
	s->wr_writes++;
	if (ptr == 9) {
		if ((v & 0xC0) == 0xC0) {          // force hardware reset
			memset(s->wr, 0, sizeof(s->wr));
			s->reg_ptr[0] = s->reg_ptr[1] = 0;
		} else if ((v & 0xC0) == 0x80) {   // channel A reset
			memset(s->wr[SCC_CH_A], 0, sizeof(s->wr[SCC_CH_A]));
		} else if ((v & 0xC0) == 0x40) {   // channel B reset
			memset(s->wr[SCC_CH_B], 0, sizeof(s->wr[SCC_CH_B]));
		}
	}
}

bool SCCReadIsIdle(void *opaque, uint32_t addr, uint64_t value)
{
	SCC8530 *s = (SCC8530 *)opaque;
	uint32_t off = addr - s->base;
	// Idle = a control-port read that reported "no Rx character" (RR0 bit0 == 0).
	return (off & 4) == 0 && (value & 0x01) == 0;
}
