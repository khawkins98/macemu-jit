/* test_dev_cuda.cpp - M3b Wave 1 Task 1 conformance vectors (plan rev 2, BINDING).
 *
 * CV-0  sync/attention sequence (the live ORB=18338 frontier, boot-observed):
 *       DDRB=0x30, ORB 0x38 then 0x28 — TREQ must assert (derived bit 3 -> 0)
 *       per QEMU cuda_update() sync semantics, and negate when TACK negates.
 * CV-1  fresh device: derived ORB bit 3 == 1 (idle, no TREQ).
 * CV-2  GET_TIME full TIP/TACK/SR choreography; response
 *       [01 00 03 T>>24 T>>16 T>>8 T], TREQ asserted across response bytes and
 *       negated at the last byte.
 * CV-3  ADB Talk routes to the injected mock handler; success framed
 *       [00 00 cmd data...]; absent (-1) framed [00 02 cmd].
 * CV-4  autopoll on + no events => no spontaneous TREQ.
 * Plus per-command table tests (PRAM full-length round-trip, FILE_SERVER_FLAG
 * ack, unknown-command counter + warning latch) and the S3 §1.5 IFR-bit-2
 * poll sequence (RAISE/CLEAR seam flags).
 *
 * Oracle framing cross-checked against QEMU hw/misc/macio/cuda.c
 * @ de5d8bfd6105d3dd3ae668df9762df244a6d1506 and DingusPPC viacuda.cpp
 * @ 92bb6d10549529f9f4031a85c2bc136149535bdc (see dev_cuda.h header). */

#include "dev_cuda.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vector>

static int n_pass = 0;
#define CHECK(cond) do { assert(cond); n_pass++; } while (0)

// Fixed injected Mac time (m8: test injects a fixed value).
static uint32_t fixed_mac_time = 0xCAFE1234u;
static uint32_t fake_now_mac(void *) { return fixed_mac_time; }

// --- mock ADB handler (CV-3): keyboard at addr 2 answers Talk R3; addr 5 absent.
static int mock_calls = 0;
static uint8_t mock_last_cmd = 0;
static std::vector<uint8_t> mock_last_listen;
static int mock_adb(void *, uint8_t cmd, const uint8_t *listen, int listen_len,
                    uint8_t *reply, int reply_max)
{
	mock_calls++;
	mock_last_cmd = cmd;
	mock_last_listen.assign(listen, listen + listen_len);
	uint8_t addr = cmd >> 4;
	if (addr == 5) return -1;                       // absent device
	if ((cmd & 0x0F) == 0x0F) {                     // Talk R3
		assert(reply_max >= 2);
		reply[0] = addr; reply[1] = 0x01;           // [devaddr, handler_id]
		return 2;
	}
	return 0;                                       // success, no data
}

static CudaDevice cuda;

// --- host-side choreography helpers (the 68k engine's view) -------------------
// Track the host's stored ORB bits (4/5 outputs) like the VIA would store them.
static uint8_t host_orb = 0x38;     // idle: TACK+TIP negated (boot-observed)
static uint8_t host_acr = 0x00;

static uint8_t orb_write(uint8_t v) { host_orb = v; return CudaORBWritten(&cuda, v, host_acr); }
static int     treq_bit()           { return (CudaDeriveORB(&cuda, host_orb) >> 3) & 1; }

// Send a complete host->Cuda packet (write SR, assert TIP, toggle TACK per
// byte, negate TIP+TACK to commit).  Asserts the IFR.2 RAISE/CLEAR seam flags
// at every step (the S3 §1.5 IFR-bit-2 poll sequence).
static void send_packet(const uint8_t *data, int len)
{
	host_acr = 0x10;                              // shift out (host->Cuda)
	uint8_t f = CudaSRWritten(&cuda, data[0]);
	CHECK(f & CUDA_SEAM_CLEAR_SR_INT);            // M4: SR access clears IFR.2
	f = orb_write(0x18);                          // assert TIP (TACK negated)
	CHECK(f == CUDA_SEAM_NONE);                   // CV-10: raises are DEFERRED
	CHECK(CudaSettle(&cuda) & CUDA_SEAM_RAISE_SR_INT);  // delivered at IFR poll
	uint8_t tack = CUDA_TACK;
	for (int i = 1; i < len; i++) {
		f = CudaSRWritten(&cuda, data[i]);
		CHECK(f & CUDA_SEAM_CLEAR_SR_INT);
		tack ^= CUDA_TACK;                        // toggle TACK = capture edge
		f = orb_write((uint8_t)(0x08 | tack));    // TIP stays asserted (bit5=0)
		CHECK(f == CUDA_SEAM_NONE);
		CHECK(CudaSettle(&cuda) & CUDA_SEAM_RAISE_SR_INT);
	}
	(void)orb_write(0x38);                        // negate TIP+TACK: commit (C2)
	// "always an IRQ at the end of transfer" (QEMU): drain the deferred raise
	// like the guest's post-commit IFR poll would.
	CHECK(CudaSettle(&cuda) & CUDA_SEAM_RAISE_SR_INT);
}

// Read a full Cuda->host response (assert TIP, read SR, toggle TACK until TREQ
// negates, then negate TIP).  Returns the framed bytes.
static std::vector<uint8_t> read_response()
{
	std::vector<uint8_t> out;
	if (treq_bit() != 0) return out;              // nothing pending
	host_acr = 0x00;                              // shift in (Cuda->host)
	uint8_t f = orb_write(0x18);                  // assert TIP: byte 0 -> SR
	CHECK(f == CUDA_SEAM_NONE);                   // CV-10: raises are DEFERRED
	CHECK(CudaSettle(&cuda) & CUDA_SEAM_RAISE_SR_INT);
	uint8_t tack = CUDA_TACK;
	for (;;) {
		uint8_t b;
		f = CudaSRRead(&cuda, &b);
		CHECK(f & CUDA_SEAM_CLEAR_SR_INT);
		out.push_back(b);
		if (treq_bit() == 1) break;               // TREQ negated at last byte
		tack ^= CUDA_TACK;
		f = orb_write((uint8_t)(0x08 | tack));
		CHECK(f == CUDA_SEAM_NONE);
		CHECK(CudaSettle(&cuda) & CUDA_SEAM_RAISE_SR_INT);
	}
	(void)orb_write(0x38);                        // end transaction
	CHECK(treq_bit() == 1);                       // back to idle
	(void)CudaSettle(&cuda);                      // drain the end-of-transfer raise
	return out;
}

static std::vector<uint8_t> roundtrip(const std::vector<uint8_t> &pkt)
{
	send_packet(pkt.data(), (int)pkt.size());
	CHECK(treq_bit() == 0);                       // C2: response pending at commit
	return read_response();
}

int main()
{
	// ---- CV-1: fresh device, derived ORB bit 3 == 1 (idle) ----
	CudaReset(&cuda, fake_now_mac, 0);
	CHECK(treq_bit() == 1);
	// C1: derivation overrides whatever bit 3 the stored byte carries (RMW-safe)
	CHECK(((CudaDeriveORB(&cuda, 0x00) >> 3) & 1) == 1);
	CHECK((CudaDeriveORB(&cuda, 0x30) & 0x30) == 0x30);   // bits 4/5 pass through

	// ---- CV-0: sync/attention sequence (boot-observed: DDRB=0x30, 0x38, 0x28) ----
	uint8_t f = orb_write(0x38);                  // idle write: no edge, no raise
	CHECK(f == CUDA_SEAM_NONE);
	f = orb_write(0x28);                          // TACK asserted, TIP negated
	// QEMU cuda_update sync semantics: TREQ mirrors TACK while TIP negated.
	CHECK(treq_bit() == 0);                       // the 18338-poll terminates
	CHECK(f == CUDA_SEAM_NONE);                   // CV-10: raise NOT eager
	CHECK(CudaSettle(&cuda) & CUDA_SEAM_RAISE_SR_INT);  // attention ack (SR int)
	CHECK(CudaSettle(&cuda) == CUDA_SEAM_NONE);   // consume-once: no double raise
	f = orb_write(0x38);                          // host negates TACK: sync done
	CHECK(treq_bit() == 1);                       // back to idle
	CHECK(f == CUDA_SEAM_NONE);
	CHECK(CudaSettle(&cuda) & CUDA_SEAM_RAISE_SR_INT);
	CHECK(cuda.syncs == 1);

	// ---- CV-10: deferred SR-int delivery (M3b Wave-1 acceptance frontier) ----
	// ROM 9.0.1 startup sync at 0x9584 (68k, guest 0x50009584): after TACK
	// negate + the sync-byte SR read (0x9602), the ROM waits AGAIN for IFR.2
	// with a 15000-poll budget (0x960e) before declaring the Cuda dead (parks
	// at 0x9754 'bra.b *').  On hardware/QEMU the post-edge SR int arrives
	// ~20us AFTER that SR read (QEMU cuda_delay_set_sr_int, sr_delay_ns).
	// Lazy-model equivalent: edge raises latch in sr_int_pending and deliver
	// ONLY at the IFR-read surface (CudaSettle); SR access must NOT consume an
	// undelivered pending raise.
	f = orb_write(0x28);                          // TACK assert   (ROM 0x95be)
	CHECK(treq_bit() == 0);                       // TREQ-assert poll (0x95c4)
	CHECK(CudaSettle(&cuda) & CUDA_SEAM_RAISE_SR_INT);  // IFR wait #1 (0x95da)
	f = orb_write(0x38);                          // TACK negate   (0x95ec)
	CHECK(treq_bit() == 1);                       // TREQ-negate poll (0x95f2)
	{
		uint8_t sync_byte;
		f = CudaSRRead(&cuda, &sync_byte);        // sync-byte read (0x9602)
		CHECK(f & CUDA_SEAM_CLEAR_SR_INT);        // clears the LATCHED IFR.2 only
	}
	// THE FRONTIER: the 0x960e wait must still see the negate-edge raise.
	CHECK(CudaSettle(&cuda) & CUDA_SEAM_RAISE_SR_INT);
	CHECK(CudaSettle(&cuda) == CUDA_SEAM_NONE);   // consume-once
	CHECK(cuda.syncs == 2);

	// ---- CV-2: GET_TIME full choreography ----
	{
		const uint8_t pkt[] = { CUDA_PKT_PSEUDO, CUDA_CMD_GET_TIME };
		send_packet(pkt, 2);
		CHECK(treq_bit() == 0);                   // C2: TREQ asserted at commit

		// Scripted read with per-byte TREQ checks (asserted across response
		// bytes, negated at the last byte load).
		host_acr = 0x00;
		(void)orb_write(0x18);                    // TIP asserted: byte 0 in SR
		uint8_t expect[7] = { CUDA_PKT_PSEUDO, 0x00, CUDA_CMD_GET_TIME,
		                      (uint8_t)(fixed_mac_time >> 24), (uint8_t)(fixed_mac_time >> 16),
		                      (uint8_t)(fixed_mac_time >> 8),  (uint8_t)fixed_mac_time };
		uint8_t tack = CUDA_TACK;
		for (int i = 0; i < 7; i++) {
			uint8_t b;
			(void)CudaSRRead(&cuda, &b);
			CHECK(b == expect[i]);
			if (i < 6) {
				CHECK(treq_bit() == 0);           // more bytes: TREQ stays low
				tack ^= CUDA_TACK;
				(void)orb_write((uint8_t)(0x08 | tack));
			}
		}
		CHECK(treq_bit() == 1);                   // negated after the last byte
		(void)orb_write(0x38);
		CHECK(treq_bit() == 1);
		CHECK(cuda.cmd_get_time == 1);
	}

	// SET_TIME shifts the GET_TIME result by the delta (QEMU tick_offset model).
	{
		uint32_t t2 = fixed_mac_time + 1000;
		std::vector<uint8_t> pkt = { CUDA_PKT_PSEUDO, CUDA_CMD_SET_TIME,
			(uint8_t)(t2 >> 24), (uint8_t)(t2 >> 16), (uint8_t)(t2 >> 8), (uint8_t)t2 };
		std::vector<uint8_t> r = roundtrip(pkt);
		CHECK(r.size() == 3 && r[0] == CUDA_PKT_PSEUDO && r[1] == 0 && r[2] == CUDA_CMD_SET_TIME);
		r = roundtrip({ CUDA_PKT_PSEUDO, CUDA_CMD_GET_TIME });
		CHECK(r.size() == 7);
		uint32_t got = ((uint32_t)r[3] << 24) | ((uint32_t)r[4] << 16) | ((uint32_t)r[5] << 8) | r[6];
		CHECK(got == t2);
		CHECK(cuda.cmd_set_time == 1);
	}

	// ---- CV-3: ADB packets via the injected mock handler ----
	CudaBindADB(&cuda, mock_adb, 0);
	{
		// Talk R3 at addr 2 (cmd 0x2F): success framing [00 00 2F 02 01]
		std::vector<uint8_t> r = roundtrip({ CUDA_PKT_ADB, 0x2F });
		CHECK(mock_calls == 1 && mock_last_cmd == 0x2F && mock_last_listen.empty());
		CHECK(r.size() == 5);
		CHECK(r[0] == CUDA_PKT_ADB && r[1] == 0x00 && r[2] == 0x2F);
		CHECK(r[3] == 0x02 && r[4] == 0x01);
		CHECK(cuda.cmd_adb == 1);

		// Absent device (addr 5, Talk R0, cmd 0x5C): timeout framing [00 02 5C]
		r = roundtrip({ CUDA_PKT_ADB, 0x5C });
		CHECK(r.size() == 3);
		CHECK(r[0] == CUDA_PKT_ADB && r[1] == 0x02 && r[2] == 0x5C);
		CHECK(cuda.adb_absent == 1);

		// Listen payload passes through to the handler (addr 2 Listen R3 = 0x2B)
		r = roundtrip({ CUDA_PKT_ADB, 0x2B, 0x03, 0xFE });
		CHECK(mock_last_cmd == 0x2B);
		CHECK(mock_last_listen.size() == 2 && mock_last_listen[0] == 0x03 && mock_last_listen[1] == 0xFE);
		CHECK(r.size() == 3 && r[0] == CUDA_PKT_ADB && r[1] == 0x00 && r[2] == 0x2B);
	}

	// ---- CV-4: autopoll on + no events => no spontaneous TREQ ----
	{
		std::vector<uint8_t> r = roundtrip({ CUDA_PKT_PSEUDO, CUDA_CMD_AUTOPOLL, 0x01 });
		CHECK(r.size() == 3 && r[1] == 0x00);
		CHECK(cuda.autopoll_on == 1);
		CHECK(cuda.cmd_autopoll == 1);
		CHECK(treq_bit() == 1);                   // silent: nothing pending
		CHECK(CudaSettle(&cuda) == CUDA_SEAM_NONE);
		CHECK(treq_bit() == 1);                   // still silent after settles
	}

	// ---- per-command table ----
	// SET_AUTO_RATE / GET_AUTO_RATE
	{
		std::vector<uint8_t> r = roundtrip({ CUDA_PKT_PSEUDO, CUDA_CMD_SET_AUTO_RATE, 0x08 });
		CHECK(r.size() == 3 && r[1] == 0);
		r = roundtrip({ CUDA_PKT_PSEUDO, CUDA_CMD_GET_AUTO_RATE });
		CHECK(r.size() == 4 && r[3] == 0x08);
		// rate 0 = bad parameter (QEMU cuda_cmd_set_autorate): error packet
		r = roundtrip({ CUDA_PKT_PSEUDO, CUDA_CMD_SET_AUTO_RATE, 0x00 });
		CHECK(r.size() == 4);
		CHECK(r[0] == CUDA_PKT_ERROR && r[1] == CUDA_ERR_BAD_ARGS);     // QEMU code 5
		CHECK(r[2] == CUDA_PKT_PSEUDO && r[3] == CUDA_CMD_SET_AUTO_RATE);
		CHECK(cuda.cmd_bad_param == 1);
	}
	// SET/GET_DEVICE_LIST
	{
		std::vector<uint8_t> r = roundtrip({ CUDA_PKT_PSEUDO, CUDA_CMD_SET_DEVICE_LIST, 0xA5, 0x5A });
		CHECK(r.size() == 3 && r[1] == 0);
		r = roundtrip({ CUDA_PKT_PSEUDO, CUDA_CMD_GET_DEVICE_LIST });
		CHECK(r.size() == 5 && r[3] == 0xA5 && r[4] == 0x5A);
	}
	// FILE_SERVER_FLAG ack
	{
		std::vector<uint8_t> r = roundtrip({ CUDA_PKT_PSEUDO, CUDA_CMD_FILE_SERVER_FLAG, 0x01 });
		CHECK(r.size() == 3 && r[0] == CUDA_PKT_PSEUDO && r[1] == 0 && r[2] == CUDA_CMD_FILE_SERVER_FLAG);
		CHECK(cuda.file_server_flag == 1);
	}
	// SET_POWER_MESSAGES ack
	{
		std::vector<uint8_t> r = roundtrip({ CUDA_PKT_PSEUDO, CUDA_CMD_SET_POWER_MESSAGES, 0x00 });
		CHECK(r.size() == 3 && r[1] == 0);
	}
	// RESET_SYSTEM / POWERDOWN: ack + latched loud-log, NO action
	{
		(void)CudaTakePendingWarning(&cuda);      // drain any earlier latch
		std::vector<uint8_t> r = roundtrip({ CUDA_PKT_PSEUDO, CUDA_CMD_RESET_SYSTEM });
		CHECK(r.size() == 3 && r[1] == 0);
		CHECK(cuda.resets_latched == 1);
		const char *w = CudaTakePendingWarning(&cuda);
		CHECK(w != 0);
		CHECK(CudaTakePendingWarning(&cuda) == 0);    // one-shot
		r = roundtrip({ CUDA_PKT_PSEUDO, CUDA_CMD_POWERDOWN });
		CHECK(r.size() == 3 && r[1] == 0);
		CHECK(cuda.powerdowns_latched == 1);
	}
	// PRAM full-length round-trip (m9): WRITE_PRAM then READ_PRAM from 0.
	{
		std::vector<uint8_t> wr = { CUDA_PKT_PSEUDO, CUDA_CMD_WRITE_PRAM, 0x00, 0x10,
		                            0xDE, 0xAD, 0xBE, 0xEF };
		std::vector<uint8_t> r = roundtrip(wr);
		CHECK(r.size() == 3 && r[1] == 0);
		CHECK(cuda.cmd_pram_write == 1);
		r = roundtrip({ CUDA_PKT_PSEUDO, CUDA_CMD_READ_PRAM, 0x00, 0x00 });
		CHECK(r.size() == 3 + 256);               // FULL-LENGTH from addr 0
		CHECK(r[0] == CUDA_PKT_PSEUDO && r[1] == 0 && r[2] == CUDA_CMD_READ_PRAM);
		CHECK(r[3 + 0x10] == 0xDE && r[3 + 0x11] == 0xAD);
		CHECK(r[3 + 0x12] == 0xBE && r[3 + 0x13] == 0xEF);
		CHECK(r[3 + 0x14] == 0x00);               // zero-init elsewhere
		// partial read: full-length from addr to end of PRAM
		r = roundtrip({ CUDA_PKT_PSEUDO, CUDA_CMD_READ_PRAM, 0x00, 0x10 });
		CHECK((int)r.size() == 3 + 256 - 0x10);
		CHECK(r[3] == 0xDE);
		// out-of-range address: bad parameter
		r = roundtrip({ CUDA_PKT_PSEUDO, CUDA_CMD_READ_PRAM, 0x01, 0x00 });
		CHECK(r.size() == 4 && r[0] == CUDA_PKT_ERROR && r[1] == CUDA_ERR_BAD_PAR);
	}
	// MCU_MEM variants: PRAM window at 0x100 (DingusPPC)
	{
		std::vector<uint8_t> r = roundtrip({ CUDA_PKT_PSEUDO, CUDA_CMD_WRITE_MCU_MEM,
		                                     0x01, 0x20, 0x77 });
		CHECK(r.size() == 3 && r[1] == 0);
		r = roundtrip({ CUDA_PKT_PSEUDO, CUDA_CMD_READ_MCU_MEM, 0x01, 0x20 });
		CHECK((int)r.size() == 3 + 256 - 0x20);
		CHECK(r[3] == 0x77);
		// and it is the same PRAM array READ_PRAM sees
		r = roundtrip({ CUDA_PKT_PSEUDO, CUDA_CMD_READ_PRAM, 0x00, 0x20 });
		CHECK(r[3] == 0x77);
		// Cuda FW ROM region: version blob (DingusPPC fake, 7 bytes)
		r = roundtrip({ CUDA_PKT_PSEUDO, CUDA_CMD_READ_MCU_MEM, 0x0F, 0x00 });
		CHECK(r.size() == 3 + 7);
	}
	// Unknown pseudo command: error packet + counter + warning latch
	{
		(void)CudaTakePendingWarning(&cuda);
		std::vector<uint8_t> r = roundtrip({ CUDA_PKT_PSEUDO, 0x5E });
		CHECK(r.size() == 4);
		CHECK(r[0] == CUDA_PKT_ERROR && r[1] == CUDA_ERR_BAD_CMD);
		CHECK(r[2] == CUDA_PKT_PSEUDO && r[3] == 0x5E);
		CHECK(cuda.cmd_unknown == 1);
		CHECK(CudaTakePendingWarning(&cuda) != 0);
		// ONE_SECOND_MODE (0x1B) is deliberately absent (plan m2): unknown path
		r = roundtrip({ CUDA_PKT_PSEUDO, 0x1B, 0x01 });
		CHECK(r.size() == 4 && r[0] == CUDA_PKT_ERROR && r[1] == CUDA_ERR_BAD_CMD);
		CHECK(cuda.cmd_unknown == 2);
	}
	// Unknown packet type / runt packet
	{
		std::vector<uint8_t> r = roundtrip({ 0x07, 0x00 });     // bogus type
		CHECK(r.size() == 4 && r[0] == CUDA_PKT_ERROR && r[1] == CUDA_ERR_BAD_PKT);
		CHECK(r[2] == 0x07);
		// 1-byte packet: bad size (DingusPPC CUDA_ERR_BAD_SIZE)
		host_acr = 0x10;
		(void)CudaSRWritten(&cuda, CUDA_PKT_PSEUDO);
		(void)orb_write(0x18);
		(void)orb_write(0x38);                    // commit with in_count == 1
		CHECK(treq_bit() == 0);
		r = read_response();
		CHECK(r.size() >= 2 && r[0] == CUDA_PKT_ERROR && r[1] == CUDA_ERR_BAD_SIZE);
	}
	// Wrong arg count on a strict command (QEMU handler false => error 5)
	{
		std::vector<uint8_t> r = roundtrip({ CUDA_PKT_PSEUDO, CUDA_CMD_GET_TIME, 0xFF });
		CHECK(r.size() == 4 && r[0] == CUDA_PKT_ERROR && r[1] == CUDA_ERR_BAD_ARGS);
		CHECK(r[2] == CUDA_PKT_PSEUDO && r[3] == CUDA_CMD_GET_TIME);
	}
	// Abandoned read: host ends the transaction mid-response — remainder is
	// dropped and TREQ negates (DingusPPC end-of-transaction semantics).
	{
		static const uint8_t gt_pkt[] = { CUDA_PKT_PSEUDO, CUDA_CMD_GET_TIME };
		send_packet(gt_pkt, 2);
		CHECK(treq_bit() == 0);
		host_acr = 0x00;
		(void)orb_write(0x18);                    // read byte 0 only
		uint8_t b; (void)CudaSRRead(&cuda, &b);
		CHECK(b == CUDA_PKT_PSEUDO);
		(void)orb_write(0x38);                    // negate TIP: abandon
		CHECK(treq_bit() == 1);                   // no stuck TREQ
	}
	// Untouched response survives an idle transaction-end (host must come back).
	{
		static const uint8_t gt_pkt[] = { CUDA_PKT_PSEUDO, CUDA_CMD_GET_TIME };
		send_packet(gt_pkt, 2);
		CHECK(treq_bit() == 0);
		std::vector<uint8_t> r = read_response(); // now actually read it
		CHECK(r.size() == 7);
	}
	// Sync arriving MID-pending-response: the TACK-negate edge of the sync
	// clears TREQ, but the next idle ORB write with the untouched queued
	// response must RE-assert it (pins the "keep signalling" branch — QEMU
	// "signal if there is data to read"; carried review follow-up).
	{
		static const uint8_t gt_pkt[] = { CUDA_PKT_PSEUDO, CUDA_CMD_GET_TIME };
		send_packet(gt_pkt, 2);
		CHECK(treq_bit() == 0);                   // response pending, untouched
		(void)orb_write(0x28);                    // sync: TACK asserts, TIP negated
		CHECK(treq_bit() == 0);                   // mirror keeps TREQ low
		(void)orb_write(0x38);                    // sync end: TACK negates -> TREQ clears
		(void)orb_write(0x38);                    // idle write, response still queued
		CHECK(treq_bit() == 0);                   // TREQ re-asserted: host comes back
		std::vector<uint8_t> r = read_response();
		CHECK(r.size() == 7);                     // response survived the sync
		CHECK(treq_bit() == 1);
	}
	// No ADB handler bound => absent framing (defensive)
	{
		CudaBindADB(&cuda, 0, 0);
		std::vector<uint8_t> r = roundtrip({ CUDA_PKT_ADB, 0x2F });
		CHECK(r.size() == 3 && r[0] == CUDA_PKT_ADB && r[1] == 0x02 && r[2] == 0x2F);
		CudaBindADB(&cuda, mock_adb, 0);
	}
	// READ_WRITE_I2C (0x22) — M3b Wave-1 acceptance frontier (boot retried it
	// 9108x as unknown).  Oracle: DingusPPC viacuda.cpp @ 92bb6d1
	// i2c_simple_transaction — packet [01 22 addr data...], addr = (7-bit dev
	// addr << 1) | RW (1 = read).  No I2C devices are modeled (stop-rule), so
	// every transaction is "Unsupported I2C device" => error_response(
	// CUDA_ERR_I2C = 5): reply [02 05 01 22].  (QEMU @ de5d8bfd does NOT
	// implement 0x22 — it would frame error 2 "unknown command"; the DingusPPC
	// framing is the one that names the real failure.)
	{
		(void)CudaTakePendingWarning(&cuda);
		uint64_t unk_before = cuda.cmd_unknown;
		// read probe at 7-bit addr 0x50 (SPD DIMM slot 0 on DingusPPC
		// Yosemite): addr byte = 0x50<<1 | 1 = 0xA1
		std::vector<uint8_t> r = roundtrip({ CUDA_PKT_PSEUDO, CUDA_CMD_READ_WRITE_I2C, 0xA1 });
		CHECK(r.size() == 4);
		CHECK(r[0] == CUDA_PKT_ERROR && r[1] == CUDA_ERR_I2C);
		CHECK(r[2] == CUDA_PKT_PSEUDO && r[3] == CUDA_CMD_READ_WRITE_I2C);
		CHECK(cuda.cmd_i2c == 1 && cuda.i2c_absent == 1);
		CHECK(cuda.cmd_unknown == unk_before);    // implemented: NOT unknown
		CHECK(CudaTakePendingWarning(&cuda) != 0);    // first-touch latch
		// write form (RW bit 0) to another address: same absent framing
		r = roundtrip({ CUDA_PKT_PSEUDO, CUDA_CMD_READ_WRITE_I2C, 0x50, 0x12, 0x34 });
		CHECK(r.size() == 4 && r[0] == CUDA_PKT_ERROR && r[1] == CUDA_ERR_I2C);
		CHECK(cuda.cmd_i2c == 2 && cuda.i2c_absent == 2);
		// runt (no addr byte): bad args (DingusPPC reads in_buf[2] blind; we
		// validate — same wire code 5, distinguished by the counter)
		r = roundtrip({ CUDA_PKT_PSEUDO, CUDA_CMD_READ_WRITE_I2C });
		CHECK(r.size() == 4 && r[0] == CUDA_PKT_ERROR && r[1] == CUDA_ERR_BAD_ARGS);
		CHECK(cuda.cmd_i2c == 2);                 // not counted as an i2c txn
		// capture-only telemetry: distinct raw addr bytes latched (probe map)
		CHECK(cuda.i2c_addr_count == 2);
		CHECK(cuda.i2c_addrs[0] == 0xA1 && cuda.i2c_addrs[1] == 0x50);
		// repeat probe: no duplicate latch
		r = roundtrip({ CUDA_PKT_PSEUDO, CUDA_CMD_READ_WRITE_I2C, 0xA1 });
		CHECK(cuda.i2c_addr_count == 2 && cuda.cmd_i2c == 3);
		// stats surface the i2c counters + the probe map
		char ibuf[512];
		CHECK(CudaFormatStats(&cuda, ibuf, sizeof(ibuf)) > 0);
		CHECK(strstr(ibuf, "i2c=3 i2c_absent=3(addrs=A1,50)") != 0);
	}

	// COMB_FMT_I2C (0x25) — issued by the live boot once per I2C probe cycle
	// (686x in the first post-0x22 acceptance boot).  Oracle: DingusPPC
	// i2c_comb_transaction @ 92bb6d1 — packet [01 25 dev_addr sub_addr
	// dev_addr1 data...]; dev_addr/dev_addr1 must match in bits 7:1 (else
	// CUDA_ERR_I2C), then start_transaction fails for absent devices =>
	// error_response(CUDA_ERR_I2C).  Reply [02 05 01 25].
	{
		uint64_t i2c_before = cuda.cmd_i2c, abs_before = cuda.i2c_absent;
		uint64_t unk_before = cuda.cmd_unknown, bad_before = cuda.cmd_bad_param;
		// combined read: write-addr 0x90, sub-addr 0x00, read-addr 0x91
		std::vector<uint8_t> r = roundtrip({ CUDA_PKT_PSEUDO, CUDA_CMD_COMB_FMT_I2C,
		                                     0x90, 0x00, 0x91 });
		CHECK(r.size() == 4);
		CHECK(r[0] == CUDA_PKT_ERROR && r[1] == CUDA_ERR_I2C);
		CHECK(r[2] == CUDA_PKT_PSEUDO && r[3] == CUDA_CMD_COMB_FMT_I2C);
		CHECK(cuda.cmd_i2c == i2c_before + 1 && cuda.i2c_absent == abs_before + 1);
		CHECK(cuda.cmd_unknown == unk_before);    // implemented: NOT unknown
		// dev_addr mismatch (bits 7:1 differ): oracle errors BEFORE the bus
		// lookup — same wire code, NOT an absent-device count
		r = roundtrip({ CUDA_PKT_PSEUDO, CUDA_CMD_COMB_FMT_I2C, 0x90, 0x00, 0xA1 });
		CHECK(r.size() == 4 && r[0] == CUDA_PKT_ERROR && r[1] == CUDA_ERR_I2C);
		CHECK(cuda.i2c_absent == abs_before + 1);
		CHECK(cuda.cmd_bad_param == bad_before + 1);
		// runt (needs dev_addr, sub_addr, dev_addr1): bad args
		r = roundtrip({ CUDA_PKT_PSEUDO, CUDA_CMD_COMB_FMT_I2C, 0x90 });
		CHECK(r.size() == 4 && r[0] == CUDA_PKT_ERROR && r[1] == CUDA_ERR_BAD_ARGS);
		// counter pin (4e544aff review minor 1): BAD_ARGS == ERR_I2C == 5 on
		// the wire, so deleting the runt guard would route through the
		// stale-in_buf mismatch branch with IDENTICAL bytes — only cmd_i2c
		// can tell (guard rejects BEFORE counting: +2; deleted guard: +3).
		// bad_param: +1 mismatch above, +1 the runt's bad_args path.
		CHECK(cuda.cmd_i2c == i2c_before + 2);
		CHECK(cuda.cmd_bad_param == bad_before + 2);
		// probe map latches the combined address too (raw dev_addr byte)
		char ibuf[512];
		CHECK(CudaFormatStats(&cuda, ibuf, sizeof(ibuf)) > 0);
		CHECK(strstr(ibuf, "90") != 0);
	}

	// Stats formatter (telemetry house style)
	{
		char buf[512];
		CHECK(CudaFormatStats(&cuda, buf, sizeof(buf)) > 0);
		CHECK(strstr(buf, "packets=") != 0);
		// carried review follow-up: unknown=N(last=T:CC) names the most recent
		// unknown (the bogus packet type 0x07 above was the last one).
		CHECK(cuda.cmd_unknown == 3);
		CHECK(strstr(buf, "unknown=3(last=7:00)") != 0);
		CHECK(CudaFormatStats(&cuda, buf, 0) == 0);   // zero-length guard
		// fresh device: plain unknown=0 (no last suffix)
		CudaDevice fresh;
		CudaReset(&fresh, 0, 0);
		CHECK(CudaFormatStats(&fresh, buf, sizeof(buf)) > 0);
		CHECK(strstr(buf, "unknown=0 ") != 0);
	}

	printf("RESULT: ALL PASS (%d checks)\n", n_pass);
	return 0;
}
