/*
 *  dev_cuda.cpp - Cuda (Apple MCU) protocol state machine + command dispatcher
 *  (Machine Layer M3b Wave 1, Task 1).  Pure module behind the VIA's SR/ORB
 *  surface — see dev_cuda.h for the full seam contract and design notes.
 *
 *  Behavioral extraction (NOT a code port) from:
 *    - QEMU hw/misc/macio/cuda.c @ de5d8bfd6105d3dd3ae668df9762df244a6d1506:
 *      the handshake engine below mirrors cuda_update() — edge-triggered byte
 *      capture/load on TACK/TIP transitions while TIP is asserted, the
 *      TIP-negated sync branch (TREQ mirrors TACK), packet commit + inline
 *      processing at the TIP-negate edge, command table + packet/error framing.
 *    - DingusPPC devices/common/viacuda.cpp @ 92bb6d10549529f9f4031a85c2bc136149535bdc:
 *      PRAM/MCU_MEM command semantics (window 0x100..0x1FF, FW-ROM blob),
 *      GET_AUTOPOLL_RATE / GET_DEVICE_BITMAP, error codes 1-4, and the
 *      end-of-transaction discard of a partially-read response.
 *
 *  §2g: every function here is reachable from the Mach exception-handler
 *  thread under the bus region lock — no stdio (snprintf-into-buffer only in
 *  CudaFormatStats, never on seam paths), no malloc, no locks.  Warnings are
 *  latched (CudaTakePendingWarning) for emission by safe threads, same pattern
 *  as the VIA's cuda_touch loud stub.
 *
 *  All handshake bits active-LOW: TREQ=ORB.3 (Cuda->host), TACK=ORB.4,
 *  TIP=ORB.5 (host outputs; DDRB=0x30).  "Asserted" = electrical low = bit 0.
 */

#include "dev_cuda.h"
#include <stdio.h>    // snprintf only (CudaFormatStats; no FILE* I/O)
#include <string.h>

#define HSK  ((uint8_t)(CUDA_TACK | CUDA_TIP))   // host-driven handshake bits
#define ACR_SR_OUT 0x10                          // ACR bit 4: shift OUT (host->Cuda)

void CudaReset(CudaDevice *c, uint32_t (*now_mac)(void *), void *now_opaque)
{
	memset(c, 0, sizeof(*c));
	c->last_b = HSK;            // idle: TACK+TIP negated (boot writes 0x38 first)
	c->autopoll_rate = 11;      // ms; DingusPPC viacuda.h default
	c->now_mac = now_mac;
	c->now_opaque = now_opaque;
}

void CudaBindADB(CudaDevice *c, CudaADBHandler fn, void *opaque)
{
	c->adb_fn = fn;
	c->adb_opaque = opaque;
}

// Warning latch (loud-stub rule; emission on safe threads only).  Latch-if-
// empty: one pending warning at a time; taking it re-arms the latch (slightly
// richer than the VIA's once-per-lifetime cuda_touch — same §2g properties).
static void warn_latch(CudaDevice *c, const char *what)
{
	if (!c->warn_what) {
		c->warned = 1;
		c->warn_what = what;    // static string; pending until taken
	}
}

const char *CudaTakePendingWarning(CudaDevice *c)
{
	const char *w = c->warn_what;
	c->warn_what = 0;
	return w;
}

// Consume-once raise latch (header contract: delivered ONLY via CudaSettle on
// the IFR-read surface — CV-10 deferred-delivery model).
static uint8_t take_pending(CudaDevice *c)
{
	if (c->sr_int_pending) {
		c->sr_int_pending = 0;
		return CUDA_SEAM_RAISE_SR_INT;
	}
	return CUDA_SEAM_NONE;
}

uint8_t CudaSettle(CudaDevice *c)
{
	// CV-10 deferred delivery: this is the ONLY raise-delivery point, called
	// from the VIA's R_IFR read path (the guest's IFR.2 poll loops).  It is
	// the lazy-model analogue of QEMU's cuda_delay_set_sr_int (sr_delay_ns =
	// 20us): on hardware the post-edge SR int arrives AFTER the host's
	// follow-up SR read, so it must not be delivered (and then cleared) before
	// the guest starts polling IFR.  ROM 9.0.1 startup sync 0x9584 depends on
	// this ordering (root cause of the M3b Wave-1 acceptance park at 0x9754).
	return take_pending(c);
}

uint8_t CudaSRWritten(CudaDevice *c, uint8_t value)
{
	c->sr = value;
	c->sr_writes++;
	// M4 (rev: CV-10): SR access clears the LATCHED IFR.2 only.  An undelivered
	// pending raise survives — QEMU's sr_delay timer is not cancelled by SR
	// accesses either.
	return CUDA_SEAM_CLEAR_SR_INT;
}

uint8_t CudaSRRead(CudaDevice *c, uint8_t *value)
{
	*value = c->sr;
	c->sr_reads++;
	return CUDA_SEAM_CLEAR_SR_INT;   // latched IFR.2 only; pending survives (CV-10)
}

uint8_t CudaDeriveORB(const CudaDevice *c, uint8_t stored_orb)
{
	// C1: bit 3 (TREQ) is recomputed from Cuda state on EVERY read; the host's
	// stored bits (4/5 and the non-Cuda bits) pass through untouched.
	return (uint8_t)((stored_orb & ~CUDA_TREQ) |
	                 (c->treq_asserted ? 0 : CUDA_TREQ));
}

// --- response builders (QEMU obuf framing / DingusPPC response_header) --------

static void resp_begin(CudaDevice *c, uint8_t type, uint8_t status, uint8_t cmd)
{
	c->out_buf[0] = type;
	c->out_buf[1] = status;
	c->out_buf[2] = cmd;
	c->out_size = 3;
	c->out_pos = 0;
}

static void resp_append(CudaDevice *c, const uint8_t *data, int len)
{
	if (len > CUDA_OUT_BUF_SIZE - c->out_size)
		len = CUDA_OUT_BUF_SIZE - c->out_size;   // never overflow (full-length
	                                             // PRAM is the sizing case, fits)
	memcpy(c->out_buf + c->out_size, data, (size_t)len);
	c->out_size += len;
}

static void resp_append_byte(CudaDevice *c, uint8_t b) { resp_append(c, &b, 1); }

// Error packet [ERROR, code, pkt_type, cmd] (QEMU cuda_receive_packet /
// DingusPPC error_response — identical layout).
static void resp_error(CudaDevice *c, uint8_t code, uint8_t type, uint8_t cmd)
{
	c->out_buf[0] = CUDA_PKT_ERROR;
	c->out_buf[1] = code;
	c->out_buf[2] = type;
	c->out_buf[3] = cmd;
	c->out_size = 4;
	c->out_pos = 0;
}

// --- pseudo command dispatcher (QEMU handlers[] + DingusPPC pseudo_command) ---

static uint32_t cuda_now(CudaDevice *c)
{
	return c->now_mac ? c->now_mac(c->now_opaque) : 0;
}

// Capture-only I2C probe map (§2g): latch distinct raw addr bytes so the
// stats dump names what the boot asked for over I2C.
static void i2c_latch_addr(CudaDevice *c, uint8_t addr)
{
	for (int i = 0; i < c->i2c_addr_count; i++)
		if (c->i2c_addrs[i] == addr) return;
	if (c->i2c_addr_count < (int)sizeof(c->i2c_addrs))
		c->i2c_addrs[c->i2c_addr_count++] = addr;
}

static void pseudo_command(CudaDevice *c, uint8_t cmd, const uint8_t *a, int n)
{
	switch (cmd) {
	case CUDA_CMD_AUTOPOLL:                      // QEMU cuda_cmd_autopoll
		if (n != 1) goto bad_args;
		c->autopoll_on = (a[0] != 0);
		c->cmd_autopoll++;
		resp_begin(c, CUDA_PKT_PSEUDO, 0, cmd);
		break;
	case CUDA_CMD_SET_AUTO_RATE:                 // QEMU: rate 0 = bad parameter
		if (n != 1 || a[0] == 0) goto bad_args;
		c->autopoll_rate = a[0];
		c->cmd_acked++;
		resp_begin(c, CUDA_PKT_PSEUDO, 0, cmd);
		break;
	case CUDA_CMD_GET_AUTO_RATE:                 // DingusPPC GET_AUTOPOLL_RATE
		if (n != 0) goto bad_args;
		c->cmd_acked++;
		resp_begin(c, CUDA_PKT_PSEUDO, 0, cmd);
		resp_append_byte(c, c->autopoll_rate);
		break;
	case CUDA_CMD_SET_DEVICE_LIST:               // QEMU: 2-byte BE mask
		if (n != 2) goto bad_args;
		c->device_list = (uint16_t)((a[0] << 8) | a[1]);
		c->cmd_acked++;
		resp_begin(c, CUDA_PKT_PSEUDO, 0, cmd);
		break;
	case CUDA_CMD_GET_DEVICE_LIST:               // DingusPPC GET_DEVICE_BITMAP
		if (n != 0) goto bad_args;
		c->cmd_acked++;
		resp_begin(c, CUDA_PKT_PSEUDO, 0, cmd);
		resp_append_byte(c, (uint8_t)(c->device_list >> 8));
		resp_append_byte(c, (uint8_t)c->device_list);
		break;
	case CUDA_CMD_FILE_SERVER_FLAG:
		if (n != 1) goto bad_args;
		c->file_server_flag = (a[0] != 0);
		c->cmd_acked++;
		resp_begin(c, CUDA_PKT_PSEUDO, 0, cmd);
		break;
	case CUDA_CMD_SET_POWER_MESSAGES:
		if (n != 1) goto bad_args;
		c->cmd_acked++;
		resp_begin(c, CUDA_PKT_PSEUDO, 0, cmd);
		break;
	case CUDA_CMD_GET_TIME: {                    // m8: Mac-epoch LOCAL seconds
		if (n != 0) goto bad_args;
		uint32_t t = cuda_now(c) + c->time_delta;
		c->cmd_get_time++;
		resp_begin(c, CUDA_PKT_PSEUDO, 0, cmd);
		resp_append_byte(c, (uint8_t)(t >> 24));
		resp_append_byte(c, (uint8_t)(t >> 16));
		resp_append_byte(c, (uint8_t)(t >> 8));
		resp_append_byte(c, (uint8_t)t);
		break; }
	case CUDA_CMD_SET_TIME: {                    // QEMU tick_offset model
		if (n != 4) goto bad_args;
		uint32_t t = ((uint32_t)a[0] << 24) | ((uint32_t)a[1] << 16) |
		             ((uint32_t)a[2] << 8) | a[3];
		c->time_delta = t - cuda_now(c);
		c->cmd_set_time++;
		resp_begin(c, CUDA_PKT_PSEUDO, 0, cmd);
		break; }
	case CUDA_CMD_POWERDOWN:                     // latched loud-log, NO action
		if (n != 0) goto bad_args;
		c->powerdowns_latched++;
		warn_latch(c, "Cuda: POWERDOWN requested (latched, no action taken)");
		resp_begin(c, CUDA_PKT_PSEUDO, 0, cmd);
		break;
	case CUDA_CMD_RESET_SYSTEM:                  // latched loud-log, NO action
		if (n != 0) goto bad_args;
		c->resets_latched++;
		warn_latch(c, "Cuda: RESET_SYSTEM requested (latched, no action taken)");
		resp_begin(c, CUDA_PKT_PSEUDO, 0, cmd);
		break;
	case CUDA_CMD_READ_PRAM: {                   // DingusPPC: addr16, <= 0xFF
		if (n != 2) goto bad_args;
		uint16_t addr = (uint16_t)((a[0] << 8) | a[1]);
		if (addr > 0xFF) {
			c->cmd_bad_param++;
			resp_error(c, CUDA_ERR_BAD_PAR, CUDA_PKT_PSEUDO, cmd);
			break;
		}
		c->cmd_pram_read++;
		resp_begin(c, CUDA_PKT_PSEUDO, 0, cmd);
		// m9: well-formed FULL-LENGTH reply (addr..0xFF) — the ROM caches all
		// 256 bytes at init; short replies wedge its read loop.
		resp_append(c, c->pram + addr, CUDA_PRAM_SIZE - addr);
		break; }
	case CUDA_CMD_WRITE_PRAM: {
		if (n < 2) goto bad_args;
		uint16_t addr = (uint16_t)((a[0] << 8) | a[1]);
		if (addr > 0xFF) {
			c->cmd_bad_param++;
			resp_error(c, CUDA_ERR_BAD_PAR, CUDA_PKT_PSEUDO, cmd);
			break;
		}
		for (int i = 0; i < n - 2; i++)
			c->pram[(addr + i) & 0xFF] = a[2 + i];
		c->cmd_pram_write++;
		resp_begin(c, CUDA_PKT_PSEUDO, 0, cmd);
		break; }
	case CUDA_CMD_READ_MCU_MEM: {                // DingusPPC: PRAM at 0x100..0x1FF
		if (n != 2) goto bad_args;
		uint16_t addr = (uint16_t)((a[0] << 8) | a[1]);
		c->cmd_pram_read++;
		resp_begin(c, CUDA_PKT_PSEUDO, 0, cmd);
		if (addr >= CUDA_MCU_PRAM_START && addr < CUDA_MCU_PRAM_START + CUDA_PRAM_SIZE) {
			resp_append(c, c->pram + (addr - CUDA_MCU_PRAM_START),
			            CUDA_PRAM_SIZE - (addr - CUDA_MCU_PRAM_START));
		} else if (addr >= CUDA_MCU_ROM_START) {
			// DingusPPC's faked Cuda FW blob: empty (c) string + 0x0019 +
			// version major/minor 0x0002/0x0029 (7 bytes).
			static const uint8_t fw[7] = { 0x00, 0x00, 0x19, 0x00, 0x02, 0x00, 0x29 };
			resp_append(c, fw, (int)sizeof(fw));
		}
		// other regions: header-only ack (DingusPPC sends open-ended zeros;
		// the boot never reads them — keep the reply finite and well-formed)
		break; }
	case CUDA_CMD_WRITE_MCU_MEM: {
		if (n < 2) goto bad_args;
		uint16_t addr = (uint16_t)((a[0] << 8) | a[1]);
		if (addr >= CUDA_MCU_PRAM_START && addr < CUDA_MCU_PRAM_START + CUDA_PRAM_SIZE) {
			for (int i = 0; i < n - 2; i++)
				c->pram[(addr - CUDA_MCU_PRAM_START + i) & 0xFF] = a[2 + i];
		}
		c->cmd_pram_write++;
		resp_begin(c, CUDA_PKT_PSEUDO, 0, cmd);
		break; }
	case CUDA_CMD_READ_WRITE_I2C:
		// DingusPPC viacuda.cpp @ 92bb6d1 i2c_simple_transaction: a[0] =
		// (7-bit dev addr << 1) | RW (1 = read), a[1..] = write data.  No I2C
		// devices are modeled (M3b stop-rule: attach a device only if the boot
		// demonstrably needs its data) — every transaction takes the oracle's
		// start_transaction-failed path: error_response(CUDA_ERR_I2C).
		// (QEMU @ de5d8bfd does not implement 0x22/0x25 at all — unknown cmd,
		// error 2 — which is what the boot retried 9108x against; the
		// DingusPPC framing names the real condition.)
		if (n < 1) goto bad_args;
		c->cmd_i2c++;
		i2c_latch_addr(c, a[0]);
		c->i2c_absent++;
		warn_latch(c, "Cuda: I2C transaction to absent device (see i2c counters)");
		resp_error(c, CUDA_ERR_I2C, CUDA_PKT_PSEUDO, cmd);
		break;
	case CUDA_CMD_COMB_FMT_I2C:
		// DingusPPC i2c_comb_transaction: a[0] = dev_addr, a[1] = sub_addr,
		// a[2] = dev_addr1 (bits 7:1 must match a[0], bit 0 = RW of the
		// repeated-start phase), a[3..] = write data.  Mismatch errors BEFORE
		// the bus lookup; otherwise the absent-device path as above.  The
		// live 9.0.1 boot issues this once per I2C probe cycle (686x in the
		// first post-0x22 acceptance boot) — boot-demanded, plan rev-2 m2.
		// Deliberate divergence (4e544aff review): the oracle lets a runt 0x25
		// fall through its n>=5 guard to a plain ACK header; we reject as
		// BAD_ARGS (same wire error byte 5, saner semantics).
		if (n < 3) goto bad_args;
		c->cmd_i2c++;
		i2c_latch_addr(c, a[0]);
		if ((a[0] & 0xFE) != (a[2] & 0xFE)) {
			c->cmd_bad_param++;
			resp_error(c, CUDA_ERR_I2C, CUDA_PKT_PSEUDO, cmd);
			break;
		}
		c->i2c_absent++;
		warn_latch(c, "Cuda: I2C transaction to absent device (see i2c counters)");
		resp_error(c, CUDA_ERR_I2C, CUDA_PKT_PSEUDO, cmd);
		break;
	default:
		// Unknown command (ONE_SECOND_MODE 0x1B lands here deliberately, plan
		// m2): well-formed error per both oracles + counter + latched warning.
		c->cmd_unknown++;
		c->last_unknown_type = CUDA_PKT_PSEUDO;
		c->last_unknown_cmd = cmd;
		warn_latch(c, "Cuda: unknown pseudo command (see cmd_unknown counter)");
		resp_error(c, CUDA_ERR_BAD_CMD, CUDA_PKT_PSEUDO, cmd);
		break;
	}
	return;

bad_args:
	// QEMU cuda_receive_packet: handler rejected parameters -> error 0x5.
	c->cmd_bad_param++;
	resp_error(c, CUDA_ERR_BAD_ARGS, CUDA_PKT_PSEUDO, cmd);
}

// --- packet commit (synchronous, C2) ------------------------------------------

static void process_packet(CudaDevice *c)
{
	const uint8_t *p = c->in_buf;
	int len = c->in_count;

	c->packets++;
	if (len < 2) {                               // runt (DingusPPC BAD_SIZE)
		resp_error(c, CUDA_ERR_BAD_SIZE, len ? p[0] : 0, 0);
		goto queued;
	}
	switch (p[0]) {
	case CUDA_PKT_ADB: {
		// QEMU cuda_receive_packet_from_host ADB_PACKET branch + M3b pinned
		// encodings: success [00 00 cmd data...], absent [00 02 cmd].
		uint8_t reply[CUDA_OUT_BUF_SIZE - 3];
		uint8_t cmd = p[1];
		int rlen = -1;
		c->cmd_adb++;
		if (c->adb_fn)
			rlen = c->adb_fn(c->adb_opaque, cmd, p + 2, len - 2,
			                 reply, (int)sizeof(reply));
		if (rlen >= 0) {
			resp_begin(c, CUDA_PKT_ADB, 0x00, cmd);
			resp_append(c, reply, rlen);
		} else {
			c->adb_absent++;
			resp_begin(c, CUDA_PKT_ADB, 0x02, cmd);   // timeout status
		}
		break; }
	case CUDA_PKT_PSEUDO:
		pseudo_command(c, p[1], p + 2, len - 2);
		break;
	default:
		c->cmd_unknown++;
		c->last_unknown_type = p[0];
		c->last_unknown_cmd = p[1];
		warn_latch(c, "Cuda: unsupported packet type");
		resp_error(c, CUDA_ERR_BAD_PKT, p[0], p[1]);
		break;
	}
queued:
	c->responses++;
	c->treq_asserted = 1;       // C2: TREQ asserted before the seam call returns
}

// --- the handshake engine (QEMU cuda_update, restated for our seam) -----------

uint8_t CudaORBWritten(CudaDevice *c, uint8_t orb, uint8_t acr)
{
	uint8_t b = orb & HSK;             // host-driven TACK/TIP image
	uint8_t last = c->last_b & HSK;

	c->orb_writes++;
	if (!(b & CUDA_TIP)) {
		// TIP asserted: transfer in progress.  Byte capture/load is
		// edge-triggered on any TACK/TIP change (M4; QEMU cuda_update
		// "(ms->b & (TACK|TIP)) != (s->last_b & (TACK|TIP))").
		if (b != last) {
			if (acr & ACR_SR_OUT) {
				// host -> Cuda: capture the SR byte
				if (c->in_count < CUDA_IN_BUF_SIZE) {
					c->in_buf[c->in_count++] = c->sr;
					c->bytes_in++;
				} else {
					c->in_overflows++;
				}
				c->sr_int_pending = 1;
			} else if (c->out_pos < c->out_size) {
				// Cuda -> host: load the next response byte into SR;
				// TREQ negates as the LAST byte loads (QEMU lines 138-147).
				c->sr = c->out_buf[c->out_pos++];
				c->bytes_out++;
				if (c->out_pos >= c->out_size)
					c->treq_asserted = 0;
				c->sr_int_pending = 1;
			}
		}
	} else if (last & CUDA_TIP) {
		// TIP negated, was negated: the sync/attention branch (QEMU lines
		// 150-159 — THE CV-0 semantics).  Each TACK change while TIP stays
		// negated mirrors onto TREQ (TACK asserted-low => TREQ asserted-low)
		// and raises the SR int.  This is what terminates the boot's
		// ORB=18338 bit-3 poll.
		if ((b & CUDA_TACK) != (last & CUDA_TACK)) {
			if (b & CUDA_TACK) {
				c->treq_asserted = 0;
			} else {
				c->treq_asserted = 1;
				c->syncs++;
			}
			c->sr_int_pending = 1;
		} else if (c->out_size > 0 && c->out_pos == 0) {
			// idle write with an untouched queued response: keep signalling
			// (QEMU "signal if there is data to read")
			c->treq_asserted = 1;
		}
	} else {
		// TIP negate edge: end of transfer (QEMU lines 161-170).
		if (c->in_count > 0) {
			// Packet commit: process synchronously, queue the response and
			// assert TREQ before returning (C2).
			process_packet(c);
			c->in_count = 0;
		} else if (c->out_pos > 0) {
			// Host finished (or abandoned) reading the response: drop the
			// remainder and negate TREQ (DingusPPC end-of-transaction; QEMU
			// would re-assert TREQ on leftovers, which wedges abandoned
			// open-ended reads).
			c->out_pos = c->out_size = 0;
			c->treq_asserted = 0;
		} else if (c->out_size > 0) {
			// Untouched response still queued: host must come back for it.
			c->treq_asserted = 1;
		}
		c->sr_int_pending = 1;     // QEMU: "always an IRQ at the end of transfer"
	}

	c->last_b = b;
	// CV-10: raises are NOT delivered at the write edge.  sr_int_pending stays
	// latched for CudaSettle on the IFR-read surface — delivering here lets the
	// guest's follow-up SR read consume the int before its IFR poll loop starts
	// (the ROM 0x9584 sync park).  QEMU delays these ints 20us for the same
	// reason (cuda_delay_set_sr_int).
	return CUDA_SEAM_NONE;
}

// --- telemetry formatter (house style: snprintf into caller buffer) -----------

size_t CudaFormatStats(const CudaDevice *c, char *buf, size_t buflen)
{
	if (!buflen) return 0;
	char unk[40];
	if (c->cmd_unknown)
		// carried review follow-up: name the most recent unknown (type:cmd, hex)
		snprintf(unk, sizeof(unk), "%llu(last=%X:%02X)",
		         (unsigned long long)c->cmd_unknown,
		         c->last_unknown_type, c->last_unknown_cmd);
	else
		snprintf(unk, sizeof(unk), "0");
	// i2c_absent=N(addrs=AA,BB,...) — the capture-only probe map (what the
	// boot asked for over I2C; all answered absent until a device is modeled)
	char i2c[64];
	{
		int p = snprintf(i2c, sizeof(i2c), "%llu",
		                 (unsigned long long)c->i2c_absent);
		for (int i = 0; i < c->i2c_addr_count && p < (int)sizeof(i2c) - 5; i++)
			p += snprintf(i2c + p, sizeof(i2c) - (size_t)p, "%s%02X%s",
			              i ? "," : "(addrs=", c->i2c_addrs[i],
			              i == c->i2c_addr_count - 1 ? ")" : "");
	}
	int n = snprintf(buf, buflen,
	    "packets=%llu responses=%llu syncs=%llu bytes_in=%llu bytes_out=%llu "
	    "adb=%llu adb_absent=%llu get_time=%llu set_time=%llu autopoll=%llu "
	    "pram_rd=%llu pram_wr=%llu i2c=%llu i2c_absent=%s "
	    "acked=%llu bad_param=%llu unknown=%s "
	    "resets=%llu powerdowns=%llu overflows=%llu",
	    (unsigned long long)c->packets, (unsigned long long)c->responses,
	    (unsigned long long)c->syncs, (unsigned long long)c->bytes_in,
	    (unsigned long long)c->bytes_out, (unsigned long long)c->cmd_adb,
	    (unsigned long long)c->adb_absent, (unsigned long long)c->cmd_get_time,
	    (unsigned long long)c->cmd_set_time, (unsigned long long)c->cmd_autopoll,
	    (unsigned long long)c->cmd_pram_read, (unsigned long long)c->cmd_pram_write,
	    (unsigned long long)c->cmd_i2c, i2c,
	    (unsigned long long)c->cmd_acked, (unsigned long long)c->cmd_bad_param,
	    unk, (unsigned long long)c->resets_latched,
	    (unsigned long long)c->powerdowns_latched, (unsigned long long)c->in_overflows);
	if (n < 0) return 0;
	return (size_t)n < buflen ? (size_t)n : buflen - 1;
}

// Crash-path telemetry instance (M3b Task 3; VIARegisterDiagInstance pattern).
static CudaDevice *g_diag_cuda;

void CudaRegisterDiagInstance(CudaDevice *c)
{
	g_diag_cuda = c;
}

size_t CudaFormatStatsRegistered(char *buf, size_t buflen)
{
	if (!g_diag_cuda) return 0;
	return CudaFormatStats(g_diag_cuda, buf, buflen);
}
